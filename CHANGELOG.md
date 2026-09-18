# Changelog

## 0.4.0

- Add `coke -- command args` to hold sleep protection for a command's lifetime.
  Preserve command arguments, streams, environment, working directory, and exit
  status. Forward termination signals and support interactive terminal jobs.
- Coordinate instances across user accounts and serialize shutdown so concurrent
  exits cannot all skip restoration.
- Use the same restoration policy for normal exit and `coke off`, preserving
  detected macOS clamshell policy and reporting unknown state or failed cleanup.
- Refuse to launch a command if sleep protection cannot be established. Report
  runtime and cleanup failures without replacing a command's nonzero exit code.
- Treat `status` as an OS policy snapshot and report missing properties as errors.
- Add behavioral tests covering processes, terminals, coordination, and simulated
  power-management failures.

Stop older Coke processes before upgrading: their per-user locks do not
coordinate with this version's machine-wide locks.

No root or persistent daemon is required. Power/display transitions can still
interrupt lid-close protection, killing Coke can leave its global override set,
and restoration remains a best-effort approximation of macOS policy. Physical
lid-close and power-transition behavior has not been verified for this release.
