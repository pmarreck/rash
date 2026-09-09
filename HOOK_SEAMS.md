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

## Implemented (lifecycle batch, 2026-09-09)

See `tests/lifecycle-hooks.*` for acceptance.

### 6. `rash.before_pipeline(fn)` — expanded argv, pipe not started

| | |
|---|---|
| **Where** | Parent, top of `execute_pipeline`, **before** download-pipe and forking |
| **Receives** | `ctx.left_words`, `ctx.right_words` (expanded), `ctx.connector` |
| **Must / may** | May `rash.deny` → abort whole pipeline |
| **Why** | Closes `"$CURL" \| bash` gap |

### 7. `rash.on_exec(fn)` — final execve identity

| | |
|---|---|
| **Where** | `shell_execve`, before `execve` |
| **Receives** | `ctx.path`, `ctx.words` (argv) |
| **Must / may** | May `rash.deny` → no execve, failure exit |
| **Why** | Path/bytes allowlists (Safety C foundation) |

### 8. `rash.on_builtin(fn)` / `rash.on_function(fn)`

| | |
|---|---|
| **Where** | `execute_builtin` / `execute_function` entry |
| **Receives** | `ctx.name`, `ctx.words` |
| **Must / may** | May `rash.deny` → skip body |
| **Why** | `eval` / `source` / dangerous functions |

### 9. `rash.on_stdio_bundle(fn)` — capture triple → FD  
*(replaces hairy `capture.bash`)*

| | |
|---|---|
| **Where** | After simple-command (same capture constraints as `after`) |
| **Trigger** | Handlers registered **and** `RASH_CAPTURE_FD=<n>` |
| **Receives** | `ctx.stdout`, `ctx.stderr`, `ctx.status`, `ctx.words` |
| **Must / may** | **Return a string**; C writes it to FD `n` |
| **Why** | Structured out/err/rc without FD juggling |

```lua
rash.on_stdio_bundle(function(ctx)
  return '{"rc":' .. ctx.status .. ',"out":"...","err":"..."}'
end)
-- RASH_CAPTURE_FD=3 cmd 3>result.json
```

## Deferred candidates

### D. `rash.before` for pipe children

Usually worse than `before_pipeline` (deny mid-pipe). Deferred.

### F. `rash.on_assignment` / nameref / `declare`

| | |
|---|---|
| **Where** | Assignment word application / `bind_*` |
| **Receives** | Name, value (sensitive!), attributes |
| **Why** | Secret-in-env audit; readonly violations |
| **Cost** | Extremely hot; easy to leak secrets into logs |
| **Status** | Deferred |

### G. Job / wait / trap seams

| | |
|---|---|
| **Where** | `wait_for`, trap dispatch |
| **Receives** | Job id, status, signal |
| **Why** | Agent job supervision |
| **Cost** | Overlaps existing trap machinery; easy to break job control |
| **Status** | Deferred |

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
| `rash.deny(reason)` | abort current stage | hook / before / before_pipeline / on_builtin / on_function / on_exec / redirect / download-pipe |
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
| 2026-09-09 | Landed `before_pipeline`, `on_exec`, `on_builtin`/`on_function`, `on_stdio_bundle` |
