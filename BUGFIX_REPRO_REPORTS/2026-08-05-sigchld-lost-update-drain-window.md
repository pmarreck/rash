# SIGCHLD firings queued by the handler are zeroed by the drain's read-then-clear

Reproduced and fixed against bash 5.3.15(1)-release, x86_64-pc-linux-gnu.

Third of three related defects in the once-per-reaped-child contract. The
others are `2026-07-31-sigchld-lost-trap-firings.md` (drain runs once, then
`continue`s past SIGCHLD) and `2026-08-04-sigchld-discarded-on-direct-dispatch.md`
(direct dispatch never marks SIGCHLD in progress, so queued firings are
discarded by the bad-handler branch). All three should travel together.

## Summary

`pending_traps[SIGCHLD]` is a counter incremented from **two contexts**:
`sigchld_handler` calls `waitchld` directly, and main-context code paths call
it too. The drain in `run_pending_traps` consumes it with a plain
read-then-clear:

```c
x = pending_traps[sig];
pending_traps[sig] = 0;
run_sigchld_trap (x);
```

If SIGCHLD is delivered between the read and the clear, the handler reaps a
child and increments the counter — and the `= 0` then zeroes an increment that
was never captured in `x`. The firing is gone: never run, never left pending,
no diagnostic of any kind. For every other signal `pending_traps` is a flag,
so this pattern loses nothing there; SIGCHLD is the only signal for which it
is a count, and a count is exactly what read-then-clear cannot handle.

## Reproduction

Same probe as the direct-dispatch report — a finite self-spawning trap with
instant-exit children and job control off — but this defect needs the machine
**under CPU load**, which widens the preemption window:

```sh
n=0
trap 'printf "CHLD\n"; n=$((n+1)); if [ "$n" -le 4 ]; then (exit 0) & fi' CHLD
i=0
while [ "$i" -lt 6 ]; do (exit 0) & i=$((i+1)); done
wait
```

Ten firings owed. With eight busy-loop processes pinning the cores, a build
with the first two fixes applied still loses one firing in roughly 1 round in
1500 (aggregate 11/17000 across campaigns); unloaded, the rate falls to nearly
zero. Load is the lever, not the child count.

## Diagnosis, by detector rather than inference

The shell is single-threaded, so a nested call into `queue_sigchld_trap`
while main context is inside a window can only be the signal handler. Two
window flags were added: one spanning the `+=` in `queue_sigchld_trap`
(write side), one spanning the read-then-clear in the drains (read side).

Across 8000 loaded rounds on the instrumented build: 15 rounds lost a firing.
Fourteen carried `hitd=1` — the handler had fired inside the read-then-clear
window — and the write-side flag was hit **zero** times all campaign. Two
rounds hit the drain window without losing a firing, which is the geometry
telling the same story: a handler landing between the flag-set and the read is
*picked up* by the read; only a landing between the read and the clear is
destroyed.

## Fix

Make the read-then-clear atomic with respect to the handler, using the
blocking idiom the codebase already uses everywhere else (`BLOCK_CHILD` /
`UNBLOCK_CHILD`), at both drain sites — the SIGCHLD branch of
`run_pending_traps` and `run_deferred_sigchld_traps`:

```c
      for (;;)
	{
	  sigset_t chld_set, chld_oset;

	  BLOCK_CHILD (chld_set, chld_oset);
	  x = pending_traps[SIGCHLD];
	  pending_traps[SIGCHLD] = 0;
	  UNBLOCK_CHILD (chld_oset);
	  if (x <= 0)
	    break;
	  run_sigchld_trap (x);		/* use as counter */
	}
```

SIGCHLD is blocked only across the two-line read/clear, never across the trap
body, so handler delivery semantics during trap execution are unchanged. A
reap arriving after the unblock is simply captured by the next loop iteration.
Two syscalls per drain batch, not per command.

The write side (`+=` in `queue_sigchld_trap`) was examined and left alone:
zero hits in 8000 loaded rounds. Main-context calls into `waitchld` are
already covered by existing machinery — the `queue_sigchld` flag makes
`sigchld_handler` defer rather than re-enter (`jobs.c:330`), and the
`wait_for` paths hold `BLOCK_CHILD` sections — so the handler cannot land
inside a main-context increment. The drains had neither protection, and were
the only site actually racing.

## Result

The strongest test available here is the intervention measured by its own
detector: with the fix applied to the instrumented build, the same 8000-round
loaded campaign produced **zero** losses and **zero** drain-window hits —
against 15 losses expected, odds of a fluke about e^-15. A further loaded
campaign on the fixed build with three independent counting channels (stdout,
an O_APPEND per-firing file immune to stdio, and a separated stderr) is
recorded in the ledger with its final count.

## A separate crash on the same path

While measuring this fix, the same probe was found to make bash **crash**
outright, at roughly 1 round in 12000 under load, on stock 5.3.15 as well as
here. It is a distinct defect and is reported separately in
`2026-08-05-sigchld-async-signal-unsafe-allocation.md`; nothing in this report
depends on it, and the fix above is unaffected either way. It is mentioned
only because a campaign long enough to measure the loss rate will eventually
see a crash, and the two should not be confused: a crash shows up as a
non-zero exit status, and the loss does not.

## History worth admitting upstream

This exact window was suspected on day one and "ruled out" when wrapping it in
`BLOCK_CHILD` moved a 9-of-30 failure rate to 7-of-30. That test was run while
the first defect — twenty times more frequent — dominated the measurements, so
the null result meant nothing. An independent verifier (a Codex session)
later reported the same guard fixing one of its cases and was discounted for
the same reason. The counters above are what settled it.
