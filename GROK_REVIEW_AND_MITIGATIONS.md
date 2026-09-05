# SIGCHLD trap once-per-child: independent review of `be829983`

**Date:** 2026-08-04 (EDT)  
**Reviewer:** Grok (xAI), independent of the Claude fix session and the Codex verification  
**Subject commit:** `be829983` — *Run every queued SIGCHLD trap before leaving run_pending_traps*  
**Binary under test:** rebuild at HEAD `b666c2ff` (identical `trap.c` / `jobs.c` to `be829983`; only `PLAN.md` differs)  
**Contract under test:** the SIGCHLD trap runs **exactly once per reaped child**

This document records measured failure rates, the structural gap that survives the drain-`while` fix, and prospective mitigations. It proposes no code change; implementation is deferred until Peter directs.

---

## 1. Executive summary

| Claim | Result |
|---|---|
| `be829983` closes the residual-`pending_traps[SIGCHLD]`-at-EOF loss class | **Confirmed.** `trap8.sub` and non-self-spawning exact-count probes stay green. |
| Contract holds in full after `be829983` | **False.** Finite self-spawning traps under no job control still lose firings. |
| Measured residual rate (self-spawn no-jc probe, exact N=10) | **6 / 1400 ≈ 0.43%** of rounds; every loss is exactly one missing firing (`got=9 expected=10`). |
| Mechanism on failing rounds | Recursive `run_pending_traps` during a **directly invoked** `run_sigchld_trap` hits the generic bad-handler path for SIGCHLD (signal 17), because `SIG_INPROGRESS` is never set on that path. Live stderr matches the theory. |

**Bottom line:** mail nothing upstream until the second path is closed. A partial patch is worse than none.

---

## 2. What `be829983` changed

### Prior defect (fixed)

`run_pending_traps` ran the SIGCHLD branch once:

```c
x = pending_traps[sig];
pending_traps[sig] = 0;
run_sigchld_trap(x);
/* ... */
continue;  /* advances to next signal */
```

A child reaped *while* the trap ran queued another firing into `pending_traps[SIGCHLD]`. The re-entrant call correctly declined to consume it (`SIG_INPROGRESS` set on this path), but the outer `continue` advanced past SIGCHLD. Nothing revisited the counter in that pass. At end of input nothing parsed again, so the shell exited with residual pending and a lost firing.

Instrumentation (Claude session) showed residual `pending_traps[SIGCHLD]` at exit equalled missing `CHLD` lines on every losing run.

### Fix

Bounded drain where the existing comment already intended drain:

```c
while ((x = pending_traps[sig]) > 0)
  {
    pending_traps[sig] = 0;
    run_sigchld_trap(x);
  }
```

Children are finite, so the loop terminates. No other logic changed.

### Evidence the first class is closed

| Probe | Expectation | Result after `be829983` |
|---|---:|---|
| `tests/trapchld.sub` (30 rounds) | 11 | 0 losses |
| `tests/trap8.sub` serial (120) | 4 | 0 losses |
| `tests/trap8.sub` 8-way (120) | 4 | 0 losses |
| Non-self-spawning exact-count scenarios (see §4) | various | **0 / 3000+** |

Pre-fix baselines (from `BUGFIX_REPRO_REPORTS/2026-07-31-sigchld-lost-trap-firings.md`): ~6–9/30 on the classic repro; `trap8.sub` 8/120 at 8-way.

---

## 3. Independent verification method

Requirements from the review brief:

1. Own harness — do **not** reuse Codex or Claude scripts, or treat their pass/fail as ground truth.
2. Exact known child counts (not fuzzy presence checks).
3. Report **failure rate**, not only pass/fail.
4. Treat prior findings as leads, not authority.

### Harness location

`/tmp/rash_sigchld_verify/verify_sigchld_once_per_child`  
(ephemeral; not committed)

Each scenario prints a unique marker line `@@CHLD@@` once per intended firing. The harness counts exact line matches and compares to the declared child total. Stderr is captured per round so bad-handler warnings are not lost.

### Scenarios (exact N)

| ID | Description | Job control | Expected firings |
|---|---|---|---:|
| A | Pure background stampede, reaped by `wait` | off | 14 |
| B | Same as A | on (`set -m`) | 14 |
| C | Foreground command overlaps bg exits (queue path) | off | 13 (12 bg + 1 fg) |
| D | Same as C, different N | on | 10 (9 bg + 1 fg) |
| **E** | **Finite self-spawning trap** (spawn on first 4 firings) | **off** | **10** (6 + 4) |
| F | Finite self-spawning trap | on | 8 (5 + 3) |
| G | Slow trap body (CPU loop) so later reaps land mid-trap | off | 18 |
| H | Staggered sleep lifetimes | on | 7 |
| NJC11 | 10 bg sleep + 1 fg sleep (Codex-shaped N) | off | 11 |
| PW11 | Pure `wait` over 11 instant children | off | 11 |

Scenario E (the one that bites):

```bash
n=0
trap 'printf "%s\n" @@CHLD@@; n=$((n+1)); if [ "$n" -le 4 ]; then (exit 0) & fi' CHLD
i=0
while [ "$i" -lt 6 ]; do (exit 0) & i=$((i+1)); done
wait
# exact N = 6 initial + 4 self-spawns = 10
```

---

## 4. Measured rates

### Aggregate table

| Scenario | Fails / rounds | Rate |
|---|---:|---:|
| A serial | 0 / 200 | 0.00% |
| B serial | 0 / 200 | 0.00% |
| C serial | 0 / 200 | 0.00% |
| D serial | 0 / 200 | 0.00% |
| **E serial** | **2 / 200** | **1.00%** |
| **E heavy** | **3 / 1000** | **0.30%** |
| **E parallel ×8** | **1 / 200** | **0.50%** |
| F serial | 0 / 200 | 0.00% |
| G serial | 0 / 200 | 0.00% |
| H serial | 0 / 200 | 0.00% |
| NJC11 | 0 / 500 | 0.00% |
| PW11 | 0 / 500 | 0.00% |
| A / C / G parallel ×8 | 0 / 600 | 0.00% |

**E combined: 6 / 1400 ≈ 0.43%**  
**All non-E probes above: 0 / 3000**

### Failure shape (E only)

- Always **exactly one** missing marker (`got=9`, `expected=10`). No overshoots observed.
- Distribution on 1000-round heavy run: `got=10` × 997, `got=9` × 3.
- Parallel load did not dramatically change the rate (0.50% vs 0.30–1.00% serial).

### Stderr on every E failure

```
bash: warning: run_pending_traps: bad value in trap_list[17]: 0x5555555e88a0
```

- Signal 17 = `SIGCHLD` on Linux x86_64.
- Pointer equals `IMPOSSIBLE_TRAP_HANDLER`, defined as `(SigHandler *)initialize_traps` (`trap.h`).
- Confirmed via `nm`: `initialize_traps` at `0x948a0`; with PIE base `0x555555554000`, runtime address is `0x5555555e88a0`.

This is not residual-pending-at-exit. It is the **generic bad-handler path** consuming (and discarding) a queued SIGCHLD firing while the trap string is the IMPOSSIBLE sentinel.

---

## 5. Structural root cause (second class)

### Two delivery routes for SIGCHLD traps

From `waitchld` in `jobs.c` (simplified):

| Condition | Action |
|---|---|
| posix + `wait` builtin interrupt path | `queue_sigchld_trap` |
| Called from signal handler (`sigchld != 0`) | `queue_sigchld_trap` |
| `signal_in_progress(SIGCHLD)` | `queue_sigchld_trap` |
| `trap_list[SIGCHLD] == IMPOSSIBLE` | `queue_sigchld_trap` |
| `running_trap` | `queue_sigchld_trap` |
| **`this_shell_builtin == wait_builtin`** | **`run_sigchld_trap` directly** |
| else | `queue_sigchld_trap` |

Queued firings are later drained by `run_pending_traps`, which **does** set `SIG_INPROGRESS` around `run_sigchld_trap`.

### Direct path gap

`run_sigchld_trap` (`jobs.c` ~4524–4533):

```c
running_trap = SIGCHLD + 1;
set_impossible_sigchld_trap();   /* trap_list → IMPOSSIBLE; clears SIG_TRAPPED */
for (i = 0; i < nchild; i++)
  parse_and_execute(...);
/* ... */
running_trap = 0;
```

It never sets `sigmodes[SIGCHLD] |= SIG_INPROGRESS`.

### Recursive `run_pending_traps` while that is true

`trap.c` ~347–362: if `running_trap > 0`, early-return only for recursive **SIGWINCH**. Upstream left a TODO at ~354:

```c
/* could check for running the trap handler for the same signal here
   (running_trap == sig+1) */
```

Then the SIGCHLD branches (~401–441):

| Branch | Predicate | During direct `run_sigchld_trap` |
|---|---|---|
| Drain / run | `trap_list != IMPOSSIBLE` && `!SIG_INPROGRESS` | **false** (list is IMPOSSIBLE) |
| Nested-ok leave-pending | `IMPOSSIBLE` && `SIG_INPROGRESS` | **false** (no `SIG_INPROGRESS`) |
| In-progress leave-pending | `SIG_INPROGRESS` | **false** |
| Bad-handler | `DEFAULT` / `IGNORE` / **`IMPOSSIBLE`** | **true** → warn + clear pending |

So a child reaped (and queued) while a *directly invoked* SIGCHLD trap is running is discarded with a warning, not deferred and not run. The drain-`while` never sees it.

### Why self-spawn under no job control hits this

Scenario E’s trap body starts another short-lived child. That child can exit and be accounted while the outer direct `run_sigchld_trap` (from `wait`) is still active: `running_trap` set, list IMPOSSIBLE, `SIG_INPROGRESS` clear. The next `run_pending_traps` (from `execute_cmd` / parse paths during trap evaluation) takes the bad-handler path.

Non-self-spawning probes (A–D, G, H, NJC11, PW11) did not produce this stderr or a count mismatch in 3000+ rounds — either reaps stay outside that window, or `jobs_list_frozen` / queue timing keeps them off the bad path often enough that the rate is below the sample. The contract still requires the state machine to be safe for the self-spawn case.

### Contrast with Codex counters

Codex reported (lead, not trusted as sole evidence):

```
reaped=11 queued=10 started=10 finished=10 pending=0
```

That shape suggests a **pre-queue** loss (reaped but never queued). This review’s failures are **post-queue discards** (bad-handler clears pending without running). Both break the once-per-child contract. They may be two faces of the same incomplete state (direct path without `SIG_INPROGRESS`), or two nearby races. Closing the state machine properly should be designed to cover both; the bad-handler path is proven live here.

Codex’s 10-of-11 no-jc case was **not** reproduced in 500 rounds of an 11-child fg-overlap probe or 500 pure-wait rounds. Either the rate is far below 0.2%, or the trigger needs a tighter window than this harness used. The self-spawn case alone is enough to reject “contract satisfied.”

---

## 6. Relation to prior work (build on, do not trust)

| Source | Claim | This review |
|---|---|---|
| Claude / `be829983` | Drain-`while` fixes residual pending at EOF; trap8 120/120 | **Agreed** for that class |
| Claude | Could not reproduce Codex failures in 180 rounds | Plausible for no-jc stampede; **self-spawn fails at ~0.4%** so 180 rounds can easily go green by chance |
| Codex | Self-spawn 6-of-7 at round 24/60 | **Same class reproduced** (9-of-10, rate ~0.43%) |
| Codex | No-jc 10-of-11 at round 61/120 | **Not reproduced** in 500+ rounds of similar N |
| Codex | Structural: no `SIG_INPROGRESS` on direct path | **Confirmed** by live bad-handler warning + symbol match |
| PLAN.md note under `b666c2ff` | Fix incomplete; set `SIG_INPROGRESS` around direct call | **Supported**; see §7 |

---

## 7. Prospective mitigations

Ordered from smallest / most local to broader. Prefer the smallest change that makes the state machine consistent and drives the E rate to 0/N for a large N (and keeps non-E green).

### Fix A (primary candidate): set `SIG_INPROGRESS` on the direct path

Mirror the queued path’s bookkeeping inside `run_sigchld_trap`, or around the sole direct caller at the `wait_builtin` branch:

```c
/* conceptual — not applied */
sigmodes[SIGCHLD] |= SIG_INPROGRESS;
running_trap = SIGCHLD + 1;
set_impossible_sigchld_trap();
for (i = 0; i < nchild; i++)
  parse_and_execute(...);
/* restore trap string via existing unwind */
running_trap = 0;
sigmodes[SIGCHLD] &= ~SIG_INPROGRESS;
```

**Why this should work:** recursive `run_pending_traps` then matches branch 2 or 3 (`IMPOSSIBLE` + `SIG_INPROGRESS`, or `SIG_INPROGRESS` alone), leaves `pending_traps[SIGCHLD]` alone, and returns/`continue`s without the bad-handler. After the direct call finishes, either:

- the outer `run_pending_traps` drain-`while` (if still in a nested context that re-enters), or
- a later top-level `run_pending_traps`,

consumes the deferred count with a full run.

**Risks / checks:**

- Unwind / longjmp from inside the trap must clear `SIG_INPROGRESS` (queued path already pairs set/clear; direct path must too — prefer unwind-protect).
- `signal_in_progress(SIGCHLD)` is consulted in `waitchld` to force queueing; setting the flag during the direct run is consistent with that intent.
- Must not double-run firings: the direct call already ran `nchild` times for the reaps that triggered it; only *newly* queued counts should run later.

### Fix B: teach the `running_trap > 0` guard about same-signal recursion

At `trap.c:347`, for SIGCHLD (or generally when `running_trap == sig+1` for the pending signal), leave pending alone and return/continue rather than falling into the generic loop’s bad-handler. Upstream’s TODO at ~354 is this idea.

**Pros:** defends any future direct caller that forgets `SIG_INPROGRESS`.  
**Cons:** alone it may leave pending stranded if nothing drains after return (the exact class `be829983` fixed for the non-recursive case). Best as a belt with Fix A, not a replacement.

### Fix C: never take the direct path when more reaps can arrive mid-trap

Always `queue_sigchld_trap` from `waitchld` and let `run_pending_traps` own execution (single code path, always sets `SIG_INPROGRESS`).

**Pros:** one state machine.  
**Cons:** larger behavioral surface (posix `wait` interrupt interactions, latency of trap relative to `wait`, frozen job list assumptions). Higher regression risk; only if A+B prove insufficient.

### Fix D (supporting): avoid clearing pending on IMPOSSIBLE for SIGCHLD

In the bad-handler arm, special-case SIGCHLD + IMPOSSIBLE: leave `pending_traps[SIGCHLD]` intact and continue, matching the intent of branches 2–3.

**Pros:** stops silent (well, warned) destruction of the counter.  
**Cons:** papering over the missing flag; still depends on a later drain. Pair with A or B.

### Recommended sequence

1. **TDD first:** promote scenario E (and a no-jc pure-wait control with a different exact N) into a statistical or multi-round test that fails on this tree before any code change. Capture that the bad-handler warning is present on at least one forced or probabilistic loss, or drive a deterministic repro if one can be built.
2. Apply **Fix A** with unwind-safe clear of `SIG_INPROGRESS`.
3. Re-run the full harness: E target **0 / ≥2000**; non-E stay 0; `trap8` 8-way 0/120; full `./test`.
4. If any residual loss remains, add **Fix B** and/or **Fix D**, remeasure.
5. Only then refresh `BUGFIX_REPRO_REPORTS/…` and consider upstream mail — as a **combined** description of both loss classes and both fixes, or as two patches if Chet prefers minimal diffs.

### Explicitly out of scope for a “quick” fix

- Weakening `trap8.sub` / `trapchld` expectations.
- Suppressing the bad-handler warning without preserving the count.
- Declaring the race “timing, not a bug” — the design loops `nchild` times specifically so coalescing cannot lose firings; a lost firing is a defect.

---

## 8. Proposed acceptance gates (when implementing)

| Gate | Pass criterion |
|---|---|
| Scenario E (self-spawn, no jc, N=10) | 0 losses in ≥2000 serial rounds |
| Scenario E parallel ×8 | 0 losses in ≥500 rounds |
| Scenarios A–D, F–H, NJC11, PW11 | 0 losses in ≥200 each (or keep current sample if green) |
| `tests/trap8.sub` | 0/120 serial and 0/120 at 8-way |
| `tests/trapchld.*` | green under `./test` |
| Full suite | `./test` exit 0 |
| Stderr on E | no `bad value in trap_list[17]` under the campaigns above |

Optional MFIC-minded additions later:

- Deterministic stress: inject a reap/queue while `run_sigchld_trap` is mid-loop (debug hook or controlled test build), assert pending is preserved and eventually drained once.
- Mutation: force `SIG_INPROGRESS` clear mid-trap and assert the test suite goes red (proves the gate has teeth).

---

## 9. Files of interest

| Path | Role |
|---|---|
| `trap.c` ~347–441 | `run_pending_traps` recursion guard, SIGCHLD branches, bad-handler |
| `trap.c` ~715–737 | `set_impossible_sigchld_trap`, `queue_sigchld_trap` |
| `trap.h` | `IMPOSSIBLE_TRAP_HANDLER`, `DEFAULT_SIG`, `IGNORE_SIG` |
| `jobs.c` ~4235–4271 | `waitchld` dispatch (queue vs direct) |
| `jobs.c` ~4493–4534 | `run_sigchld_trap` (sets `running_trap`, never `SIG_INPROGRESS`) |
| `tests/trap8.sub`, `tests/trapchld.*` | existing statistical controls for the first loss class |
| `BUGFIX_REPRO_REPORTS/2026-07-31-sigchld-lost-trap-firings.md` | first-class writeup (pre/post drain-`while`) |
| `PLAN.md` | open item: `be829983` incomplete |

---

## 10. Conclusion

`be829983` correctly fixed the drain-once / residual-pending loss and made `trap8` reliable. The once-per-reaped-child contract is still violated by a second path: **direct `run_sigchld_trap` without `SIG_INPROGRESS`**, so recursive `run_pending_traps` treats the IMPOSSIBLE sentinel as a bad handler and drops queued firings.

Independent measurement: **~0.43% (6/1400)** on a finite self-spawning, no-job-control probe with exact N=10; every failure one short, every failure with the bad-handler warning on SIGCHLD. Non-self-spawning probes and the original trap suite stayed at 0 losses in this review’s samples.

**Prospective primary fix:** set and clear `SIG_INPROGRESS` around the direct path (Fix A), with TDD and the acceptance gates in §8. Do not send a partial upstream report until E is green at large N.

---

*Document only. No source changes applied for this review.*
