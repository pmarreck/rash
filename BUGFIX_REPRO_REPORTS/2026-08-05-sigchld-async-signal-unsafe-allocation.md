# Heap corruption: the SIGCHLD path allocates from signal-handler context

Observed on stock bash 5.3.15(1)-release, x86_64-pc-linux-gnu (glibc 2.42),
and reproduced independently on a build using bash's own `lib/malloc`.

**Not fixed here.** This report is a reproduction and a diagnosis. The fix is
an architectural change to what `waitchld` may do when entered from a signal
handler, and that is a maintainer's call, not a patch to guess at.

## Summary

`sigchld_handler` calls `waitchld` directly:

```c
static sighandler
sigchld_handler (int sig)
{
  ...
  if (queue_sigchld == 0)
    n = waitchld (-1, 0);
  ...
}
```

`waitchld` is not async-signal-safe. On the reaping path it reaches code that
allocates — `set_pipestatus_array`, `bgp_add`, and, when it dispatches the
trap directly, `run_sigchld_trap` → `unwind_protect_mem` → `xmalloc`. If
SIGCHLD is delivered while the main program is inside `malloc` or `free`, the
allocator's internal state is re-entered and the heap is corrupted. The shell
then dies at some later, unrelated allocation.

POSIX lists the functions that may be called from a signal handler; `malloc`
is not among them, and neither is anything that calls it.

## Reproduction

An ordinary script: a SIGCHLD trap, background children, and `wait`.

```sh
#!/bin/bash
n=0
trap 'printf "CHLD\n"; n=$((n+1)); if [ "$n" -le 4 ]; then (exit 0) & fi' CHLD
i=0
while [ "$i" -lt 6 ]; do (exit 0) & i=$((i+1)); done
wait
```

Run it in a loop **with the machine under load** — eight busy-loop processes
on an 8-core machine — and check the exit status, which is the only reliable
signal that this happened:

```sh
for i in $(seq 1 12000); do
	./repro.sh >/dev/null 2>&1 || echo "round $i rc=$?"
done
```

Measured rates, both under the same load:

| build | allocator | crashes | signature |
|---|---|---:|---|
| stock 5.3.15 | glibc | 2 / 12000 | `SIGABRT`, `malloc(): ...` from `malloc_printerr` |
| local build | bash `lib/malloc` | 1 / 24000 | `SIGSEGV` reading `0xdfdfdfdfdfdfdfdf` |

Load is essential. Unloaded, neither build crashed in comparable campaigns.

## Evidence

Stock, glibc detecting the corruption at the next allocation:

```
#2  abort
#3  __libc_message_impl.cold
#4  malloc_printerr
#5  __libc_malloc
#6  xmalloc
#7  unwind_protect_mem
#8  run_sigchld_trap
#9  waitchld
#10 wait_for
#11 wait_for_single_pid
#12 wait_for_background_pids
#13 wait_builtin
#14 execute_builtin
```

The crashing frame is main context — `wait_builtin` reaching `waitchld`
synchronously. glibc is reporting damage done *earlier*, by the handler-context
entry into the same code, and detected here at the next `malloc`.

The local build, whose `lib/malloc` scrambles freed memory with `0xdf`
(`lib/malloc/malloc.c:894`, `MALLOC_MEMSET ((char *)(p + 1), 0xdf, n)`),
shows the other half of the same story — a use-after-free rather than a
detected-and-aborted heap:

```
#0  wait_builtin        rax = 0xdfdfdfdfdfdfdfdf
#1  execute_builtin
#2  execute_command_internal
```

Faulting at `builtins/wait.def:271`, `w = list->word->word`, with `list->word`
pointing into freed-and-scrambled memory. `wait` was invoked with **no
arguments** in this script, so that list should have been empty.

Two allocators, two signatures, one cause: the heap is being mutated from a
signal handler.

## Why this is worth fixing rather than tolerating

- It is reachable from a plain script. No unusual builtins, no `set -m`, no
  subshell tricks — a SIGCHLD trap plus background children.
- It is memory corruption, so the observed crash is a best case. Silent
  corruption is the same bug on a luckier schedule.
- The rate scales with load, which is the wrong direction: the failure gets
  more likely exactly when a machine is busy.

## Notes toward a fix

`queue_sigchld` and the `UNQUEUE_SIGCHLD` macro (`jobs.c:330`) already exist to
make the handler defer instead of re-entering `waitchld`, which suggests the
shape of the answer: from handler context, do the async-signal-safe minimum —
`waitpid` into a preallocated table and set a flag — and leave every allocation
to the next synchronous drain. Making that complete is the real work, since
several helpers on the reaping path allocate today.

Deferring the trap dispatch alone is not sufficient: `set_pipestatus_array` and
`bgp_add` allocate on the reaping path whether or not a trap is set.
