# SIGCHLD trap firings are lost when a child is reaped during a SIGCHLD trap

Reproduced and fixed against bash 5.3.15(1)-release, x86_64-pc-linux-gnu.

## Summary

The SIGCHLD trap is meant to run once per reaped child, and bash implements
that deliberately: `waitchld` passes the number of children reaped to
`queue_sigchld_trap`, which accumulates into `pending_traps[SIGCHLD]`, and
`run_sigchld_trap` loops that many times so kernel signal coalescing cannot
lose a firing.

Firings are lost anyway when a child is reaped *while a SIGCHLD trap is
running*. `bash`'s own `tests/trap8.sub` depends on the exact count and fails
intermittently as a result, making `make tests` unreliable.

## Reproduction

```sh
#!/bin/bash
trap 'echo CHLD' SIGCHLD
for i in {1..10}; do sleep 0.3 & done
sleep 0.3
wait
```

Expect 11 `CHLD` lines. Intermittently 10, sometimes 9. Loop and count:

```sh
for i in {1..30}; do ./repro.sh 2>&1 | grep -c CHLD; done | sort -n | uniq -c
```

Measured on an unmodified 5.3.15 build: 6 to 9 losing rounds out of 30.
`tests/trap8.sub` itself loses a firing in 8 of 120 runs at 8-way parallelism.

## Cause

`run_pending_traps` in `trap.c` handles a pending SIGCHLD like this:

```c
	      sigmodes[SIGCHLD] |= SIG_INPROGRESS;
	      evalnest++;
	      x = pending_traps[sig];
	      pending_traps[sig] = 0;
	      run_sigchld_trap (x);	/* use as counter */
	      running_trap = 0;
	      evalnest--;
	      sigmodes[SIGCHLD] &= ~SIG_INPROGRESS;
	      /* continue here rather than reset pending_traps[SIGCHLD] below in
		 case there are recursive calls to run_pending_traps and children
		 have been reaped while run_sigchld_trap was running. */
	      continue;
```

While `run_sigchld_trap` executes the trap action, more children can be reaped.
Those reaps queue additional firings into `pending_traps[SIGCHLD]`, and a
re-entrant `run_pending_traps` correctly declines to consume them — the
`SIG_INPROGRESS` branch leaves the counter alone on purpose.

But the `continue` above advances the enclosing `for (sig = 1; sig < NSIG;
sig++)` loop to the **next signal**. Nothing revisits SIGCHLD in that pass, so
the newly queued firings wait for the next call to `run_pending_traps`, which
happens in `parse_command` on the way to reading another command. At end of
input nothing parses again, and the firings are never run. The shell exits with
`pending_traps[SIGCHLD]` still non-zero.

This was confirmed by instrumentation rather than inference: with counters at
each accounting point, the number of children reaped was *always* correct, the
trap ran exactly as many times as it was asked to, and the residual
`pending_traps[SIGCHLD]` at exit was exactly equal to the number of missing
`CHLD` lines in every run. A counter on the re-entrant `SIG_INPROGRESS` branch
predicted the loss exactly: one bail, one missing firing.

This also explains the trigger. With only background children and a closing
`wait`, `waitchld` takes the `this_shell_builtin == wait_builtin` branch and
runs the trap synchronously without ever queueing, and no firing is lost. A
*foreground* child is what places a reap inside the trap-execution window.

## Fix

Drain the counter where the existing comment already intends it to be drained:

```c
	      /* Drain here rather than once: a child reaped while the trap was
	         running queues another firing, and the `continue' below advances
	         to the next signal, so nothing would revisit SIGCHLD in this
	         pass. Bounded -- children are finite. */
	      while ((x = pending_traps[sig]) > 0)
		{
		  pending_traps[sig] = 0;
		  run_sigchld_trap (x);	/* use as counter */
		}
```

`if` becomes a bounded `while`; the loop terminates because the number of
children is finite. Nothing else changes.

## Result

| | before | after |
|---|---|---|
| reproduction above, 60 rounds | 6-9 losing rounds per 30 | 60/60 correct |
| `tests/trap8.sub`, 120 runs at 8-way parallelism | 8/120 lost a firing | 120/120 correct |

The complete bash test suite passes with the change applied.
