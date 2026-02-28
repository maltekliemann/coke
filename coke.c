/*
 * coke - CLI tool to disable sleep on macOS
 *
 * Keeps your MacBook awake — whether the lid is open or closed —
 * without requiring a power adapter, external display, or keyboard/mouse.
 *
 * Usage:
 *   coke          Run in foreground, disable sleep
 *   coke off      Re-enable clamshell sleep (manual cleanup)
 *   coke status   Show current state
 *   coke version  Show version
 *
 * On clean exit (Ctrl+C, SIGTERM, SIGHUP), the clamshell flag is
 * reset and the idle assertion released. If the lid is closed at
 * that moment, the machine will go to sleep immediately — a laptop
 * left awake in a bag with no display is a thermal hazard.
 *
 * On crash or SIGKILL, the idle assertion is auto-released by the
 * kernel, but the clamshell flag remains set until coke off or
 * reboot.
 *
 * This behavior is a consequence of the kernel design: the XNU
 * IOPMrootDomain evaluates clamshell sleep policy inline with the
 * flag change (setClamShellSleepDisable → handlePowerNotification →
 * shouldSleepOnClamshellClosed). There is no userspace mechanism to
 * clear the flag without triggering this evaluation. Amphetamine
 * (CDMManager) uses the same API and has the same behavior.
 *
 * How it works:
 *   Two mechanisms prevent sleep:
 *
 *   1. Clamshell sleep (lid close): IOKit API kPMSetClamshellSleepState
 *      sets the kernel's clamshellSleepDisableMask (bit
 *      kClamshellSleepDisablePowerd). The flag is globally sticky —
 *      RootDomainUserClient::clientClose does not undo it, so it
 *      persists across connection open/close. No root or entitlement
 *      required (kIOUserClientEntitlementsKey = false, no
 *      clientHasPrivilege gate on this selector). Re-asserted every
 *      second to recover if another tool clears it.
 *
 *   2. Idle sleep (lid open): An IOPMAssertion of type
 *      kIOPMAssertionTypePreventUserIdleSystemSleep (same as
 *      caffeinate -i). Per-process, auto-released by the kernel on
 *      process death — no coordination needed, no orphan risk.
 *
 * Multi-instance coordination:
 *   flock() shared/exclusive locks on a per-user temp file ensure
 *   the kernel flag is only cleared when the last instance exits.
 *   Crash recovery: flock is auto-released on process death.
 *   The clamshell flag may remain set until coke off or reboot.
 *
 * Safety:
 *   - Clean exit (Ctrl+C, SIGTERM, SIGHUP) always resets kernel state
 *   - SIGKILL or crash leaves the clamshell flag set until coke off or reboot
 *   - Thermal throttling and emergency shutdown (SMC) remain active
 *   - User-initiated sleep (power button, Apple menu) still works
 *   - State resets automatically on reboot
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

#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/pwr_mgt/IOPMLibDefs.h>
#include <CoreFoundation/CoreFoundation.h>

#define LOCK_FILENAME "coke.lock"
#define LOCK_PATH_MAX 256

/*
 * Build the lock file path using confstr(_CS_DARWIN_USER_TEMP_DIR),
 * which queries the kernel for the per-user temp directory rather than
 * trusting the environment ($TMPDIR can be poisoned). Returns NULL on
 * failure rather than falling back to /tmp (world-writable, DoS-prone).
 *
 * The per-user temp dir on macOS (e.g. /var/folders/xx/.../T/) is
 * owned by the user and not world-writable.
 */
static char lock_path[LOCK_PATH_MAX];

static const char *get_lock_path(void)
{
    if (lock_path[0] != '\0')
        return lock_path;

    char tmpdir[LOCK_PATH_MAX];
    size_t len = confstr(_CS_DARWIN_USER_TEMP_DIR, tmpdir, sizeof(tmpdir));

    /* confstr returns total size including NUL. len==0 means error,
     * len==1 means empty string — reject both to prevent a size_t
     * underflow in the strlen(tmpdir)-1 trailing-slash check below. */
    if (len <= 1 || len > sizeof(tmpdir)) {
        fprintf(stderr, "coke: confstr(_CS_DARWIN_USER_TEMP_DIR) failed; "
                "cannot determine safe temp directory\n");
        return NULL;
    }

    int n = snprintf(lock_path, sizeof(lock_path), "%s%s%s",
                     tmpdir,
                     (tmpdir[strlen(tmpdir) - 1] == '/') ? "" : "/",
                     LOCK_FILENAME);

    if (n < 0 || (size_t)n >= sizeof(lock_path)) {
        fprintf(stderr, "coke: lock path too long\n");
        lock_path[0] = '\0'; /* prevent stale truncated path on next call */
        return NULL;
    }

    return lock_path;
}

/* ---------- IOKit clamshell API ---------- */

/*
 * Toggle the kernel's clamshellSleepDisabled flag via IOPMrootDomain.
 *
 * Opens a user client connection to IOPMrootDomain (the top-level power
 * management driver) and calls selector kPMSetClamshellSleepState.
 * This is idempotent — calling with the same value is a no-op in the
 * kernel. No root or entitlement required (see file header).
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
 * Returns 0 on success, -1 if IOPMrootDomain is not found.
 */
static int get_clamshell_state(bool *lid_closed, bool *causes_sleep)
{
    io_service_t service = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("IOPMrootDomain"));
    if (service == IO_OBJECT_NULL)
        return -1;

    if (lid_closed) {
        if (!read_bool_property(service, CFSTR("AppleClamshellState"), lid_closed))
            *lid_closed = false;
    }

    if (causes_sleep) {
        if (!read_bool_property(service, CFSTR("AppleClamshellCausesSleep"), causes_sleep))
            *causes_sleep = true; /* default: lid close causes sleep */
    }

    IOObjectRelease(service);
    return 0;
}

/* ---------- flock-based coordination ---------- */

/*
 * Locking protocol
 * ================
 * Multiple coke instances coordinate through a single lock file using
 * flock() shared and exclusive locks:
 *
 *   Running instance:  holds LOCK_SH for its entire lifetime.
 *   Clean shutdown:    tries to upgrade LOCK_SH → LOCK_EX (non-blocking).
 *                      If it succeeds, this was the last instance — reset
 *                      the kernel flag. If it fails, other instances are
 *                      still running — do nothing.
 *   Crash:             flock is released automatically when a process dies
 *                      (fd closed by kernel). The clamshell flag remains
 *                      set until coke off or reboot.
 *
 * Key invariant: a running instance ALWAYS holds at least LOCK_SH.
 * This guarantees that LOCK_EX succeeds only when no instances are alive.
 */

static int lock_fd = -1;
static IOPMAssertionID idle_assertion = kIOPMNullAssertionID;

/*
 * Try to acquire an exclusive lock (non-blocking).
 * Returns 1 if acquired (no other instances alive), 0 if not.
 */
static int try_exclusive_lock(int fd)
{
    return flock(fd, LOCK_EX | LOCK_NB) == 0;
}

/*
 * Acquire a shared lock (blocking, but shared locks don't block each other).
 */
static int acquire_shared_lock(int fd)
{
    return flock(fd, LOCK_SH) == 0;
}

/*
 * Open the lock file safely:
 *   - O_NOFOLLOW: refuse to follow symlinks
 *   - 0600: user-only permissions
 *   - Verify the opened file is a regular file we own
 */
static int open_lock_file(void)
{
    const char *path = get_lock_path();
    if (!path)
        return -1;
    int fd = open(path, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (fd < 0) {
        fprintf(stderr, "coke: failed to open lock file %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    /* Verify the file is a regular file owned by us with safe permissions */
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid()) {
        fprintf(stderr, "coke: lock file %s has unexpected owner or type\n", path);
        close(fd);
        return -1;
    }
    if ((st.st_mode & 0777) != 0600) {
        /* Someone created the file with lax permissions — refuse it */
        fprintf(stderr, "coke: lock file %s has unsafe permissions %04o (expected 0600)\n",
                path, st.st_mode & 0777);
        close(fd);
        return -1;
    }

    return fd;
}

/*
 * Called on clean exit (Ctrl+C, SIGTERM, SIGHUP, or atexit).
 *
 * The two sleep mechanisms have different cleanup needs:
 *   - Idle assertion: per-process, released here but also auto-released
 *     by the kernel on crash — no orphan risk.
 *   - Clamshell flag: global, shared across instances. Only the last
 *     instance (the one that successfully upgrades to LOCK_EX) resets it.
 */
static void cleanup_on_exit(void)
{
    if (lock_fd < 0)
        return;

    /* Release idle sleep assertion (per-process, no coordination needed) */
    if (idle_assertion != kIOPMNullAssertionID) {
        IOPMAssertionRelease(idle_assertion);
        idle_assertion = kIOPMNullAssertionID;
    }

    /*
     * Try to upgrade our shared lock to exclusive (non-blocking).
     * On macOS/BSD flock: if other shared holders exist, this fails
     * with EWOULDBLOCK and our shared lock stays in place. If we're
     * the only holder, it atomically upgrades — no gap, no race.
     */
    if (try_exclusive_lock(lock_fd)) {
        set_clamshell_sleep_disabled(false);
        fprintf(stderr, "coke: last instance exiting, clamshell sleep re-enabled\n");
    }

    close(lock_fd);
    lock_fd = -1;
}

/* ---------- Signal handling ---------- */

/*
 * Signal strategy: set a flag and let the main loop exit gracefully.
 * The handler only writes to a volatile sig_atomic_t (async-signal-safe).
 * sleep(1) in the main loop returns early on EINTR, so the flag is
 * checked within ~1 second of signal delivery.
 *
 * SIGKILL cannot be caught — in that case the OS closes our fd
 * (releasing the flock). The clamshell flag remains set until
 * coke off or reboot.
 */
static volatile sig_atomic_t should_exit = 0;

static void handle_signal(int sig)
{
    (void)sig;
    should_exit = 1;
}

static void install_signal_handlers(void)
{
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
}

/* ---------- Commands ---------- */

/*
 * Main entry point. Acquires a shared lock, disables clamshell sleep,
 * creates an idle sleep assertion, then loops re-asserting the clamshell
 * flag every second until signaled. On exit, releases the assertion and
 * (if last instance) resets the clamshell flag.
 */
static int cmd_run(void)
{
    lock_fd = open_lock_file();
    if (lock_fd < 0)
        return 1;

    /* Register cleanup so it runs on exit() from any code path (e.g.,
     * a library calling exit(), or a future early-return we forget to
     * guard). cleanup_on_exit is idempotent (checks lock_fd < 0), so
     * the explicit call at the end of cmd_run + this atexit is safe. */
    atexit(cleanup_on_exit);
    install_signal_handlers();

    if (!acquire_shared_lock(lock_fd)) {
        if (!should_exit) {
            fprintf(stderr, "coke: failed to acquire shared lock: %s\n",
                    strerror(errno));
        }
        close(lock_fd);
        lock_fd = -1;
        return should_exit ? 0 : 1;
    }

    /* Disable clamshell sleep */
    IOReturn ret = set_clamshell_sleep_disabled(true);
    if (ret != kIOReturnSuccess) {
        close(lock_fd);
        lock_fd = -1;
        return 1;
    }

    /* Prevent idle system sleep (like caffeinate -i).
     * Each process holds its own assertion — the kernel auto-releases
     * it if the process dies, so no multi-instance coordination needed. */
    ret = IOPMAssertionCreateWithName(
        kIOPMAssertionTypePreventUserIdleSystemSleep,
        kIOPMAssertionLevelOn,
        CFSTR("coke: preventing idle sleep"),
        &idle_assertion);
    if (ret != kIOReturnSuccess) {
        fprintf(stderr, "coke: warning: failed to create idle sleep assertion: 0x%x\n", ret);
        /* Non-fatal: clamshell sleep prevention still works */
    }

    fprintf(stderr, "coke: sleep disabled (pid %d)\n", getpid());
    fprintf(stderr, "coke: press Ctrl+C to stop\n");

    /* Re-assert every second to recover if another tool clears the flag */
    while (!should_exit) {
        sleep(1);
        if (!should_exit)
            set_clamshell_sleep_disabled(true);
    }

    fprintf(stderr, "\ncoke: shutting down...\n");
    cleanup_on_exit();
    return 0;
}

/*
 * Manually re-enable clamshell sleep. Refuses to run while instances
 * are active (they would re-assert within 1 second, making it futile).
 * Holds LOCK_EX across the IOKit call to prevent a new instance from
 * starting and re-disabling between our check and our disable.
 */
static int cmd_off(void)
{
    int fd = open_lock_file();
    if (fd < 0) {
        fprintf(stderr, "coke: warning: cannot check for running instances\n");
    } else if (!try_exclusive_lock(fd)) {
        close(fd);
        fprintf(stderr, "coke: instances are still running — stop them first (kill or Ctrl+C)\n");
        return 1;
    }
    /* Hold exclusive lock (if acquired) while we disable — prevents a new
     * instance from starting and calling set_clamshell_sleep_disabled(true)
     * between our check and our disable call. */

    IOReturn ret = set_clamshell_sleep_disabled(false);

    if (fd >= 0)
        close(fd); /* release exclusive lock */

    if (ret != kIOReturnSuccess)
        return 1;

    fprintf(stderr, "coke: clamshell sleep re-enabled\n");
    return 0;
}

/*
 * Print current lid state, clamshell sleep override, and whether any
 * coke instances are running. Uses the lock file to detect instances:
 * if LOCK_EX succeeds, no instances hold LOCK_SH, so none are alive.
 */
static int cmd_status(void)
{
    bool lid_closed, causes_sleep;
    if (get_clamshell_state(&lid_closed, &causes_sleep) < 0) {
        fprintf(stderr, "coke: failed to read clamshell state\n");
        return 1;
    }

    /* Check if any coke instances are running. Probe with a non-blocking
     * exclusive lock. Briefly holds LOCK_EX until close() — a concurrent
     * coke startup's acquire_shared_lock would block for this duration. */
    int fd = open_lock_file();
    bool instances_running = false;
    if (fd >= 0) {
        if (!try_exclusive_lock(fd))
            instances_running = true;
        close(fd);
    }

    printf("lid:              %s\n", lid_closed ? "closed" : "open");
    printf("clamshell sleep:  %s\n", causes_sleep ? "enabled (normal)" : "disabled (overridden)");
    printf("coke running:     %s\n", instances_running ? "yes" : "no");

    return 0;
}

/* ---------- Main ---------- */

static void usage(void)
{
    fprintf(stderr,
        "coke - disable sleep on macOS\n"
        "\n"
        "usage:\n"
        "  coke             disable sleep (foreground, Ctrl+C to stop)\n"
        "  coke off         re-enable clamshell sleep\n"
        "  coke status      show current state\n"
        "  coke version     show version\n"
        "  coke help        show this message\n"
    );
}

int main(int argc, char *argv[])
{
    if (argc < 2)
        return cmd_run();

    if (strcmp(argv[1], "off") == 0)
        return cmd_off();
    if (strcmp(argv[1], "status") == 0)
        return cmd_status();
    if (strcmp(argv[1], "version") == 0 ||
        strcmp(argv[1], "--version") == 0 ||
        strcmp(argv[1], "-v") == 0) {
        printf("coke %s\n", COKE_VERSION);
        return 0;
    }
    if (strcmp(argv[1], "help") == 0 ||
        strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "-h") == 0) {
        usage();
        return 0;
    }

    fprintf(stderr, "coke: unknown command '%s'\n", argv[1]);
    usage();
    return 1;
}
