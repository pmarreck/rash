# SIGCHLD remainder

The `while` drain and the direct-dispatch `SIG_INPROGRESS` fix are on master. Evidence: `BUGFIX_REPRO_REPORTS/2026-08-04-sigchld-discarded-on-direct-dispatch.md`. The independent 2026-07-31 verdict against `be829983` was incorporated; that commit is not the accepted fix.

Still open, and not a patch to invent from a guess:

- A fourth loss, about 1 in 1000 under load, where a child is reaped and the firing is neither queued nor run. It is documented, not in the suite, and not patched.
- `run-trapchld` can flake with one lost SIGCHLD (expected 0, got 1). Retry when that is the only failure.
- Async-signal-unsafe `malloc` in the trap handler stays unfixed. Do not mail an upstream patch.

Reports stay in `BUGFIX_REPRO_REPORTS/`.
