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

---

## 2. `--emit-ast` writes the parse tree as JSON

**Upstream behavior.** No such option exists.

**Rash behavior.** `--emit-ast` parses commands from a script file, standard
input, or `-c`, writes each as one line of JSON on standard output, and exits
without executing. It follows `--pretty-print` exactly on the `-c` question:
printed, not run, with the non-execution announced on stderr for `-c` alone.

**Why.** Every consumer of shell text — linters, security gates, agent
harnesses — reimplements some slice of the grammar and can therefore disagree
with the shell that actually executes. For a gate, analyzer-versus-executor
divergence is the whole risk. Emitting from the `COMMAND *` the executor itself
built makes that divergence *inexpressible* rather than merely unlikely, since
there is only one parser involved.

**Guarantees.**

- *Never expands.* Words are emitted as written: `$x` stays `$x` and `$(cmd)`
  stays `$(cmd)`. Expansion is a runtime act with side effects, and a tool that
  reports a tree must not be able to run `$(rm -rf ...)`. A canary covers the
  command-substitution case specifically.
- *Never executes*, on any input path, proven by canary on `-c`, standard
  input, and a script file.
- *Versioned.* Every line carries `"rash_ast_version":1` so a consumer fails
  loudly on schema change instead of silently misreading.

**Schema, version 1.** `simple` and `connection` nodes are fully modeled:
words as `{"text","flags"}` with the raw `W_*` bits, per-command `redirects[]`
carrying a named `instruction`, the `redirector` descriptor, and either a
`word` or an `fd` depending on the redirection form. All other node types
(`for`, `if`, `group`, `subshell`, `case`, `function_def`, `coproc`, …) report
their `kind` and redirects, so a consumer can tell that something it does not
model is present rather than seeing nothing at all.

Words carry no position and non-`simple` nodes carry no line number, because
bash does not record them: `WORD_DESC` holds only text and flags, and
`COMMAND.line` is populated only for the node types modeled here. Emitting a
line number for the others would report uninitialized memory. Column positions
do not exist anywhere in the tree and would require threading offsets through
the lexer.

**POSIX.** Not implicated. This is a new GNU-style long option; `-c`, `-n`, and
every POSIX-specified behavior are unchanged.

**Tests.** `tests/emitast.tests`, `tests/emitast.right`, `tests/run-emitast` —
non-execution on all three input paths, the command-substitution canary, the
stderr announcement being present for `-c` and absent elsewhere, pipeline and
redirect structure, and unexpanded words. `tests/invocation.right` was updated
because the option legitimately appears in the usage output.

**Upstream status.** Rash-specific. Not proposed upstream.

---

## 3. SIGCHLD trap firings are not lost when a child is reaped during a trap

**Upstream behavior.** A child reaped while a SIGCHLD trap is executing queues
another firing that nothing in that pass comes back for, because the branch
`continue`s to the next signal. At end of input nothing parses again, so the
shell exits with the firing unrun. Roughly 20% of runs of the reproduction lose
at least one; bash's own `tests/trap8.sub` loses one in 8 of 120 runs.

**Rash behavior.** The counter is drained in a bounded loop, so every reaped
child gets its firing.

**Why.** Bash already guarantees one trap per reaped child on purpose —
`waitchld` counts reaps, `queue_sigchld_trap` accumulates, `run_sigchld_trap`
loops. The gap defeats machinery that exists specifically to close it, and it
makes bash's own test suite intermittently red, which is worse than the bug: a
suite that fails at random trains everyone to ignore it.

**Implementation.** One `if` became a bounded `while` in `run_pending_traps`,
at the site whose existing comment already says it is leaving the counter for
children reaped during the trap. Nine lines, four of them comment.

**POSIX.** Not implicated; this makes bash match its own documented behavior.

**Tests.** `tests/trapchld.tests`, `tests/trapchld.sub`, `tests/trapchld.right`,
`tests/run-trapchld` — thirty rounds, decisive past a hundred to one against the
measured pre-fix rate. `tests/trap8.sub` also stops failing intermittently.

**Upstream status.** Ready to send, with the patch:
`BUGFIX_REPRO_REPORTS/2026-07-31-sigchld-lost-trap-firings.md`.
