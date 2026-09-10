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

## 7. Migration phases

Each phase: `./test` / `run-all` green (or an explicit divergence decision).

| Phase | Deliverable |
|---|---|
| 0 | Spec accepted or shelved |
| 1 | Outer Rack: `rash.exec(cmd)` whose default `next` = today’s C execute | identity |
| 2 | Dispatch via ports (builtin/function/disk) |
| 3 | Expand port / partial Lua expand |
| 4 | Redirect port |
| 5 | Full lower to Lua IR; blessed stack owns order |
| 6 | Userland read-only observers via copy physics |
| 7 | Shrink C where suite proves parity |

Structured FDs are **not** gated on this rewrite (see `FUTURE_IDEAS.md`); they
become easier *after* phase 5+, but can be prototyped earlier on current seams.

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
acceptance of multi-month suite-bound migration starting at phase 1 identity.

**Otherwise** keep this as north star; continue sensors/ports + backlog
structured FDs.
