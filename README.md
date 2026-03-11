# coke

Disable sleep on macOS.

No root. No kernel extension. No background daemon. One binary.

> [!IMPORTANT]
> USE AT YOUR OWN RISK

## Background

**Clamshell mode** is defined as running a MacBook with the lid closed, an
external display connected, and AC power attached. In clamshell mode, macOS
does not sleep on lid close. Idle sleep from inactivity is not disabled.

**coke** sets the same bit (0x02) via `kPMSetClamshellSleepState`, preventing
sleep on lid close regardless of whether an external display or AC power is
present. It also creates an idle sleep assertion (same as `caffeinate -i`) to
prevent sleep from inactivity in **any** circumstance.

In clamshell mode, the mask is redundant — `powerd` already has the bit set.
The idle sleep assertion still applies (idle sleep is independent of clamshell
mode). Coke's unique value is keeping the machine awake with the lid closed
when you don't have an external display or AC power.

`caffeinate -i` prevents idle sleep but does not prevent sleep on lid close. If
you close the lid without an external display and AC power, `caffeinate` will
not keep the machine awake. coke will.

## Usage

```
coke             # disable sleep (foreground, Ctrl+C to stop)
coke off         # re-enable clamshell sleep (manual cleanup)
coke status      # show current lid/sleep state
```

While `coke` is running, the machine will not sleep — neither from closing
the lid nor from idle timeout. On clean exit (Ctrl+C, SIGTERM, SIGHUP),
the idle sleep assertion is released and normal idle sleep behavior resumes.
The clamshell flag is cleared unless the system is in clamshell mode
(external display + AC), in which case the flag is left intact to avoid
desyncing `powerd` (see **Behavior on exit**).

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

On clean exit, coke checks whether the system is in clamshell mode (external
display connected and AC power attached):

- **Clamshell mode active:** The mask is left intact. `powerd` has the same bit
  set and tracks it internally — clearing it would desync `powerd`'s state.
  `powerd` only re-sends the bit on zero↔non-zero transitions of its internal
  tracking variable, so once desynced, it won't recover until a display or power
  source change.

- **Clamshell mode not active:** The mask is cleared. **If the lid is closed at
  that moment, the machine sleeps immediately.** This is how the XNU kernel
  works: `IOPMrootDomain` evaluates clamshell sleep policy the instant
  `clamshellSleepDisableMask` drops to zero. There is no userspace mechanism to
  clear the flag without triggering this evaluation. Amphetamine (CDMManager)
  uses the same API and has the same behavior.

## How it works

coke prevents sleep through two independent mechanisms:

**Clamshell sleep** (lid close): `IOConnectCallScalarMethod` with selector
`kPMSetClamshellSleepState` on the `IOPMrootDomain` user client sets the
`kClamshellSleepDisablePowerd` bit in the kernel's `clamshellSleepDisableMask`.
The flag is globally sticky — `RootDomainUserClient::clientClose` does not undo
it, so it persists across IOKit connection open/close cycles. coke re-asserts
the flag every second to recover if another tool clears it.

**Idle sleep:** An `IOPMAssertion` of type
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
succeeds, it is the last instance and conditionally resets the clamshell flag
(see **Behavior on exit**). If it fails, other instances still hold `LOCK_SH`,
so the flag stays set.

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

- Clean exit (Ctrl+C, SIGTERM, SIGHUP) releases the idle assertion and
  conditionally clears the clamshell flag (preserved in clamshell mode)
- SIGKILL or crash leaves the clamshell flag set until `coke off` or reboot
- Thermal throttling and emergency shutdown (SMC) remain active
- User-initiated sleep (power button, Apple menu) still works
- State resets on reboot regardless
