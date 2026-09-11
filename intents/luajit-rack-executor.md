# Direction: LuaJIT-driven Rack-style executor (proposed)

**Status:** proposed — refined 2026-09-10 from Peter’s responses; still **not
accepted for implementation** until he explicitly prioritizes it.  
**Source:** Peter, 2026-09-10.  
**Relation to root:** Extends agent-safe / reversible shell; keeps Bash parse
identity and GPL provenance. C becomes syscall / job-control / builtin
**ports** under a Lua-owned middleware stack — not the place policy lives.

---

## 1. Thesis

Keep Bash’s **parser and compatibility surface**, re-express **command
execution** as a root-blessed stack of LuaJIT middleware over a single
lifecycle object called **`cmd`** (not `env` — that name is Bash’s variable
environment and would collide). Middleware *is* the pipeline; today’s bolted
sensors become layers in a named, ordered stack.

**Why bother (Peter):** once this exists, structured FDs, extra hooks, and most
new execution-policy ideas become “add or reorder middleware,” with **no
marshalling** between Lua and an internal C command representation on the hot
path. That is the payoff — not a requirement for implementing those ideas
sooner on the current sensor/port model.

---

## 2. Compatibility gate

Hermetic full upstream suite: `./test` → `tests/run-all` (+ Rash drivers).

**Bar (Peter):** **100% compatibility unless physically impossible**; any
divergence is an explicit, carefully weighed decision with a suite/DIVERGENCE
record — never an accidental byproduct of the rewrite.

---

## 3. Naming: `cmd`, not `env`

| Name | Meaning |
|---|---|
| **`cmd`** | Command-execution lifecycle state threaded through middleware |
| **`environ` / shell variables** | Bash’s existing variable environment — untouched by this name |

Brevity kept; Rack’s `env` deliberately avoided.

### 3.1 `cmd` sketch

| Field | Meaning |
|---|---|
| `ast` / IR | lowered command representation (Lua-native after migration) |
| `stage` | expand → redirect → dispatch → wait → done |
| `words` | expanded argv when known |
| `redirects` | resolved path/fd intents |
| `stdin` / `stdout` / `stderr` | classical fds + optional captures |
| `status` | exit status when known |
| `job` | job-control identity |
| `caps` | policy / approval flags |

```lua
function (cmd, next)
  -- mutable middleware may change documented cmd fields
  local result = next(cmd)
  return result
end
```

---

## 4. Blessed stack + userland read-only (physics over policy)

| Layer | Who edits | Mutates `cmd`? | Returns new `cmd`? |
|---|---|---|---|
| **Blessed stack** | root-owned files only (same discipline as enforcing hooks) | yes | yes — full Rack |
| **Userland read-only middleware** | unprivileged paths, if present | **no** — receives a **copy** | **no** — not in the return path |

Enforcement by **unreachability**, not honor system:

- Userland modules are invoked by a **blessed** wrapper middleware that deep-copies (or freeze-proxies) `cmd` into the child.
- Child API simply **has no** mutators / no `next` that accepts a replacement `cmd`.
- Optional: run userland in a separate Lua state with only read libraries.

Policy power example: “allow user read-only observers” = blessed layer that
loads `~/.config/rash/observe/*.lua` and calls them with copies. Agents cannot
edit the blessed order without root.

Default blessed order is versioned and named (e.g. `expand → audit → approve →
redirect → dispatch`). Alternate orders are root-configured profiles, not
inheritable env tokens.

---

## 5. Forking without modifying LuaJIT’s jitter

**Do not fork “inside” LuaJIT** in the sense of `fork(2)` from Lua mid-trace and
expect both sides to keep JIT-compiling happily. LuaJIT does not specially
virtualize fork; the child inherits address space including mcode/traces, and
continuing dual LuaJIT worlds after fork is a known footgun.

**Intended model:** fork remains a **C port** called from middleware:

```text
Lua middleware: “run these pipeline stages”
    → rash_c.pipeline_fork(cmd_ir_slice)
         → Bash/C make_child / fork / jobctl (existing physics)
         → parent waits / wires pipes
         → child either execve’s or re-enters a minimal runner
```

Middleware describes *intent*; C performs *process graph*. No LuaJIT JIT patch
required. Spike item: prove pipe fork-before-expand parity via that port while
`cmd` IR still lives in Lua for the parent’s planning side.

---

## 6. Stay-in-Lua vs marshalling

| Strategy | Role |
|---|---|
| Hybrid lower once `COMMAND *` → Lua IR | **Target** — eliminates hot-path marshalling |
| COMMAND* userdata forever | Incremental only — fights LuaJIT performance thesis |
| Pure Lua parse | Out of scope initially; parse.y stays |

---

## 7. Migration phases (overview)

Each phase: `./test` / `run-all` green (or an explicit divergence decision).
Detailed ownership / crossover map: **§7A**.

| Phase | Deliverable |
|---|---|
| 0 | Spec accepted or shelved |
| 1 | Outer Rack: `rash.exec(cmd)` whose default `next` = today’s C execute (identity) |
| 2 | Dispatch via ports (builtin/function/disk) |
| 3 | Expand port / partial Lua expand |
| 4 | Redirect port |
| 5 | Full lower to Lua IR; blessed stack owns order |
| 6 | Userland read-only observers via copy physics |
| 7 | Shrink C where suite proves parity |

Structured FDs are **not** gated on this rewrite (see `FUTURE_IDEAS.md`); they
become easier *after* phase 5+, but can be prototyped earlier on current seams.

**Effort calibration (Peter 2026-09-10):** “multi-month” is **human** calendar.
For an AI pair on this repo the same arc is closer to **multi-day** wall-clock
(suite farming still dominates). Do not use human-month estimates to talk
ourselves out of an accepted direction — use suite green / phase gates instead.

---

## 7A. Interim ownership and crossover points

Goal of this section: while migrating, **LuaJIT owns some slices and C owns
others**. Every phase must name (1) the single choke point where control
crosses, (2) what data crosses, (3) what must *not* be dual-implemented yet,
(4) which suite classes are the canaries.

Invariant for all interim phases: **one authoritative executor per concern**.
Never “Lua expand *and* C expand both live on the same word list.” Dual paths
are how suites go intermittently red.

### 7A.0 Today (baseline)

| Concern | Owner |
|---|---|
| Parse (`parse.y` → `COMMAND *`) | C |
| Outermost execute entry | C (`execute_command` → hooks wrap → `execute_command_internal`) |
| Expand / redirect / dispatch / fork / wait | C |
| Policy sensors | Lua (opt-in), called from C seams |

Crossover today: C → Lua (ctx in) → Lua ports / deny → C continues. No Lua-owned pipeline.

### 7A.1 Phase 1 — Outer Rack identity (first real crossover)

**Lua owns:** the *middleware stack shell* around one outermost command.  
**C owns:** everything inside `execute_command_internal` (unchanged).

```
execute_command / eval loop
    → build cmd { ast = COMMAND* userdata }
    → Lua: blessed_stack(cmd, next)
         → default next = rash_c.execute_command_internal(cmd.ast, …)
    → status back to C eval loop
```

| Crossing | Direction | Payload |
|---|---|---|
| Enter stack | C → Lua | `COMMAND *` handle, async/pipe flags, fd bitmap ref |
| Default `next` | Lua → C | same handle + flags |
| Return | C → Lua → C | integer status only |

**Must not yet:** Lua expand, Lua redirect, Lua fork planning, IR lower.  
**Today’s hooks:** either (a) remain C-called seams inside the C body, or (b)
become the *only* middleware layers whose `next` is still full C — prefer (b)
only after they are rebased onto `cmd` without behavior change.  
**Canary suite:** entire `run-all` must be bit-identical in intent (exit codes +
`.right` files). This phase is worthless if anything drifts.  
**Rollback:** compile flag / runtime `RASH_EXEC=c` skips Lua stack entirely.

Phase 1 success = “we can insert a no-op Lua layer and nobody can tell.”

### 7A.2 Phase 2 — Dispatch crossover (builtin | function | disk)

**Lua owns:** choice of leaf after C (or Lua) has produced expanded words.  
**C owns:** `execute_builtin`, `execute_function`, `execute_disk_command` /
`shell_execve` bodies as **ports**.

Interim shape for a **simple** command only (connections still 100% C):

```
C execute_simple_command … through expand_words …
    → hand expanded WORD_LIST + redirects to Lua dispatch middleware
    → Lua next = one of:
         rash_c.builtin(name, words)
         rash_c.function(name, words)
         rash_c.disk(path, argv, redirects)
```

| Crossing | Payload |
|---|---|
| C → Lua | expanded words (copy or userdata), redirect list, flags |
| Lua → C port | name/path + argv + redirect intent |
| C → Lua | status |

**Still C-owned:** pipelines (`execute_pipeline`), connections (`;`, `&`, `&&`,
`||`), loops/case/if, job control, expand itself.  
**Canaries:** `run-builtins`, `run-func`, `run-execscript`, `run-type`,
`run-varenv`; plus identity that `cm_connection` paths never enter Lua dispatch
yet.  
**Trap:** do not let Lua “almost” own pipelines here — fork-before-expand will
bite. Connections stay behind the phase-1 default `next` into full C.

### 7A.3 Phase 3 — Expand crossover

**Lua owns:** decision *when* to expand and optional Lua expand for a
**declared subset** (e.g. simple unquoted words).  
**C owns:** `expand_words` as the default port; full bashism expand remains C
until subset proofs exist.

```
Lua expand middleware
    → rash_c.expand_words(word_list_handle)   -- default
    → OR lua_expand_simple(words) for allowlisted shapes only
    → write cmd.words
    → next(cmd)  -- dispatch from phase 2
```

| Crossing | Payload |
|---|---|
| Lua → C expand | `WORD_LIST *` (or IR words lowered to WORD_LIST once) |
| C → Lua | new `WORD_LIST *` / string table |

**Dual-path rule:** a command shape uses **either** C expand **or** Lua expand,
selected by a pure classifier; never both. Classifier tests over sets (not one
example).  
**Canaries:** `run-exp`, `run-new-exp`, `run-quote*`, `run-nquote*`, `run-glob*`,
`run-comsub*`, `run-arith*`. Comsub and quoted forms stay on C expand until
explicitly migrated.  
**Rollback:** classifier always returns “use C.”

### 7A.4 Phase 4 — Redirect crossover

**Lua owns:** redirect *policy* and intent list on `cmd`.  
**C owns:** `do_redirections` / `redir_open` as apply ports (open, dup, close).

```
Lua redirect middleware
    → may deny / snapshot (today’s on_clobber logic moves here)
    → rash_c.apply_redirects(cmd.redirects)
    → next(cmd)
```

Clobber undo / download-pipe rebase onto `cmd` fields instead of ad-hoc seams.  
**Canaries:** `run-redir`, `run-heredoc`, `run-herestr`, `run-vredir`, plus Rash
`clobber-undo` / `installer-approval`.  
**Still C:** heredoc temp file creation details inside the apply port.

### 7A.5 Phase 5 — IR lower (marshalling ends)

**Lua owns:** `cmd` IR for the whole execute path of migrated shapes.  
**C owns:** parse only (`COMMAND *` → one-shot `rash_c.lower(ast) → IR`), plus
leaf ports (fork/exec/open/bind/wait).

```
parse.y → COMMAND *
    → rash_c.lower(COMMAND*) → Lua IR once
    → blessed middleware stack (all Lua)
    → ports only at kernel/shell-global boundary
```

**Crossover shrinks to:** lower (in), and ports (out). No WORD_LIST traffic in
the middle.  
**Migrate by shape family**, not by percentage of lines:

1. `cm_simple` non-pipe, no assign-prefix  
2. `cm_simple` with assign-prefix  
3. `cm_connection` `;` / `&&` / `||`  
4. `|` pipelines (fork port must preserve fork-before-expand)  
5. compound (`if`/`while`/`for`/`case`/`subshell`/`group`/`coproc`)

Each family flips behind a classifier; unmigrated families use phase-1 “full C
next.” **One family at a time; suite green before the next.**  
**Canaries:** widen with each family; pipelines + `run-lastpipe` + `run-jobs` +
`run-trap*` before declaring pipe IR done.

### 7A.6 Phase 6 — Userland read-only

No new execute ownership. Blessed middleware loads user observers with **copies**
of `cmd`; no mutator API in that state.  
**Canaries:** existing hook trust tests (`run-hooks`, deny/unowned); plus new
tests that userland cannot change argv/status.

### 7A.7 Phase 7 — Shrink C

Replace individual ports with Lua only where a **differential** against C port
on the suite (and targeted micro suites) matches. Prefer keeping fork/jobctl/
signal-adjacent code in C indefinitely.

### 7A.8 What today’s hooks become at each stage

| Today | Phase 1–2 | Phase 4–5 |
|---|---|---|
| `rash.hook` | middleware layer wrapping default C `next` | layer on IR |
| `before` / `after` | stay C-seam *or* thin middleware around simple dispatch | IR layers |
| `before_pipeline` | middleware before C `execute_pipeline` port | IR pipeline layers |
| `on_redirect` / `on_clobber` | stay in C apply path until phase 4 | redirect middleware |
| `on_download_pipe` | special-case middleware calling capture/exec ports | same, on IR |
| `on_builtin` / `on_function` / `on_exec` | fold into dispatch middleware | same |
| `on_stdio_bundle` | middleware after leaf ports return | same; structured FDs later |

### 7A.9 Dual-stack danger (explicit non-goals for interim)

- Two expanders both touching one command  
- Lua planning a pipe graph while C `execute_pipeline` also runs  
- Partial IR where `cmd.words` is Lua but redirects still secret C pointers without a port  
- “Temporary” env `RASH_LUA_EXEC=1` as a forgeable bypass — use compile-time or root-only profile if a kill switch is needed  

### 7A.10 Suggested spike order when un-shelved

1. Phase 1 identity only + `RASH_EXEC=c` kill switch + full `./test`  
2. Rebase **one** existing hook (`rash.hook`) as middleware with C `next`  
3. Stop. Re-evaluate. Only then phase 2 simple-dispatch.  

That keeps the interim tractable: each crossover is a **narrow waist**, not a
vague “Lua owns more now.”

### 7A.11 C prep landed (2026-09-10, pre-Lua)

Light extraction toward future stages **without** a C Rack registry:

- `rash_stage_expand_simple_words()` — expand waist
- `rash_stage_resolve_simple()` — special-builtin / function / `command` prefix
- Comments mark dispatch; redirect still inside builtin/disk **ports**
- Pins: `tests/exec-stages.*` (+ POSIX assign persist, function shadows builtin,
  `%job` vs disk, unknown-command 127, pipeline builtin, case match/;&)
- Dispatch/case/disk in `execute_cmd.c` no longer use `goto`. Flags replace
  `%job` skip, autocd retry, `EX_DISKFALLBACK`, and cleanup; `leave_case`
  replaces `EXIT_CASE` (outer `break` required); restricted `/` shares parent
  cleanup sequentially instead of `parent_return`.
- `rash_stage_dispatch_simple()` — named waist over that flag machine
  (`struct rash_simple_dispatch` is a parameter cluster, not a Rack `cmd`).
- `rash_stage_apply_simple_redirects()` — redirect **port** (parent undoable vs
  child permanent). Not a pre-dispatch stage: builtins restore fds after;
  disk applies in the child after fork. `on_redirect` / `on_clobber` stay
  inside `do_redirections`. Compound-command and null-command apply still
  call `do_redirections` directly.

---

## 8. Hard problems (unchanged physics)

Fork-before-expand, `set -e` / traps / `longjmp`, job control, comsubs, POSIX +
bashism oracle. LuaJIT speed does not shrink the suite.

---

## 9. Resolved vs open (2026-09-10)

| Topic | Resolution |
|---|---|
| Lifecycle object name | **`cmd`** |
| Compatibility | **100%** unless physically impossible → explicit decision |
| Userland middleware | Read-only via **copy + no mutator API** (physics) |
| Fork | **C port**, not LuaJIT JIT changes |
| Structured FDs | Backlog / `FUTURE_IDEAS.md` — flesh out separately; easier post-rewrite but not blocked on it |
| Full rewrite vs sensors | Still a **priority** choice — implementing other ideas does **not** require this path |

---

## 10. Decision rule

**Accept** only with explicit prioritization over nearer shipping work and
acceptance of a **suite-bound** migration starting at phase 1 identity
(human: months; AI pair: on the order of **days** of wall-clock — still
gated by `run-all`, not by vibes).

**Otherwise** keep this as north star; continue sensors/ports + backlog
structured FDs.
