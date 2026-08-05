# Upstream bug reproduction reports

Defects found in GNU Bash itself, written so they can be sent upstream
unchanged. Each report stands on its own: it must not reference Rash, Nix, or
anything else local, because none of that is upstream's problem.

Bash does not accept pull requests. `bminor/bash` on GitHub is a read-only
mirror; Chet Ramey maintains the tree and reads `bug-bash@gnu.org`. Send these
as a plain-text message body or attachment to that address.

Before sending, reproduce against a **pristine** clone of upstream Bash — this
repository carries local changes that would contaminate the result.

Each report states the observed behavior, a minimal reproduction, the measured
rate if intermittent, the source-level cause where known, and — kept explicitly
separate — any suggested fix.

## The SIGCHLD series (three fixes, one contract)

`sigchld-fixes.patch` carries all three against a tree with none of them, and
is verified end to end: applied to a pristine checkout it takes
`tests/trapchld` from 9-of-30, 6-of-1000, and 416-of-1000 failures to zero,
with the complete upstream test suite passing. Read the reports in order —
each mechanism was only visible once the louder one above it was closed.

1. `2026-07-31-sigchld-lost-trap-firings.md` — the drain runs once and
   `continue`s past SIGCHLD, so firings queued during the trap are never
   revisited.
2. `2026-08-04-sigchld-discarded-on-direct-dispatch.md` — the direct dispatch
   from `waitchld` never sets `SIG_INPROGRESS`, so a recursive
   `run_pending_traps` takes the bad-handler branch and discards the backlog.
3. `2026-08-05-sigchld-lost-update-drain-window.md` — the drain's
   read-then-clear races the handler's increment, zeroing it uncaptured.

`2026-08-05-sigchld-async-signal-unsafe-allocation.md` is a fourth defect on
the same path, reported but **not** fixed: `waitchld` allocates when entered
from the signal handler, which corrupts the heap. It is architectural and
belongs to the maintainer. It is unrelated to the three fixes and none of them
depend on it.
