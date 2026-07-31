# SIGCHLD trap firings are lost when a foreground child is reaped

Reproduced on bash 5.3.15(1)-release, x86_64-pc-linux-gnu, Linux 6.18.

## Summary

The SIGCHLD trap is documented and implemented to run once per reaped child.
`waitchld` passes the number of children reaped to `queue_sigchld_trap`, which
accumulates it in `pending_traps[SIGCHLD]`, and `run_sigchld_trap` loops that
many times — machinery that exists precisely so kernel signal coalescing cannot
lose a firing.

Firings are lost anyway, intermittently, when background children are reaped in
the same window as a **foreground** child. Roughly one run in five loses one
firing under the reproduction below; occasionally two are lost.

This is not a theoretical concern: bash's own `tests/trap8.sub` depends on the
exact count and therefore fails intermittently, making `make tests` unreliable.

## Reproduction

```sh
#!/bin/bash
trap 'echo CHLD' SIGCHLD
for i in {1..10}; do sleep 0.3 & done
sleep 0.3
wait
```

Expected: 11 lines of `CHLD` (ten background children plus the foreground
`sleep`). Observed: usually 11, intermittently 10, occasionally 9.

Measured over 25 runs:

```
  1 run  fired  9 times
  4 runs fired 10 times
 20 runs fired 11 times
```

A smaller version — one background child plus one foreground command — loses a
firing roughly one run in twenty, so the rate scales with the number of
children reaped in the window.

## Isolation

Each variant run 20-30 times:

| variant | result |
|---|---|
| 10 background children **and** a foreground `sleep` | loses 1-2 firings in ~25% of runs |
| same, without `set -m` | loses 1 firing in ~15% of runs |
| 10 background children, **no** foreground command | 20/20 correct, no loss |
| 10 background children, foreground **busy loop** instead of a child | 25/25 correct, no loss |
| 1 background child plus a foreground command | loses 1 firing in ~5% of runs |

Two conclusions follow. Job control is not required — the loss occurs without
`set -m`. And merely being outside the `wait` builtin is not sufficient: a
foreground busy loop, which also leaves `this_shell_builtin != wait_builtin`,
never loses a firing. **Reaping a foreground child is the trigger.**

Staggering the exits (background child exiting well before, during, or
simultaneously with the foreground child) did not change the outcome in 30 runs
each, so this is not simple simultaneity.

## Where it is not

With no foreground command, `waitchld` takes the
`this_shell_builtin == wait_builtin` branch in `jobs.c` and calls
`run_sigchld_trap (children_exited)` directly, never touching `pending_traps`.
That configuration never loses a firing, which places the defect in the queued
path rather than in `run_sigchld_trap`'s loop or in the reap count itself.

With a foreground child, the reap happens inside `wait_for`, so the trap is
queued via `queue_sigchld_trap` instead. The count does reach
`pending_traps[SIGCHLD]`; the loss is downstream of queueing.

I was not able to isolate the exact statement responsible, and I would rather
report the measurement than guess at a patch.

## One candidate, offered as an observation rather than a fix

`run_pending_traps` reads and clears the counter in two steps:

```c
	      x = pending_traps[sig];
	      pending_traps[sig] = 0;
	      run_sigchld_trap (x);	/* use as counter */
```

`pending_traps` is a plain `int` array incremented from the SIGCHLD handler via
`queue_sigchld_trap`, so a child reaped between the read and the clear has its
increment discarded. Blocking SIGCHLD across those two statements is a correct
guard for that specific window.

However, doing so did **not** measurably change the loss rate in this
reproduction (9 losing rounds out of 30 before, 7 out of 30 after — within
noise of each other), so whatever dominates here is elsewhere. I mention the
window only because it appears genuinely unsafe on its own terms, not because
closing it fixed anything.

There is also existing evidence in the tree that this area has lost traps
before. `jobs.c` carries the comment:

```c
	  /* This was trap_handler (SIGCHLD) but that can lose traps if
	     children_exited > 1 */
```

and `run_pending_traps` has a `/* whoops -- print warning? */` branch for
SIGCHLD arriving while a SIGCHLD trap is in progress.
