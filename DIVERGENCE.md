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
`BUGFIX_REPRO_REPORTS/2026-07-31-sigchld-lost-trap-firings.md`. Send it
together with entry 4, which fixes the second half of the same contract.

---

## 4. SIGCHLD trap firings are not discarded on the direct dispatch path

**Upstream behavior.** `waitchld` runs the SIGCHLD trap two different ways: it
queues firings for `run_pending_traps`, or, when the `wait` builtin reaps the
child, it calls `run_sigchld_trap` **directly**. The direct call sets
`running_trap` and installs the `IMPOSSIBLE_TRAP_HANDLER` sentinel but never
sets `SIG_INPROGRESS`, and its dispatch chain never checks whether firings are
already queued. So a trap dispatched directly while `pending_traps[SIGCHLD]` is
non-zero re-enters `run_pending_traps` from its own trap body, matches none of
the three SIGCHLD branches, and falls through to the generic bad-handler
branch — which prints `run_pending_traps: bad value in trap_list[17]` and then
discards the queued firings. Upstream left a TODO for exactly this at
`trap.c:354`: *"could check for running the trap handler for the same signal
here."*

**Rash behavior.** Firings queued while a SIGCHLD trap is executing are kept
and then run, on both dispatch paths.

**Why.** Same contract as entry 3 — one trap per reaped child — broken by the
other dispatch path. Entry 3's fix drains the queued path; nothing drained this
one. The failure is also silent in its consequence and noisy in the wrong
place: the user loses a trap firing and gets an internal warning about a bad
pointer value, which reads like memory corruption rather than a lost signal.

**Implementation.** Two changes, because stopping the discard is not the same
as running what was kept.

1. The branch that recognizes "a SIGCHLD trap is already running" tested
   `SIG_INPROGRESS` as well as the sentinel. That was too narrow: the sentinel
   is installed only for the duration of `run_sigchld_trap` and restored when
   it unwinds, so it alone means a SIGCHLD trap is executing. Dropping the
   extra condition makes the recursive call defer the firings instead of
   discarding them.
2. Deferring alone converted a loud loss into a silent one — measured, not
   assumed: instrumentation showed `pending_at_exit=1` with the trap having run
   ten times instead of eleven. Branch 1 has a drain loop for the queued path;
   the direct path had none, so `run_deferred_sigchld_traps` drains it at that
   call site after the trap returns.

**POSIX.** Not implicated; this makes bash match its own documented behavior.

**Tests.** `tests/trapchld2.sub` plus the second section of
`tests/trapchld.tests` — a finite self-spawning trap with job control off and
instant-exit children. What is asserted is that the discarding branch is never
taken, which is what this fix guarantees and is decisive: over 2000 serial
rounds it fires 14 times without the fix and 0 times with it. The test runs
1000 rounds, expects about seven hits on a regression, and costs ~19 seconds.

Two properties of the workload were established by measurement and are easy to
get wrong. Sleeping children put the reaps outside the trap window — the same
shape with `sleep 0.3` ran 8600 rounds clean against a build this probe fails
on at 0.7%. And it must run serially: eight-way parallel load suppressed the
window completely (0 in 800 rounds) rather than widening it.

The firing **count** is deliberately not asserted over this probe. A third,
undiagnosed loss survives both fixes at roughly one round in a thousand under
load, with a signature distinct from either fixed mechanism: `waitchld` counts
a reap that is then neither queued nor run directly, with no warning and
nothing left pending at exit. Asserting the count here would make the suite
intermittently red for an open bug, which this project has already decided is
worse than the bug. It is recorded in `PLAN.md` with its counter evidence.

**Upstream status.** Not yet sent. Belongs in the same message as entry 3.
Found by an independent verifier (a Codex session) rejecting entry 3's fix as
incomplete; reproduced and diagnosed here from its lead.

---

## 5. SIGCHLD firings queued by the handler survive the drain's read-then-clear

**Upstream behavior.** `pending_traps[SIGCHLD]` is a counter incremented from
two contexts: `sigchld_handler` calls `waitchld` directly, and main-context
paths do too. The drain in `run_pending_traps` consumes it with a plain
read-then-clear (`x = pending_traps[sig]; pending_traps[sig] = 0;`). A
SIGCHLD delivered between those two statements increments the counter, and
the clear then zeroes an increment never captured in `x`. The firing is
neither run nor left pending, and nothing diagnostic is printed. Every other
signal uses `pending_traps` as a flag, where this pattern is harmless;
SIGCHLD is the only counting signal, and a count is what read-then-clear
cannot handle.

**Rash behavior.** The read and clear are atomic with respect to the handler:
both drain sites (`run_pending_traps` and `run_deferred_sigchld_traps`) hold
`BLOCK_CHILD` across the two-line window, releasing it before the trap body
runs so delivery semantics during trap execution are unchanged.

**Why.** Same contract as entries 3 and 4, broken a third way. This one is the
quietest of the three: no warning, nothing pending at exit, and a rate that
only becomes measurable under CPU load — which made it the one that kept the
Zig-compiled test suite intermittently red after the other two were fixed.

**Evidence.** Diagnosed by detector, not inference. The shell is
single-threaded, so a nested `queue_sigchld_trap` while main context is inside
a flagged window can only be the signal handler. Across 8000 loaded rounds of
the self-spawning probe: 15 losses, 14 carrying a drain-window hit, and the
write-side window hit zero times (main-context queueing is already covered by
`queue_sigchld` deferral and the `wait_for` `BLOCK_CHILD` sections). With the
window sealed, the same campaign produced zero losses and zero hits —
fifteen-to-zero against its own detector. The write side was left untouched
on that evidence.

This window was suspected on day one and wrongly "ruled out" when guarding it
moved 9-of-30 to 7-of-30 — a null result measured while entry 3's mechanism,
twenty times more frequent, dominated. The independent verifier that reported
the same guard working was discounted for the same reason. It was right.

**POSIX.** Not implicated; blocking a signal across two statements changes no
observable semantics except the absence of the loss.

**Tests.** The count assertion in `tests/trapchld.tests` over
`tests/trapchld2.sub`, restored once this landed — it was deliberately held
out while this class was open, because a gate that flakes on an unfixed bug
trains everyone to ignore it. With all three classes closed it is decisive
again: 766-of-2000 (stock), 14-of-2000 (entry 3's fix only), 0 thereafter.

**Upstream status.** Belongs in the same message as entries 3 and 4;
`BUGFIX_REPRO_REPORTS/2026-08-05-sigchld-lost-update-drain-window.md` is the
send-ready writeup, and `BUGFIX_REPRO_REPORTS/sigchld-fixes.patch` carries all
three fixes as one reviewable diff.

---

## 6. `--about`, and presenting as `bash` when invoked under that name

**Upstream behavior.** No `--about` option exists. bash already dispatches on
the basename of `$0` — `sh` sets `act_like_sh`, and the restricted-shell name
sets `restricted_shell` — but nothing selects an identity string that way,
because upstream only has one identity to select.

**Rash behavior.** `--about` writes a single line naming the shell, its
version, the platform it was built for, and a one-line description, then exits
without executing. The name it reports comes from the invocation name: `bash`,
`sh`, and the restricted-shell name all answer as **bash**; every other name,
including one the caller invents, answers as **rash**.

```
$ bash --about
bash 5.3.15(1)-release (x86_64-pc-linux-gnu) - GNU Bourne-Again SHell
$ rash --about
rash 5.3.15(1)-release (x86_64-pc-linux-gnu) - reversible, agent-safe GNU Bash fork
```

**Why.** This is the compatibility escape hatch that makes an eventual rename
of the binary safe to perform at all, so it has to exist *before* any broad
rename rather than after. A shell that is installed as `/bin/bash` must be
indistinguishable from bash when something asks it what it is; a script that
invokes `sh` is asking for compatibility and should not be told it is talking
to a fork. The fork announces itself only when called by its own name.

`--about` is also the smallest surface that makes the identity switch
observable and therefore testable. It is a new option, so it cannot break any
existing caller — unlike changing `--version` or `--help`, which scripts parse.
Those stay untouched until the rename is deliberately made.

**Implementation.** `shell_identity ()` in `shell.c` reuses the existing
mechanism: `base_pathname` on `shell_name`, with a leading `-` stripped so a
login shell arriving as `-bash` matches. It reads argv[0] rather than the
executable's real path, so `exec -a` is honored exactly like a symlink — the
name the caller used is the name that governs.

**POSIX.** Not implicated. A new GNU-style long option; no POSIX-specified
behavior changes, and no existing output changes.

**Tests.** `tests/identity.tests`, `tests/identity.right`,
`tests/run-identity` — checked as a classifier over the set of invocation
names rather than the one case that motivated it: symlinks named `bash`,
`rash`, `sh`, `rbash`, and an unknown name; `exec -a` for both identities;
login-shell forms `-bash` and `-rash`; and an absolute path. It also asserts
the line is exactly one line, carries the version and the platform, does not
run a `-c` command, and that answering as bash leaves the fork's name nowhere
in the output. `tests/invocation.right` was updated because the option
legitimately appears in the usage list.

**Upstream status.** Rash-specific. Not proposed upstream.
