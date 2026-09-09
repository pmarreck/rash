# Hook seams catalog

What Lua can see and must return at each lifecycle point. Companion to
`HOOKS_DESIGN.md` (policy/trust) and `INSTALLER_APPROVAL.md` (one product use).

**Conventions**

- Lua sandbox: `base` + `string` only (no `io` / `os` / FFI). Side effects go
  through C ports (`rash.deny`, `rash.spawn`, `rash.approve_bytes`, …).
- **Enforcing** (root-owned hook file, or `RASH_HOOK_ENFORCE_UNOWNED=1`):
  `rash.deny` / handler errors fail closed. **Advisory**: deny ignored; errors
  usually fail open (except download-pipe, which refuses fall-through).
- Nested `execute_command` while parse-stage `run()` is active does **not**
  re-enter parse-stage hooks (`hook_execution_depth`). Sensors that are not
  gated on depth (redirect, download-pipe, before/after) may still fire.
- “Return” below means Lua return values **and/or** side effects via ports /
  `rash.deny`. Unless noted, callbacks may return nothing.

---

## Implemented

### 1. Parse-stage — `rash.hook(fn)`

| | |
|---|---|
| **Where** | Outermost `execute_command` entry, before `execute_command_internal` |
| **When** | Post-parse, **pre-expansion**, pre-exec of that command tree |
| **Receives** | `cmd` userdata (`kind`, `words`, `redirects`, `left`/`right`/`connector` for connections); `run` closure |
| **Words** | Unexpanded (`$FOO` stays `$FOO`). Aliases already substituted at parse. |
| **Must / may** | Call `run()` zero or one time; may `rash.deny` before `run()`; may return exit status integer after `run()` |
| **Good for** | Structural policy (pipelines, sudo\|tee shapes, deny-lists on literals) |
| **Not for** | Values of variables; live pipe bytes; expanded argv |

```lua
rash.hook(function(cmd, run)
  -- inspect cmd; optionally rash.deny("…"); return
  return run()
end)
```

### 2. Expanded before — `rash.before(fn)`

| | |
|---|---|
| **Where** | `execute_simple_command`, after `expand_words`, before builtin/function/disk exec |
| **When** | Per **simple** command only |
| **Receives** | `ctx.words` (expanded argv table), `ctx.line` (joined string) |
| **Words** | Fully expanded (vars, globs, command subs). Sensitive. |
| **Must / may** | May `rash.deny` (aborts this simple command). No return value required. |
| **Good for** | Allowlists on real paths/args; deny after seeing values |
| **Not for** | Pipeline members: Bash forks pipe stages **before** expand; we skip before when `already_forked` (so `curl\|bash` stages never see `before`) |

### 3. Expanded after — `rash.after(fn)`

| | |
|---|---|
| **Where** | End of `execute_simple_command` (parent path only) |
| **When** | After the simple command finishes |
| **Receives** | `ctx.words`, `ctx.line`, `ctx.status`; `ctx.stdout` / `ctx.stderr` (strings or `nil`) |
| **Stdio** | Captured only when **not** already in a pipeline/async fork; cap **64 KiB** each |
| **Must / may** | Observe only (deny after the fact does not un-run the command). No required return. |
| **Good for** | Audit, metrics, soft warnings |
| **Not for** | Intercepting pipe plumbing; replacing `capture` for arbitrary FD wiring |

### 4. Redirect sensor — `rash.on_redirect(fn)` / `rash.on_clobber(fn)`

| | |
|---|---|
| **Where** | `redir.c` after path resolve, **before** `open(2)` |
| **Receives** | `ctx.path`, `ctx.fd`, `ctx.kind`, `ctx.exists`, `ctx.will_clobber` |
| **Kinds** | `output`, `force`, `append`, `input`, `err_and_out`, `append_err_and_out`, `input_output`, … |
| **Must / may** | May `rash.deny` (→ `RASH_DENIED_REDIRECT`). May call `rash.snapshot_file` / later `rash.undo_last`. |
| **Good for** | Clobber undo, path policy on final lexical path |
| **Not for** | Fd-dup/close (`>&2`, `n<&-`) — no path |

`on_clobber` is the same event filtered to `will_clobber == true`.

### 5. Download→shell — `rash.on_download_pipe(fn)`

| | |
|---|---|
| **Where** | Top of `execute_pipeline` (replaces normal pipe for matching shapes) |
| **When** | After shape match; producer has been run and **fully buffered**; consumer not started |
| **Receives** | `ctx.bytes` (producer stdout), `ctx.producer_argv`, `ctx.consumer_argv` (from **unexpanded** AST words) |
| **Must / may** | Should `rash.approve_bytes` then `rash.exec_with_stdin(consumer_argv, bytes)`, or `rash.deny`. If it neither denies nor `exec_with_stdin`, C **fail-closes** (no fall-through into a real pipe). |
| **Shape** | `simple\|simple` only; producer basename `curl`\|`wget`; consumer `bash`\|`sh`\|`rash` |
| **Good for** | Installer review (exact buffered bytes) |
| **Not for** | `"$URL"\|bash` (unexpanded); nested `a\|b\|bash` |

---

## Candidate seams (not implemented)

Each row: **hook name (proposed)**, location, what Lua would get, return contract, why it might matter.

### A. `rash.before_pipeline` — expanded argv, pipe not started

| | |
|---|---|
| **Where** | Parent, top of `execute_pipeline` (beside download-pipe), **before** forking stages |
| **Receives** | Expanded copies of left/right argv (and maybe full connection shape); optional redirect summaries |
| **Must / may** | `allow` / `deny` / or “hand off to download-pipe”. Deny aborts whole pipeline. |
| **Why** | Closes `"$CURL" \| bash` gap; allowlists on real URLs |
| **Cost** | Expanding in the parent early: must not apply assignment words to parent; command-subs on the right run earlier than stock Bash |

### B. `rash.on_exec` — final execve identity

| | |
|---|---|
| **Where** | `shell_execve` / just before `execve` in `execute_disk_command` child |
| **Receives** | Resolved pathname, argv, selected env keys, optional content hash |
| **Must / may** | `allow` / `deny` (deny → exit 126/127-class). No mutation of argv unless explicitly designed. |
| **Why** | Safety C content-hash allowlist; stops path-only lies |
| **Cost** | Runs in child after fork for pipes; hashing cost; interpreter/`ld.so` policy still separate |

### C. `rash.on_builtin` / `rash.on_function`

| | |
|---|---|
| **Where** | `execute_builtin` / `execute_function` entry |
| **Receives** | Builtin name or function name; expanded `words` |
| **Must / may** | `allow` / `deny`; optional wrap |
| **Why** | `eval`, `source`, `exec`, `cd` to sensitive paths; function shadowing |
| **Cost** | High call volume; keep predicates cheap |

### D. `rash.before_simple` for pipe children (opt-in)

| | |
|---|---|
| **Where** | Same as today’s before, but also when `already_forked` / `SUBSHELL_PIPE` |
| **Receives** | Expanded words for **that** stage only |
| **Must / may** | deny that stage (awkward mid-pipe) |
| **Why** | Per-stage expanded policy inside pipelines |
| **Cost** | Denial mid-pipe is hard to reason about; usually worse than `before_pipeline` |

### E. `rash.on_stdio_bundle` — capture triple → transform → FD  
*(Peter 2026-09-09: replace hairy `capture` )*

**Problem.** `~/dotfiles/bin/src/capture.bash` juggles nested command substitutions and FDs 3/4 to get `out` / `err` / `rc` into caller locals, optionally through `printable-binary`. It works; it is hard to maintain.

**Idea.** When a command (or group) is “connected” to a designated FD, Rash:

1. Runs it with stdout/stderr captured (and records status), like a stronger `after`.
2. Invokes Lua with the triple (and argv).
3. Lua builds structured JSON (binary fields via printable-binary encoding).
4. C writes that payload to the designated FD (and/or suppresses the original stdio).

| | |
|---|---|
| **Where** | Generalization of today’s after-capture path in `execute_simple_command`, or a wrapper around a simple/group when a **watch FD** is open / a magic redirect is present |
| **Trigger options** (pick later) | (1) env `RASH_CAPTURE_FD=9`; (2) open FD N marked at session start; (3) redirect sugar e.g. `cmd 9>@rash-json`; (4) Lua registration `rash.on_stdio_bundle(fd, fn)` |
| **Receives** | `ctx.stdout`, `ctx.stderr`, `ctx.status`, `ctx.words` / `ctx.line`; maybe `ctx.fd` (destination); caps + truncation flags |
| **Must / may** | Return a string (JSON) **or** call `rash.write_fd(fd, bytes)`. If return string, C writes it to the watch FD. Encoding of NULs/binary is Lua’s job (printable-binary); C must pass raw bytes with `lua_pushlstring` / write full length. |
| **Why** | `local out err rc; capture cmd` → `cmd` with FD convention + `jq` / read JSON from FD |
| **Cost** | Caps (default larger than 64 KiB?); pipelines (capture whole pipeline vs forbid); interaction with existing `after`; must not forge a bypass by “capture FD means skip policy” |

Sketch (illustrative, not shipped):

```lua
rash.on_stdio_bundle(function(ctx)
  -- ctx.stdout / ctx.stderr may contain NULs; treat as byte strings
  local out_enc = printable_binary.encode(ctx.stdout)  -- if library exposed to hooks
  local err_enc = printable_binary.encode(ctx.stderr)
  return string.format(
    '{"rc":%d,"out":%q,"err":%q}\n',  -- real impl: proper JSON + encoding fields
    ctx.status, out_enc, err_enc)
end)
-- C writes return value to the designated FD
```

Exposing printable-binary into the sandbox needs an explicit port or a
carefully loaded pure-Lua codec (no FFI) — hooks currently forbid FFI.

### F. `rash.on_assignment` / nameref / `declare`

| | |
|---|---|
| **Where** | Assignment word application / `bind_*` |
| **Receives** | Name, value (sensitive!), attributes |
| **Why** | Secret-in-env audit; readonly violations |
| **Cost** | Extremely hot; easy to leak secrets into logs |

### G. Job / wait / trap seams

| | |
|---|---|
| **Where** | `wait_for`, trap dispatch |
| **Receives** | Job id, status, signal |
| **Why** | Agent job supervision |
| **Cost** | Overlaps existing trap machinery; easy to break job control |

---

## Ports available to hooks (implemented)

| Port | Role | Typical stage |
|---|---|---|
| `rash.warn` | stderr message | any |
| `rash.deny(reason)` | abort current stage | hook / before / redirect / download-pipe |
| `rash.spawn(argv)` | fork/exec, capture out/err (64 KiB), no hook re-entry | any |
| `rash.snapshot_file` / `rash.undo_last` | clobber preimage stack | redirect / clobber |
| `rash.approve_bytes` / `rash.exec_with_stdin` | installer review + exact stdin exec | download-pipe |

---

## Design checklist for a new seam

1. What is available **without** expanding secrets? Prefer that default.
2. Does it run in parent or already-forked child?
3. Fail closed vs open for enforcing vs advisory.
4. Can Lua express the policy with return values only, or does it need a new C port?
5. Interaction with pipelines, async, and `hook_execution_depth`.
6. Cap sizes and truncation signaling (never silent truncate for security claims).
7. Document the seam here before coding; add a classifier-set test, not one happy path.

---

## Revision

| Date | Change |
|---|---|
| 2026-09-09 | Initial catalog: implemented seams + candidates; capture/FD JSON idea (E) |
