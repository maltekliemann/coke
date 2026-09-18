# coke

Keep a MacBook working with its lid closed, from the terminal.

No root. No kernel extension. No persistent background daemon. One binary.

Coke prevents idle system sleep and requests a lid-close sleep override. The
lid-close mechanism shares state with macOS and has limitations: power/display
changes can interrupt protection, and killing Coke can leave the override set.

## Usage

```sh
coke                         # run until Ctrl+C
coke -- make                 # protect a build until make exits
coke -- sh -c 'make && make test'
coke off                     # restore lid-close policy after an interrupted session
coke status                  # reported lid policy and running Coke instances
```

With `coke -- command args`, protection is established before starting the
command. Coke passes arguments directly, without shell expansion, and preserves
stdin, stdout, stderr, the environment, and the working directory. Use an explicit
shell for pipelines, redirections inside the command, or shell builtins.

Coke waits for the command, then restores sleep policy. It returns the command's
exit code, or `128 + signal` if the command dies from a signal. If the command
succeeds but protection or cleanup fails, Coke returns `1`. An executable that
cannot be found returns `127`; other launch errors return `126`. A failure to
establish protection prevents the command from starting.

Ctrl+C reaches the foreground command. SIGINT, SIGTERM, or SIGHUP sent to Coke
are forwarded to the command's process group; Coke keeps protection until the
command exits, including while its signal handler finishes. A command that ignores
those signals can keep running. Interactive terminal input and Ctrl+Z/foreground
resume are supported. Protection follows the direct command's lifetime, not
background jobs it leaves behind. Run long-lived work in the foreground.

`coke off` refuses to run while any cooperating Coke instance is active, including
instances under other user accounts. It is a recovery command, not a command to
stop active sessions.

## Install

### Homebrew

```sh
brew tap maltekliemann/coke
brew install coke
```

### From source

```sh
make
make install
```

Requires macOS and Xcode command line tools (`xcode-select --install`).
Use `make install PREFIX=...` to choose the installation directory.

Stop older Coke binaries before using this version. Older versions use per-user
locks and do not participate in the machine-wide coordination described below.

## Sleep behavior and limitations

Coke uses two independent mechanisms:

- **Idle sleep:** a process-owned `PreventUserIdleSystemSleep` assertion, as used
  by `caffeinate -i`. macOS releases this assertion when Coke exits or dies.
- **Lid-close sleep:** `kPMSetClamshellSleepState` on `IOPMrootDomain` sets the
  global `kClamshellSleepDisablePowerd` bit. This permits closed-lid operation
  without requiring an external display or AC power. Closing the IOKit connection
  or killing the process does not reset this bit.

Coke retries the lid-close request once per second. This is **best effort**:
`powerd` or another sleep utility can clear the same bit. If the lid is closed,
macOS can initiate sleep immediately, before Coke gets another chance to run.
Unplugging a charger or display is an important case to test on your hardware.
A faster retry cannot remove this race. The process-owned lid-close assertion
requires an Apple entitlement unavailable to an ordinary CLI.

Implementation references: Apple's
[root-domain sleep policy](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/Kernel/IOPMrootDomain.cpp)
and [powerd assertion handling and entitlement checks](https://github.com/apple-oss-distributions/PowerManagement/blob/main/pmconfigd/PMAssertions.c).

Coke does not disable display sleep. User-requested sleep, critical battery
conditions, and thermal protection can still cause sleep or shutdown. Avoid
leaving a running, closed laptop in a bag or other unventilated space.

If a runtime lid-close request fails, Coke reports it. Command mode keeps waiting
for the command and retrying, then reports failure even if the command succeeds.
Standalone mode exits with failure and attempts cleanup.

### Restoring sleep

The last exiting instance and `coke off` use the same restoration policy:

- If an external display and AC power are present, or an active macOS assertion
  carries a lid-close/hot-plug modifier, leave the shared bit intact.
- If environment queries fail, leave the bit intact and return an error rather
  than claim cleanup succeeded. Retry `coke off` after resolving the error.
- Otherwise clear the bit and report whether the operation succeeded.

**Clearing the bit can immediately put the Mac to sleep if the lid is closed.**
The restoration checks are a best-effort approximation of macOS policy, not an
atomic ownership API. They cannot coordinate with every private `powerd` state,
future battery-powered desktop mode, or unrelated sleep utility. Avoid running
multiple different tools that modify clamshell sleep at the same time.

After a Coke crash or SIGKILL, the override may remain until `coke off`, a macOS
policy change, or reboot. If only the wrapped command crashes, the surviving Coke
process still performs cleanup. Reboot resets the override regardless.

`coke status` shows the OS's reported lid policy, which may be a stale snapshot;
it does not prove that Coke owns an override or that future lid closure cannot
sleep. The running-instance check includes all users of this version.

### Coordination

Each instance holds a shared `flock()` on
`/private/tmp/com.maltekliemann.coke.sessions.lock`. A separate exclusive lock on
`/private/tmp/com.maltekliemann.coke.control.lock` serializes startup, shutdown,
status, and recovery. In particular, each exiting instance closes its session
lock before allowing the next cleanup to proceed, so simultaneous exits cannot
all skip restoration.

Both files are empty and mode `0444`, allowing other accounts to open and lock
them without write access or root. Symlinks, non-regular files, hard links, and
unexpected permissions are rejected. Lock descriptors are closed on exec, so
wrapped commands do not inherit them. Do not delete the lock files while Coke
is running: replacing an inode defeats advisory locking. This protocol
coordinates cooperating users; it is not a security boundary against the file
owner or root. macOS releases file locks when a process dies.

## Tests

```sh
make test
```

The tests replace power-management calls and exercise the real CLI, macOS file
locks, process execution, signals, and terminal handling. They cover simultaneous
exits, restoration policy, injected API failures, command exit codes and streams,
and unsafe lock files. They do not change the machine's sleep settings.

Physical validation is still needed on each supported macOS/hardware combination:
check lid closure on AC and battery, with and without a display; disconnect power
and displays while the lid is closed; check manual sleep/wake, command completion,
Ctrl+C, and recovery after killing Coke. Record which configurations pass rather
than treating compilation or simulated tests as hardware verification.
