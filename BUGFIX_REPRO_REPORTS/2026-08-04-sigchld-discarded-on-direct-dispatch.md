# SIGCHLD trap firings are discarded when the trap is dispatched directly

Reproduced and fixed against bash 5.3.15(1)-release, x86_64-pc-linux-gnu.

This is the second half of the bug described in
`2026-07-31-sigchld-lost-trap-firings.md`. That report fixed the queued
dispatch path; this one fixes the direct path. The two should be applied
together — each alone leaves the contract broken.

## Summary

`waitchld` dispatches the SIGCHLD trap two ways. Usually it queues firings into
`pending_traps[SIGCHLD]` for `run_pending_traps` to run. But when the `wait`
builtin reaps the child, it calls `run_sigchld_trap` **directly**.

The direct call sets `running_trap` and installs the `IMPOSSIBLE_TRAP_HANDLER`
sentinel, but never sets `SIG_INPROGRESS`. Its dispatch chain also never checks
whether firings are already queued. So a trap dispatched directly while
`pending_traps[SIGCHLD]` is non-zero re-enters `run_pending_traps` from inside
its own trap body, matches none of the three SIGCHLD branches, and falls
through to the generic bad-handler branch, which warns and discards the queued
firings.

The user-visible result is a lost trap firing plus an internal warning that
reads like memory corruption:

```
warning: run_pending_traps: bad value in trap_list[17]: 0x5f0fed662920
```

## Reproduction

```sh
#!/bin/bash
# Job control off (the default for a non-interactive shell) is what steers
# waitchld to the direct call. Children exit instantly rather than sleeping, so
# reaps land inside the trap window; the trap spawns a child on each of its
# first four firings so reaping continues while the trap is already running.
n=0
trap 'printf "CHLD\n"; n=$((n+1)); if [ "$n" -le 4 ]; then (exit 0) & fi' CHLD
i=0
while [ "$i" -lt 6 ]; do (exit 0) & i=$((i+1)); done
wait
```

Exactly ten firings are owed: six initial children plus four spawned by the
trap. Loop it and count:

```sh
for i in $(seq 1 2000); do ./repro.sh 2>/dev/null | grep -c CHLD; done |
	sort -n | uniq -c
```

Measured on stock 5.3.15, 2000 rounds: **766 rounds came up short**. Only 10 of
those printed the `bad value in trap_list` warning, because two distinct
mechanisms are in play — the drain-once bug reported in
`2026-07-31-sigchld-lost-trap-firings.md` accounts for the bulk and is silent,
and this one is the loud remainder. Applying that first patch alone drops the
same probe to **14 rounds in 2000, every one of them warning**. This is what
isolates the second mechanism.

Two things about the workload matter, both established by measurement rather
than assumed. Sleeping children put the reaps *outside* the trap window: the
same shape with `sleep 0.3` children ran 8600 rounds clean on a build where
this probe fails at 0.7%. And it must be run **serially** — eight-way parallel
load suppressed the window entirely (0 in 800 rounds) rather than widening it.

## Cause

`run_pending_traps` has three SIGCHLD branches, then a generic one:

```c
	  else if (sig == SIGCHLD &&
		   trap_list[SIGCHLD] != (char *)IMPOSSIBLE_TRAP_HANDLER &&
		   (sigmodes[SIGCHLD] & SIG_INPROGRESS) == 0)
	    { /* run the queued firings */ }
	  else if (sig == SIGCHLD &&
		   trap_list[SIGCHLD] == (char *)IMPOSSIBLE_TRAP_HANDLER &&
		   (sigmodes[SIGCHLD] & SIG_INPROGRESS) != 0)
	    { /* leave pending_traps[SIGCHLD] alone */ }
	  else if (sig == SIGCHLD && (sigmodes[SIGCHLD] & SIG_INPROGRESS))
	    { /* leave pending_traps[SIGCHLD] alone */ }
	  else if (trap_list[sig] == (char *)DEFAULT_SIG ||
		   trap_list[sig] == (char *)IGNORE_SIG ||
		   trap_list[sig] == (char *)IMPOSSIBLE_TRAP_HANDLER)
	    { internal_warning (...); }		/* falls through to clear */
```

During a directly-dispatched trap the state is `trap_list[SIGCHLD] ==
IMPOSSIBLE_TRAP_HANDLER` with `SIG_INPROGRESS` clear. That matches no SIGCHLD
branch. The generic branch does not `continue`, so control reaches
`pending_traps[sig] = 0` at the bottom of the loop and the backlog is gone.

Upstream anticipated this. The comment at `trap.c:354` reads: *"could check for
running the trap handler for the same signal here (running_trap == sig+1)."*

Confirmed by instrumentation rather than inference. Counters at each accounting
point, dumped at `exit_shell`, on two captured failing runs:

```
CHLD=10 :: direct=1 b4=1 queued=10 ran=10 pending_at_exit=0
CHLD=10 :: direct=3 b4=1 queued=8  ran=10 pending_at_exit=0
```

`b4` counts entries to the generic branch for SIGCHLD; it is 1 in exactly the
runs that come up one firing short, and 0 in every clean run. A further counter
showed `pending_traps[SIGCHLD]` was already non-zero when the direct call
started, and that nothing was queued *during* it — establishing that the
backlog predates the direct dispatch rather than arriving inside it.

## Fix

Two changes. Stopping the discard is not the same as running what was kept.

First, recognize a running SIGCHLD trap by the sentinel alone. It is installed
only for the duration of `run_sigchld_trap` and restored when it unwinds, so it
alone means a SIGCHLD trap is executing:

```c
	  else if (sig == SIGCHLD &&
		   trap_list[SIGCHLD] == (char *)IMPOSSIBLE_TRAP_HANDLER)
```

That alone is not sufficient, and shipping it alone would be worse than the
bug: instrumentation after this change showed `b4=0` but `pending_at_exit=1`
with the trap having run ten times instead of eleven. The firings were no
longer discarded, merely deferred forever — a loud loss turned into a silent
one. `run_pending_traps` has a drain loop for the queued path; the direct path
had none.

So, second, drain at the direct dispatch site in `waitchld`:

```c
	  run_sigchld_trap (children_exited);	/* XXX */
	  run_deferred_sigchld_traps ();
```

```c
void
run_deferred_sigchld_traps (void)
{
  int x;

  while ((x = pending_traps[SIGCHLD]) > 0)
    {
      pending_traps[SIGCHLD] = 0;
      run_sigchld_trap (x);		/* use as counter */
    }
}
```

## Result

The reproduction above, 2000 serial rounds, across the three states:

| build | rounds losing a firing | of those, warning |
|---|---:|---:|
| stock 5.3.15, neither patch | 766 / 2000 | 10 |
| first patch only | 14 / 2000 | 14 |
| both patches | **0 / 2000** | 0 |

Each patch removes a distinct mechanism, and neither alone satisfies the
contract. Seeing zero in 2000 rounds against a 0.7% baseline puts a surviving
regression past a hundred thousand to one.

Other checks:

| | before | after |
|---|---|---|
| captured failing runs | `b4=1`, `ran=10` of 11 | mechanism cannot be entered |
| after the first change only | `b4=0` but `pending_at_exit=1`, `ran=10` | — |
| self-spawning SIGCHLD trap, bounded | terminates | terminates identically |
| self-spawning SIGCHLD trap, unbounded | does not terminate | does not terminate |

The drain loop terminates on the same inputs stock bash terminates on: a
bounded self-spawning trap completes with the same firing count on both, and an
unbounded one fails to terminate on both, so the loop introduces no new
divergence.

The complete bash test suite passes with both changes applied.

## A further loss, not addressed here

The same probe still comes up one short in roughly one round in a thousand
**when the machine is under load**, by a mechanism distinct from either fix
above. On those rounds `waitchld` accounts the reap and it is then neither
queued nor run: no warning, and nothing left pending at exit. Counters read
`reaped=10 queued=9 ran=9` against `reaped=10 queued=8 ran=10` on clean rounds.
A counter placed on the `if (children_exited && ...)` dispatch condition itself
showed the reap does enter the block, so it is being lost inside the dispatch
chain rather than skipped by the guard.

That is reported separately rather than bundled here, because the two fixes
above are complete and independently justified for the mechanisms they close,
and because it has not been diagnosed to the same standard.
