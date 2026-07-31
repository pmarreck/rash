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
