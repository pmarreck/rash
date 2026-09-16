# Future ideas (backlog)

Not scheduled. Flesh out before promoting to `PLAN.md` checkboxes.
Related accepted work stays in `PLAN.md` / `intents/`.

---

## Structured-data FDs

**Status:** backlog — needs more design (Peter 2026-09-10).  
**Easier after** a LuaJIT middleware executor (`intents/luajit-rack-executor.md`),
but **implementable earlier** by extending today’s `on_stdio_bundle` /
capture path.

### Sketch

Reserve three well-known fds (numbers TBD; must not collide with casual scripts):

| Role | Purpose |
|---|---|
| Structured **in** | Typed request channel into a command |
| Structured **out** | Typed success payload |
| Structured **err** | Typed error payload (≠ human stderr) |

- Classical 0/1/2 remain the human interactive contract.
- Opt-in via profile / shell option / explicit fd assignment — default off.
- Binary fields: **printable-binary** encoding (no third scheme).
- Overlap today: `rash.on_stdio_bundle` + `RASH_CAPTURE_FD` is a thin “bundle
  stdout/stderr/rc → bytes on one FD” prototype of the **out** side only.

### Open design questions

1. Fixed fd numbers vs dynamic advertisement (`RASH_STRUCT_OUT=…`)?
2. Framing: JSON lines, length-prefixed blobs, or both?
3. Which commands speak structured out by default vs opt-in per invocation?
4. How structured-err interacts with `set -e` and pipefail?
5. MFIC: golden vectors for encode/decode; no silent truncation.

### Non-goals (until specified)

- Replacing stdout for interactive humans
- Requiring structured fds for POSIX scripts
- Implementing this as a reason to start the full Rack rewrite

---

## Symlink replaced by a regular file

**Status:** slice 2 shipped 2026-09-16 (`deny_symlink_replace.lua`). Remaining: expose `is_symlink` on redirect ctx (slice 1); in-process Write tools still out of scope.

Agents (and some editors) “write” by creating a temp file and `rename(2)` onto the destination. That **replaces the symlink inode**. `echo x > link` does **not**: `open(2)` follows, the link stays, the target is truncated.

Rash already `lstat`s in `rash_hooks_on_redirect`. A symlink is not `S_ISREG`, so `will_clobber` is false. Extending `on_clobber` would not catch the real bug.

| What | Rash can see? |
|---|---|
| `>` / `>>` / `>|` onto a symlink | Yes, redirect sensor. Follows; link survives. |
| `mv tmp dest` / `cp` replace / `install` | Yes if spawned through Rash (`before` / `on_exec`). |
| Editor/agent Write via `rename` inside another process | No, unless that process is a child we exec and we wrap the tool. |
| `ln -sf` | Yes; usually intentional. |

---

## Other parked notes

*(Add stubs here rather than inflating PLAN.md.)*
