/*
 * coke - CLI tool to disable clamshell sleep on macOS
 *
 * Keeps your MacBook awake when the lid is closed, without requiring
 * a power adapter, external display, or keyboard/mouse.
 *
 * Usage:
 *   coke          Run in foreground, disable clamshell sleep
 *   coke off      Re-enable clamshell sleep (manual cleanup)
 *   coke status   Show current clamshell state
 *   coke version  Show version
 *
 * When coke exits (Ctrl+C, SIGTERM, crash, or any other reason), it
 * always re-enables clamshell sleep. If the lid is closed at that
 * moment, the machine will go to sleep immediately. This is
 * intentional — a laptop left awake in a bag with no display is a
 * thermal hazard.
 *
 * This behavior is a consequence of the kernel design: the XNU
 * IOPMrootDomain evaluates clamshell sleep policy inline with the
 * flag change (setClamShellSleepDisable → handlePowerNotification →
 * shouldSleepOnClamshellClosed). There is no userspace mechanism to
 * clear the flag without triggering this evaluation. Amphetamine
 * (CDMManager) uses the same API and has the same behavior.
 *
 * How it works:
 *   IOKit API kPMSetClamshellSleepState sets the kernel's
 *   clamshellSleepDisableMask (bit kClamshellSleepDisablePowerd).
 *   The flag is globally sticky — RootDomainUserClient::clientClose
 *   does not undo it, so it persists across connection open/close.
 *   No root or entitlement required (kIOUserClientEntitlementsKey =
 *   false, no clientHasPrivilege gate on this selector).
 *
 * Multi-instance coordination:
 *   flock() shared/exclusive locks on a per-user temp file ensure
 *   the kernel flag is only cleared when the last instance exits.
 *   Crash recovery: flock is auto-released on process death; the
 *   next startup detects orphaned state via an exclusive lock probe.
 *
 * Safety:
 *   - Kernel state is ALWAYS reset on exit (no stale flags)
 *   - Thermal throttling and emergency shutdown (SMC) remain active
 *   - Normal sleep (idle, power button, Apple menu) still works
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
 *   Startup cleanup:   briefly holds LOCK_EX to detect orphaned kernel
 *                      state (crashed instance left the flag set), then
 *                      atomically downgrades to LOCK_SH.
 *   Clean shutdown:    tries to upgrade LOCK_SH → LOCK_EX (non-blocking).
 *                      If it succeeds, this was the last instance — reset
 *                      the kernel flag. If it fails, other instances are
 *                      still running — do nothing.
 *   Crash recovery:    flock is released automatically when a process dies
 *                      (fd closed by kernel). The next startup's exclusive
 *                      lock probe detects the orphaned state.
 *
 * Key invariant: a running instance ALWAYS holds at least LOCK_SH.
 * This guarantees that LOCK_EX succeeds only when no instances are alive.
 *
 * Lock transitions (all atomic on macOS/BSD flock):
 *   LOCK_EX → LOCK_SH  (downgrade, cleanup_orphaned_state)
 *   LOCK_SH → LOCK_EX  (upgrade, cleanup_on_exit — non-blocking)
 */

static int lock_fd = -1;

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
 * Check if we're the only instance. If so, clean up any orphaned
 * kernel state from a previous crash, then downgrade to a shared lock.
 */
static void cleanup_orphaned_state(int fd)
{
    if (try_exclusive_lock(fd)) {
        /* No other instances alive. Clean up stale state. */
        set_clamshell_sleep_disabled(false);
        /* Atomic downgrade to shared — no gap where no lock is held */
        if (flock(fd, LOCK_SH) != 0) {
            fprintf(stderr, "coke: warning: failed to downgrade lock: %s\n",
                    strerror(errno));
        }
    }
}

/*
 * Called on clean exit. If we're the last instance, re-enable
 * clamshell sleep.
 */
static void cleanup_on_exit(void)
{
    if (lock_fd < 0)
        return;

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
 * (releasing the flock), and the next startup cleans up via
 * cleanup_orphaned_state().
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

    /* Clean up orphaned state from a previous crash.
     * If we got the exclusive lock, it's already downgraded to shared. */
    cleanup_orphaned_state(lock_fd);

    /* Take our shared lock (no-op if cleanup_orphaned_state already holds one) */
    if (!acquire_shared_lock(lock_fd)) {
        fprintf(stderr, "coke: failed to acquire shared lock: %s\n",
                strerror(errno));
        close(lock_fd);
        lock_fd = -1;
        return 1;
    }

    /* Disable clamshell sleep */
    IOReturn ret = set_clamshell_sleep_disabled(true);
    if (ret != kIOReturnSuccess) {
        close(lock_fd);
        lock_fd = -1;
        return 1;
    }

    fprintf(stderr, "coke: clamshell sleep disabled (pid %d)\n", getpid());
    fprintf(stderr, "coke: press Ctrl+C to stop\n");

    install_signal_handlers();

    /*
     * Re-assert every second. This ensures:
     *   - Recovery if another tool flips the flag behind our back
     *   - The shared lock is held as long as this fd is open
     */
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
    if (fd >= 0) {
        if (!try_exclusive_lock(fd)) {
            close(fd);
            fprintf(stderr, "coke: instances are still running — stop them first (kill or Ctrl+C)\n");
            return 1;
        }
        /* Hold exclusive lock while we disable — prevents a new instance
         * from starting and calling set_clamshell_sleep_disabled(true)
         * between our check and our disable call. */
    }

    IOReturn ret = set_clamshell_sleep_disabled(false);

    if (fd >= 0)
        close(fd); /* release exclusive lock */

    if (ret != kIOReturnSuccess)
        return 1;

    fprintf(stderr, "coke: clamshell sleep re-enabled\n");
    return 0;
}

static int cmd_status(void)
{
    bool lid_closed, causes_sleep;
    if (get_clamshell_state(&lid_closed, &causes_sleep) < 0) {
        fprintf(stderr, "coke: failed to read clamshell state\n");
        return 1;
    }

    /* Check if any coke instances are running.
     * Probe with a non-blocking exclusive lock. If we get it, downgrade
     * to shared immediately — this prevents a concurrent cmd_run from
     * grabbing exclusive in cleanup_orphaned_state() until our close(). */
    int fd = open_lock_file();
    bool instances_running = false;
    if (fd >= 0) {
        if (try_exclusive_lock(fd)) {
            /* No instances running. Downgrade to shared so the lock is
             * held right up until close(), narrowing the race window. */
            if (flock(fd, LOCK_SH) != 0) {
                fprintf(stderr, "coke: warning: failed to downgrade lock: %s\n",
                        strerror(errno));
            }
        } else {
            instances_running = true;
        }
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
        "coke - disable clamshell sleep on macOS\n"
        "\n"
        "usage:\n"
        "  coke             disable clamshell sleep (foreground, Ctrl+C to stop)\n"
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
