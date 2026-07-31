# Rash divergences from GNU Bash

Rash is a fork of GNU Bash 5.3. Compatibility is a constraint, not an
aspiration: every place Rash behaves differently from upstream is recorded
here, with the evidence that justified it and the test that pins it.

An entry belongs in this file only if it changes **observable behavior**.
Build-system, packaging, CI, and documentation-only changes do not.

Each entry must state: what upstream does, what Rash does, why, whether POSIX
is implicated, which test proves it, and whether it has been reported
upstream. An entry with no test is not a divergence, it is a rumor.

---

## 1. `--pretty-print` does not execute a `-c` command

**Upstream behavior.** `bash --pretty-print -c 'cmd'` **executes** `cmd` and
never prints it. The same flag on a script file or on standard input prints the
parsed command and does not execute it.

**Rash behavior.** `--pretty-print` prints and does not execute on *every*
input path, including `-c`. When `-c` is used, Rash additionally writes
`warning: -c command was printed, not executed` to standard error.

**Why.**

1. *An optimization must not change semantics.* The divergence between input
   paths is not a design decision — it is a side effect of a performance
   switch. `config-top.h:34-36` defines `ONESHOT` solely as fork avoidance:
   *"Define ONESHOT if you want sh -c 'command' to avoid forking to execute
   `command' whenever possible. This is a big efficiency improvement."* With
   `ONESHOT` defined (the default), the `-c` branch in `shell.c` calls
   `run_one_command` and exits before `pretty_print_mode` is ever consulted.
   Build the identical source with `ONESHOT` undefined and `--pretty-print -c`
   prints without executing. A fork-avoidance flag decides whether your command
   runs.

2. *Bash already rules that do-not-execute beats `-c`.* `bash -n -c 'cmd'` does
   not execute `cmd`. `-n` is enforced at both parse loops —
   `builtins/evalstring.c:455` for the `-c` path and `eval.c:149` for the
   reader loop — so it covers every input route. `pretty_print_mode` is
   enforced at one site only, which is why `-c` slips past. This is a missing
   enforcement site, not a deliberate carve-out.

3. *Printing and executing cannot share standard output.* Both the printed
   representation and the command's own output go to stdout. Combining them
   produces interleaved, unparseable output, so the combination has no coherent
   meaning to preserve.

4. *The silence is the hazard.* Upstream warns when it ignores this same flag
   for interactive shells but is completely silent when it ignores it for `-c`.
   A caller who believes they are inspecting a command instead runs it, with
   full effect. This is not hypothetical: it is how the flag was encountered.

**Implementation.** The `ONESHOT` fast path in `shell.c` is skipped when a
printing mode is active, falling through to `with_input_from_string` and the
reader loop — which is precisely what a non-`ONESHOT` build has always done for
`-c`. No new parsing or printing machinery was introduced; the two build
configurations were brought back into agreement. The `read_and_execute:` label
is no longer conditional, since the `goto` now exists in both configurations.

**POSIX.** Not implicated. `--pretty-print` is a GNU extension: it is absent
from POSIX, and (upstream) absent from `doc/bash.1` and `doc/bashref.texi`
entirely, appearing only in `--help` output. `-c` and `-n` are POSIX and are
unchanged. No exit status changes. Rash documents the option in `doc/bash.1`,
which upstream never did.

**Tests.** `tests/prettyprint.tests`, `tests/prettyprint.right`,
`tests/run-prettyprint`. All input routes are asserted as a set — `-c`,
`-n -c`, standard input, and script file — so the option is checked as a
classifier over invocation forms rather than only the route that changed.

**Upstream status.** Not yet reported. The `ONESHOT` semantic defect is
reportable on its own terms, with no reference to Rash, and should go to
`bug-bash@gnu.org` from a pristine clone. The stderr diagnostic is a Rash
choice and is not part of that report. Tracked in `PLAN.md`.
