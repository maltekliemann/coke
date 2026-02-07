# coke

Disable sleep on macOS. Keeps your MacBook awake when the lid is closed,
without requiring a power adapter, external display, or keyboard/mouse.

No root. No kernel extension. No background daemon. One binary.

> [!IMPORTANT]
> USE AT YOUR OWN RISK

## Usage

```
coke             # disable clamshell sleep (foreground, Ctrl+C to stop)
coke off         # re-enable clamshell sleep (manual cleanup)
coke status      # show current lid/sleep state
```

While `coke` is running, closing the MacBook lid will not trigger sleep.
When `coke` exits — for any reason — clamshell sleep is re-enabled and
normal lid-close behavior resumes.

## Install

### Homebrew

```
brew tap maltekliemann/coke
brew install coke
```

### From source

```
make
make install
```

Requires Xcode command line tools (`xcode-select --install`).

## Behavior on exit

When coke stops, the kernel flag is always reset. **If the lid is closed at
that moment, the machine sleeps immediately.** This is not a coke limitation.
It is how the XNU kernel works: the `IOPMrootDomain` evaluates clamshell sleep
policy the instant the `clamshellSleepDisableMask` drops to zero. There is no
userspace mechanism to clear the flag without triggering this evaluation.
Amphetamine (the Mac App Store app that offers the same feature via its
CDMManager) has identical behavior.

## Multiple instances

Multiple `coke` processes can run simultaneously. Clamshell sleep stays
disabled as long as at least one instance is alive. The kernel flag is only
reset when the last instance exits.

Coordination uses `flock()` shared/exclusive locks on a per-user temp file.
If a `coke` process crashes or is killed with `SIGKILL`, the lock is
automatically released by the OS, and the next `coke` startup detects and
cleans the orphaned kernel state.

## How it works

coke calls `IOConnectCallScalarMethod` with selector `kPMSetClamshellSleepState`
on the `IOPMrootDomain` user client. This sets the `kClamshellSleepDisablePowerd`
bit in the kernel's `clamshellSleepDisableMask`. The flag is globally sticky —
`RootDomainUserClient::clientClose` does not undo it, so it persists across
IOKit connection open/close cycles.

No root or entitlement is required. The `IOPMrootDomain` user client has
`kIOUserClientEntitlementsKey = false` and the `kPMSetClamshellSleepState`
selector has no `clientHasPrivilege` gate. This is the same API that
Amphetamine uses from the App Store sandbox.

coke re-asserts the flag every second to recover if another tool clears it.

## Safety

> [!IMPORTANT]
> By using `coke`, you accept full responsibility for any consequences.

- Kernel state is **always** reset on exit (no stale flags left behind)
- Thermal throttling and emergency shutdown (SMC) remain active
- Normal sleep paths (idle timer, power button, Apple menu) are unaffected
- State resets on reboot regardless
- Crash recovery via `flock`: next startup cleans orphaned state
