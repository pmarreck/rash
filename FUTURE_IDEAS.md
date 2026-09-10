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

## Other parked notes

*(Add stubs here rather than inflating PLAN.md.)*
