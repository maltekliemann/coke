# coke

Disable sleep on macOS. Keeps your MacBook awake — whether the lid is open
or closed — without requiring a power adapter, external display, or
keyboard/mouse.

No root. No kernel extension. No background daemon. One binary.

> [!IMPORTANT]
> USE AT YOUR OWN RISK

## Usage

```
coke             # disable sleep (foreground, Ctrl+C to stop)
coke off         # re-enable clamshell sleep (manual cleanup)
coke status      # show current lid/sleep state
```

While `coke` is running, the machine will not sleep — neither from closing
the lid nor from idle timeout. On clean exit (Ctrl+C, SIGTERM, SIGHUP),
clamshell sleep is re-enabled, the idle sleep assertion is released, and
normal sleep behavior resumes.

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

On clean exit, the clamshell sleep flag is reset. **If the lid is closed at that
moment, the machine sleeps immediately.** This is not a coke limitation.
It is how the XNU kernel works: the `IOPMrootDomain` evaluates clamshell sleep
policy the instant the `clamshellSleepDisableMask` drops to zero. There is no
userspace mechanism to clear the flag without triggering this evaluation.
Amphetamine (the Mac App Store app that offers the same feature via its
CDMManager) has identical behavior.

## How it works

coke prevents sleep through two independent mechanisms:

**Clamshell sleep** (lid close): `IOConnectCallScalarMethod` with selector
`kPMSetClamshellSleepState` on the `IOPMrootDomain` user client sets the
`kClamshellSleepDisablePowerd` bit in the kernel's `clamshellSleepDisableMask`.
The flag is globally sticky — `RootDomainUserClient::clientClose` does not undo
it, so it persists across IOKit connection open/close cycles. coke re-asserts
the flag every second to recover if another tool clears it.

**Idle sleep** (lid open): An `IOPMAssertion` of type
`kIOPMAssertionTypePreventUserIdleSystemSleep` prevents the system from sleeping
due to inactivity. This is the same mechanism `caffeinate -i` uses. The assertion
is per-process and automatically released by the kernel if the process dies.

No root or entitlement is required. The `IOPMrootDomain` user client has
`kIOUserClientEntitlementsKey = false` and the `kPMSetClamshellSleepState`
selector has no `clientHasPrivilege` gate. This is the same API that
Amphetamine uses from the App Store sandbox.

### Multi-instance coordination

Multiple instances coordinate via `flock()` on a per-user lock file. Each
running instance holds `LOCK_SH` (shared lock) for its lifetime. On exit, an
instance tries a non-blocking upgrade to `LOCK_EX` (exclusive). If the upgrade
succeeds, it is the last instance and resets the clamshell flag. If it fails,
other instances still hold `LOCK_SH`, so the flag stays set.

The idle assertion needs no coordination — each process holds its own, and the
kernel releases it automatically on process death.

### Crash behavior

On `SIGKILL` or crash, the kernel closes the process's file descriptors
(releasing the flock) and releases the idle assertion. The clamshell flag,
however, is a global kernel flag with no process ownership — it remains set
until `coke off` or reboot.

## Safety

> [!IMPORTANT]
> By using `coke`, you accept full responsibility for any consequences.

- Clean exit (Ctrl+C, SIGTERM, SIGHUP) always resets kernel state
- SIGKILL or crash leaves the clamshell flag set until `coke off` or reboot
- Thermal throttling and emergency shutdown (SMC) remain active
- User-initiated sleep (power button, Apple menu) still works
- State resets on reboot regardless
