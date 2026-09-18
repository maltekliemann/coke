/*
 * coke - prevent idle sleep and request a lid-close sleep override on macOS.
 *
 * The idle assertion belongs to this process; the clamshell bit does not.
 * Machine-wide locks coordinate cooperating coke processes, but cannot make
 * the shared bit atomic with powerd. Power/display transitions can still
 * initiate sleep before our next retry. See README.md for these limitations.
 */

#ifndef COKE_VERSION
#define COKE_VERSION "dev"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <spawn.h>

#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/pwr_mgt/IOPMLibDefs.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/ps/IOPowerSources.h>

extern char **environ;

/* Read-only lock files allow every account to participate without root.
 * Never unlink them: replacing an inode would split the set of lock holders.
 * The fixed, root-owned sticky directory prevents other users removing them.
 * As with all advisory locks, the creator/root must not remove active files. */
static const char *control_path = "/private/tmp/com.maltekliemann.coke.control.lock";
static const char *sessions_path = "/private/tmp/com.maltekliemann.coke.sessions.lock";

/* ---------- IOKit clamshell API ---------- */

/*
 * Toggle the powerd bit in clamshellSleepDisableMask via IOPMrootDomain.
 *
 * Opens a user client connection to IOPMrootDomain (the top-level power
 * management driver) and calls selector kPMSetClamshellSleepState.
 * This is idempotent — calling with the same value is a no-op in the
 * kernel. No root or entitlement required.
 */
static IOReturn set_clamshell_sleep_disabled(bool disable)
{
    io_service_t service = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("IOPMrootDomain"));
    if (service == IO_OBJECT_NULL) {
        fprintf(stderr, "coke: failed to find IOPMrootDomain\n");
        return kIOReturnNotFound;
    }

    io_connect_t connection = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "coke: IOServiceOpen failed: 0x%x\n", kr);
        return kr;
    }

    uint64_t input = disable ? 1 : 0;
    uint32_t output_count = 0;
    IOReturn ret = IOConnectCallScalarMethod(
        connection, kPMSetClamshellSleepState,
        &input, 1, NULL, &output_count);

    IOServiceClose(connection);

    if (ret != kIOReturnSuccess) {
        fprintf(stderr, "coke: IOConnectCallScalarMethod failed: 0x%x\n", ret);
    }
    return ret;
}

/*
 * Read a boolean property from IOPMrootDomain.
 * Returns 1 on success, 0 if the property is missing or not a CFBoolean.
 */
static int read_bool_property(io_service_t service, CFStringRef key, bool *out)
{
    CFTypeRef val = IORegistryEntryCreateCFProperty(
        service, key, kCFAllocatorDefault, 0);
    if (!val)
        return 0;

    if (CFGetTypeID(val) != CFBooleanGetTypeID()) {
        CFRelease(val);
        return 0;
    }

    *out = CFBooleanGetValue((CFBooleanRef)val);
    CFRelease(val);
    return 1;
}

/*
 * Query the current clamshell state from IOPMrootDomain.
 *   lid_closed:   AppleClamshellState      — is the lid physically closed?
 *   causes_sleep: AppleClamshellCausesSleep — will closing the lid sleep?
 * Either output pointer may be NULL to skip that query.
 * Returns 0 on success, -1 if the service or a requested property is absent.
 */
static int get_clamshell_state(bool *lid_closed, bool *causes_sleep)
{
    io_service_t service = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("IOPMrootDomain"));
    if (service == IO_OBJECT_NULL)
        return -1;

    int result = 0;
    if (lid_closed && !read_bool_property(service, CFSTR("AppleClamshellState"), lid_closed))
        result = -1;
    if (causes_sleep && !read_bool_property(service, CFSTR("AppleClamshellCausesSleep"), causes_sleep))
        result = -1;
    IOObjectRelease(service);
    return result;
}

/* ---------- Restoration policy ---------- */

/* Return 1/0 for yes/no, -1 if the environment cannot be queried. */
static int has_external_display(void)
{
    uint32_t count = 0;
    if (CGGetOnlineDisplayList(0, NULL, &count) != kCGErrorSuccess)
        return -1;
    if (!count)
        return 0;
    CGDirectDisplayID *displays = calloc(count, sizeof(*displays));
    if (!displays)
        return -1;
    int result = 0;
    if (CGGetOnlineDisplayList(count, displays, &count) != kCGErrorSuccess) {
        result = -1;
    } else {
        for (uint32_t i = 0; i < count; i++) {
            if (!CGDisplayIsBuiltin(displays[i]))
                result = 1;
        }
    }
    free(displays);
    return result;
}

static int is_on_ac_power(void)
{
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (!info)
        return -1;
    CFStringRef source = IOPSGetProvidingPowerSourceType(info);
    int result = source ? CFEqual(source, CFSTR(kIOPMACPowerKey)) : -1;
    CFRelease(info);
    return result;
}

static void check_lid_assertions(const void *key, const void *value, void *context)
{
    (void)key;
    int *result = context;
    if (CFGetTypeID(value) != CFArrayGetTypeID()) {
        if (*result != 1)
            *result = -1;
        return;
    }
    CFArrayRef assertions = value;
    for (CFIndex i = 0; i < CFArrayGetCount(assertions); i++) {
        CFDictionaryRef assertion = CFArrayGetValueAtIndex(assertions, i);
        if (CFGetTypeID(assertion) != CFDictionaryGetTypeID()) {
            if (*result != 1)
                *result = -1;
            continue;
        }
        CFTypeRef level = CFDictionaryGetValue(assertion, kIOPMAssertionLevelKey);
        int active = 0;
        if (!level || CFGetTypeID(level) != CFNumberGetTypeID() ||
            !CFNumberGetValue(level, kCFNumberIntType, &active)) {
            if (*result != 1)
                *result = -1;
            continue;
        }
        /* Read the private property names without creating privileged assertions. */
        if (active &&
            (CFDictionaryGetValue(assertion, CFSTR("AppliesOnLidClose")) == kCFBooleanTrue ||
             CFDictionaryGetValue(assertion, CFSTR("ProcessingHotPlug")) == kCFBooleanTrue))
            *result = 1;
    }
}

static int restore_clamshell_sleep(void)
{
    int external = has_external_display();
    int ac = is_on_ac_power();
    int lid_assertion = 0;
    CFDictionaryRef assertions = NULL;
    IOReturn ret = IOPMCopyAssertionsByProcess(&assertions);
    if (ret == kIOReturnSuccess && assertions &&
        CFGetTypeID(assertions) == CFDictionaryGetTypeID()) {
        CFDictionaryApplyFunction(assertions, check_lid_assertions, &lid_assertion);
    } else {
        lid_assertion = -1;
    }
    if (assertions)
        CFRelease(assertions);

    if ((external == 1 && ac == 1) || lid_assertion == 1) {
        fprintf(stderr, "coke: macOS clamshell policy active; override left intact\n");
        return 0;
    }
    if (external < 0 || ac < 0 || lid_assertion < 0) {
        fprintf(stderr, "coke: cannot determine macOS clamshell policy; override left intact; retry coke off\n");
        return 1;
    }
    if (set_clamshell_sleep_disabled(false) != kIOReturnSuccess) {
        fprintf(stderr, "coke: failed to restore clamshell sleep; retry coke off\n");
        return 1;
    }
    fprintf(stderr, "coke: clamshell sleep override cleared\n");
    return 0;
}

/* ---------- Machine-wide coordination ---------- */

static int control_fd = -1;
static int sessions_fd = -1;
static bool session_started;
static IOPMAssertionID idle_assertion = kIOPMNullAssertionID;

static int open_lock_file(const char *path)
{
    /* Explicitly set mode despite umask so another account can open the file.
     * There is no data in either file. Locks work on read-only descriptors. */
    mode_t previous_umask = umask(0);
    int fd = open(path, O_CREAT | O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0444);
    int saved_errno = errno;
    umask(previous_umask);
    errno = saved_errno;
    if (fd < 0) {
        fprintf(stderr, "coke: cannot open lock %s: %s\n", path, strerror(errno));
        return -1;
    }
    struct stat st, named;
    if (fstat(fd, &st) < 0 || lstat(path, &named) < 0 ||
        !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        (st.st_mode & 07777) != 0444 ||
        st.st_dev != named.st_dev || st.st_ino != named.st_ino) {
        fprintf(stderr, "coke: unsafe or replaced lock file: %s\n", path);
        close(fd);
        return -1;
    }
    return fd;
}

static int lock_control(void)
{
    int ret;
    do {
        ret = flock(control_fd, LOCK_EX);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0)
        fprintf(stderr, "coke: cannot acquire control lock: %s\n", strerror(errno));
    return ret;
}

/* Return 1 if exclusive, 0 if another holder exists, -1 on an actual error. */
static int try_exclusive_lock(int fd)
{
    if (flock(fd, LOCK_EX | LOCK_NB) == 0)
        return 1;
    if (errno == EWOULDBLOCK)
        return 0;
    fprintf(stderr, "coke: cannot inspect session lock: %s\n", strerror(errno));
    return -1;
}

static void close_locks(void)
{
    if (sessions_fd >= 0)
        close(sessions_fd);
    if (control_fd >= 0)
        close(control_fd);
    sessions_fd = control_fd = -1;
}

static int open_locks(void)
{
    control_fd = open_lock_file(control_path);
    if (control_fd < 0)
        return 1;
    sessions_fd = open_lock_file(sessions_path);
    if (sessions_fd < 0) {
        close_locks();
        return 1;
    }
    return 0;
}

static int cleanup_session(void)
{
    int failed = 0;
    if (sessions_fd >= 0 && session_started) {
        if (lock_control() < 0) {
            fprintf(stderr, "coke: cleanup could not coordinate; override may remain set\n");
            failed = 1;
        } else {
            /* Serialize the upgrade AND close: concurrent exits cannot both
             * fail their upgrades while each still holds a shared lock. Starts,
             * status and off also acquire control before touching sessions. */
            int last = try_exclusive_lock(sessions_fd);
            if (last == 1)
                failed = restore_clamshell_sleep();
            else if (last < 0)
                failed = 1;
        }
    }
    session_started = false;
    close_locks(); /* closes sessions before releasing control */
    if (idle_assertion != kIOPMNullAssertionID) {
        IOReturn ret = IOPMAssertionRelease(idle_assertion);
        idle_assertion = kIOPMNullAssertionID;
        if (ret != kIOReturnSuccess) {
            fprintf(stderr, "coke: idle assertion release failed: 0x%x\n", ret);
            failed = 1;
        }
    }
    return failed;
}

static void cleanup_on_exit(void)
{
    (void)cleanup_session();
}

static int start_session(void)
{
    if (open_locks() || lock_control() < 0)
        return 1;
    if (flock(sessions_fd, LOCK_SH) < 0) {
        fprintf(stderr, "coke: cannot acquire session lock: %s\n", strerror(errno));
        return 1;
    }
    /* Do not change global state unless the idle assertion succeeded. */
    IOReturn ret = IOPMAssertionCreateWithName(
        kIOPMAssertionTypePreventUserIdleSystemSleep, kIOPMAssertionLevelOn,
        CFSTR("coke: preventing idle sleep"), &idle_assertion);
    if (ret != kIOReturnSuccess) {
        fprintf(stderr, "coke: cannot prevent idle sleep: 0x%x\n", ret);
        return 1;
    }
    if (set_clamshell_sleep_disabled(true) != kIOReturnSuccess)
        return 1;
    session_started = true;
    if (flock(control_fd, LOCK_UN) < 0) {
        fprintf(stderr, "coke: cannot release control lock: %s\n", strerror(errno));
        return 1;
    }
    fprintf(stderr, "coke: idle sleep prevented; lid-close override requested (pid %d)\n", getpid());
    return 0;
}

/* ---------- Signals and command supervision ---------- */

static volatile sig_atomic_t pending_signals;
static sigset_t watched_signals;
static sigset_t original_mask;
static const int forwarded_signals[] = { SIGINT, SIGTERM, SIGHUP };

static void handle_signal(int sig)
{
    pending_signals |= (sig_atomic_t)(1U << sig);
}

static int install_signal_handlers(void)
{
    sigemptyset(&watched_signals);
    for (size_t i = 0; i < sizeof(forwarded_signals) / sizeof(forwarded_signals[0]); i++)
        sigaddset(&watched_signals, forwarded_signals[i]);
    sigaddset(&watched_signals, SIGCHLD);
    struct sigaction sa = {0};
    sa.sa_handler = handle_signal;
    sa.sa_mask = watched_signals;
    for (int sig = 1; sig < NSIG; sig++) {
        if (!sigismember(&watched_signals, sig))
            continue;
        struct sigaction old;
        if (sigaction(sig, NULL, &old) < 0)
            return 1;
        /* Preserve nohup/background-shell signal dispositions. SIGCHLD must
         * be catchable so we can reap and return the command's status. */
        if (old.sa_handler == SIG_IGN && sig != SIGCHLD)
            continue;
        if (sigaction(sig, &sa, NULL) < 0)
            return 1;
    }
    return sigprocmask(SIG_SETMASK, NULL, &original_mask) < 0;
}

static int status_code(int status)
{
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static int set_foreground(int tty, pid_t group)
{
    sigset_t block, previous;
    sigemptyset(&block);
    sigaddset(&block, SIGTTOU);
    if (sigprocmask(SIG_BLOCK, &block, &previous) < 0)
        return -1;
    int result = tcsetpgrp(tty, group);
    int saved_errno = errno;
    sigprocmask(SIG_SETMASK, &previous, NULL);
    errno = saved_errno;
    return result;
}

/* Only hand off a terminal owned by the expected group. A job resumed with
 * bg must not steal the foreground terminal from the shell. */
static int move_foreground(int tty, pid_t from, pid_t to)
{
    if (tty < 0)
        return 0;
    pid_t foreground = tcgetpgrp(tty);
    if (foreground < 0)
        return -1;
    return foreground == from ? set_foreground(tty, to) : 0;
}

static int run_command(char **command, pid_t *child, int *tty)
{
    posix_spawnattr_t attr;
    int error = posix_spawnattr_init(&attr);
    if (error)
        goto fail;
    /* A separate group lets signals sent to coke reach the command's children.
     * Start suspended so an interactive command cannot read before handoff. */
    error = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP |
        POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_START_SUSPENDED);
    if (!error)
        error = posix_spawnattr_setpgroup(&attr, 0);
    if (!error)
        error = posix_spawnattr_setsigmask(&attr, &original_mask);
    if (!error)
        error = posix_spawnp(child, command[0], NULL, &attr, command, environ);
    posix_spawnattr_destroy(&attr);
    if (error)
        goto fail;

    int status;
    pid_t waited;
    do {
        waited = waitpid(*child, &status, WUNTRACED);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        fprintf(stderr, "coke: cannot wait for command startup: %s\n", strerror(errno));
        kill(-*child, SIGKILL);
        while (waitpid(*child, NULL, 0) < 0 && errno == EINTR) {}
        *child = -1;
        return 1;
    }
    if (!WIFSTOPPED(status)) {
        *child = -1;
        return status_code(status);
    }

    *tty = open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (move_foreground(*tty, getpgrp(), *child) < 0) {
        fprintf(stderr, "coke: cannot give command the terminal: %s\n", strerror(errno));
        kill(-*child, SIGKILL);
        while (waitpid(*child, NULL, 0) < 0 && errno == EINTR) {}
        *child = -1;
        return 1;
    }
    kill(-*child, SIGCONT);
    return 0;
fail:
    fprintf(stderr, "coke: cannot execute %s: %s\n", command[0], strerror(error));
    return error == ENOENT ? 127 : 126;
}

static int supervise(pid_t child, int tty)
{
    int failed = 0;
    if (sigprocmask(SIG_BLOCK, &watched_signals, NULL) < 0)
        return 1;
    for (;;) {
        sig_atomic_t pending = pending_signals;
        pending_signals = 0;
        for (size_t i = 0; i < sizeof(forwarded_signals) / sizeof(forwarded_signals[0]); i++) {
            int sig = forwarded_signals[i];
            if (pending & (sig_atomic_t)(1U << sig)) {
                if (child < 0) {
                    sigprocmask(SIG_SETMASK, &original_mask, NULL);
                    return 128 + sig;
                }
                kill(-child, sig);
                kill(-child, SIGCONT); /* let a stopped child handle termination */
            }
        }
        if (child > 0) {
            int status;
            pid_t result = waitpid(child, &status, WNOHANG | WUNTRACED);
            if (result < 0 && errno != EINTR) {
                fprintf(stderr, "coke: waitpid failed: %s\n", strerror(errno));
                sigprocmask(SIG_SETMASK, &original_mask, NULL);
                return 1;
            }
            if (result > 0) {
                if (WIFSTOPPED(status)) {
                    if (move_foreground(tty, child, getpgrp()) < 0)
                        failed = 1;
                    /* Let the invoking shell observe the stopped job. */
                    raise(SIGSTOP);
                    if (move_foreground(tty, getpgrp(), child) < 0)
                        failed = 1;
                    kill(-child, SIGCONT);
                } else {
                    int code = status_code(status);
                    sigprocmask(SIG_SETMASK, &original_mask, NULL);
                    return code ? code : failed;
                }
            }
        }
        struct timespec timeout = { .tv_sec = 1 };
        int result = pselect(0, NULL, NULL, NULL, &timeout, &original_mask);
        if (result < 0 && errno != EINTR) {
            fprintf(stderr, "coke: cannot wait for signals: %s\n", strerror(errno));
            sigprocmask(SIG_SETMASK, &original_mask, NULL);
            return 1;
        }
        if (result == 0 && set_clamshell_sleep_disabled(true) != kIOReturnSuccess) {
            if (!failed)
                fprintf(stderr, "coke: lid-close protection failed; %s\n",
                        child < 0 ? "stopping" : "continuing to retry");
            failed = 1;
            if (child < 0) {
                sigprocmask(SIG_SETMASK, &original_mask, NULL);
                return 1;
            }
        }
    }
}

static int cmd_run(char **command)
{
    if (atexit(cleanup_on_exit) != 0 || install_signal_handlers()) {
        fprintf(stderr, "coke: cannot install cleanup or signal handlers\n");
        return 1;
    }
    int code = start_session();
    pid_t child = -1;
    int tty = -1;
    if (!code) {
        if (pending_signals) {
            for (size_t i = 0; i < sizeof(forwarded_signals) / sizeof(forwarded_signals[0]); i++) {
                int sig = forwarded_signals[i];
                if (pending_signals & (sig_atomic_t)(1U << sig)) {
                    code = 128 + sig;
                    break;
                }
            }
        }
        if (!code && command)
            code = run_command(command, &child, &tty);
        if (!code && (!command || child > 0))
            code = supervise(child, tty);
    }
    if (tty >= 0) {
        if (move_foreground(tty, child, getpgrp()) < 0) {
            fprintf(stderr, "coke: cannot restore terminal foreground group: %s\n", strerror(errno));
            if (!code)
                code = 1;
        }
        close(tty);
    }
    if (cleanup_session() && !code)
        code = 1;
    return code;
}

static int cmd_off(void)
{
    if (open_locks() || lock_control() < 0) {
        close_locks();
        return 1;
    }
    int exclusive = try_exclusive_lock(sessions_fd);
    int code = 1;
    if (exclusive == 0)
        fprintf(stderr, "coke: instances are still running; stop them first\n");
    else if (exclusive == 1)
        code = restore_clamshell_sleep();
    close_locks();
    return code;
}

static int cmd_status(void)
{
    bool lid_closed, causes_sleep;
    if (get_clamshell_state(&lid_closed, &causes_sleep) < 0) {
        fprintf(stderr, "coke: failed to read clamshell state\n");
        return 1;
    }
    if (open_locks() || lock_control() < 0) {
        close_locks();
        return 1;
    }
    int exclusive = try_exclusive_lock(sessions_fd);
    close_locks();
    if (exclusive < 0)
        return 1;
    printf("lid:                  %s\n", lid_closed ? "closed" : "open");
    printf("lid sleep (reported): %s\n", causes_sleep ? "enabled" : "disabled");
    printf("coke running:         %s (all users)\n", exclusive ? "no" : "yes");
    printf("The reported lid policy is an OS snapshot, not proof of an active override.\n");
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "coke - prevent idle sleep and request a lid-close sleep override\n\n"
        "usage:\n"
        "  coke                  run until interrupted\n"
        "  coke -- command args  run a command with sleep protection\n"
        "  coke off              restore clamshell policy (no active instances)\n"
        "  coke status           show reported lid policy and active instances\n"
        "  coke version          show version\n"
        "  coke help             show this message\n");
}

int main(int argc, char *argv[])
{
    if (argc == 1)
        return cmd_run(NULL);
    if (strcmp(argv[1], "--") == 0) {
        if (argc > 2)
            return cmd_run(&argv[2]);
        fprintf(stderr, "coke: expected a command after --\n");
        return 2;
    }
    if (argc != 2) {
        fprintf(stderr, "coke: unexpected arguments; use coke -- command args\n");
        return 2;
    }
    if (strcmp(argv[1], "off") == 0)
        return cmd_off();
    if (strcmp(argv[1], "status") == 0)
        return cmd_status();
    if (strcmp(argv[1], "version") == 0 || strcmp(argv[1], "--version") == 0 ||
        strcmp(argv[1], "-v") == 0) {
        printf("coke %s\n", COKE_VERSION);
        return 0;
    }
    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }
    fprintf(stderr, "coke: unknown command '%s'; use coke -- command args\n", argv[1]);
    return 2;
}
