# Download-to-shell installer approval

Status: **design + first tests in progress** (2026-09-08). Opt-in only.
Does not authorize host-wide rollout. COW and passkey leases remain separate.

## Use case (Peter)

Intercept common `curl URL | bash` (and kin) so a human can review the
downloaded script **before any of it runs**, then execute the **exact**
bytes that were reviewed.

## Prior art

`~/dotfiles/bin/confirm` — manual middle of the pipe:

```bash
curl "https://example/install.sh" | confirm | bash
```

It tees stdin to a tempfile + stderr, prompts on `/dev/tty`, and on approval
cats the tempfile downstream. Rash should make the equivalent **automatic**
for recognized shapes when an opt-in hook is installed, without requiring the
human to remember `| confirm |`.

`inspect` in the same tree is unrelated (variable pretty-printer).

## Non-goals (honest boundary)

- Not a security boundary against a determined local user (they can run
  `/bin/bash` directly).
- Approval of script₁ is **not** approval of secondary downloads script₁ may
  fetch. State that in the UX.
- Does not replace COW / exec allowlists / passkey effect-leases
  (`inbox/2026-09-04-…-effect-scoped-approvals-with-passkeys`).
- Text/regex matching of the command line is insufficient; match the
  executor AST (pipeline connector + producer/consumer words).

## Required properties

1. **No partial execution.** Buffer the complete producer output (with size
   and time caps) before the consumer starts. Never forward-while-reviewing.
2. **Exact bytes.** Approval executes the reviewed buffer, not a second HTTP
   GET of the URL (TOCTOU / CDN swap).
3. **Fail closed** on download failure, truncation, oversize, timeout, missing
   TTY, cancel, or reject.
4. **Safe display.** Treat script bytes as hostile to the terminal (strip /
   neutralize CSI and other control sequences in the review pane).
5. **Argument preservation.** Consumer argv after `-s` / `--` (e.g.
   `bash -s -- --prefix=/usr`) must survive into the approved exec.
6. **Opt-in.** Packaged example hook; Bash-identical when unloaded.

## Supported shapes (v1 proposal — pending Peter)

| Producer words (basename) | Consumer words (basename) |
|---|---|
| `curl`, `wget` | `bash`, `sh`, `rash` |

Also match absolute paths whose basename is in that set
(`/usr/bin/curl | /bin/bash`). Parse-stage hooks see **unexpanded** words, so
`"$CURL" | bash` does not match until an expanded-stage seam exists — document
as a known gap. Unmatched forms for v1: `python -`, `node`, `powershell`,
`curl | tee file | bash`, `bash -c "$(curl …)"`.

## Architecture sketch

```
parse-stage rash.hook
  └─ recognize download|shell connection
       └─ do NOT call run()
            └─ C: run producer, capture stdout (cap)
                 └─ C: sanitize + show on stderr; prompt /dev/tty
                      ├─ reject/cancel → deny, no consumer
                      └─ approve → exec consumer argv with stdin = buffer
```

Business logic (which shapes, messaging) stays in Lua.
C ports stay inert: capture producer, review UI, exec-with-stdin-bytes.

### Proposed C ports

| Port | Role |
|---|---|
| `rash.capture_command(argv, opts)` | run argv; return `{status,stdout,stderr}` with hard byte cap (larger than today's 64KiB spawn cap, or a dedicated installer cap) |
| `rash.approve_bytes(bytes, meta)` | print sanitized review; Y/n on `/dev/tty`; return boolean |
| `rash.exec_with_stdin(argv, bytes)` | fork/exec argv with stdin fed from `bytes`; never re-enters hooks |

Pipe-buffer alternative (preferred for exactness when the producer is not a
pure URL fetch): intercept in `execute_pipeline` when a hook declares the
outer connection as a download-pipe. Same review/exec tail. Harder; better
fidelity for `curl -H Auth:…`.

## Unresolved policy choices (need Peter)

1. **Capture strategy for v1:** re-run producer argv via capture (simpler;
   loses nothing if argv is complete) vs true pipe buffer in `execute_pipeline`
   (exact stream, more C)?
2. **Approval UX for v1:** tty Y/n (like `confirm`) vs deny-only stub vs wait
   for passkey broker?
3. **Non-interactive sessions** (`rash -c`, no TTY): always deny?
4. **Size / time caps:** defaults? (proposal: 2 MiB script, 30s producer)
5. **Consumer set:** include `rash` itself? `zsh`? deny `bash -c "$(curl …)"`
   command-substitution form in the same hook or later?

## MFIC tests (initial)

- Classifier over a **set** of pipeline shapes (match / non-match), not a
  single happy example.
- Approve path runs consumer with byte-identical stdin; mutate one byte after
  "approval" in a test double and prove exec cannot see the mutation
  (approval seals a copy).
- Reject / cancel / no-tty → consumer never starts (marker file absent).
- Oversize / empty producer → deny.
- Ordinary `echo hi | cat` unaffected.
- CSI-laden script displays without executing escapes (assert sanitized view
  or that raw ESC did not reach the tty in the review path).

## References

- Inbox: `inbox/2026-09-08-from-einstein@thelio-nixos-idea-intercept-download-to-shell-pipelines-for-script-approval.frontmatter.md`
- Start auth: `inbox/2026-09-08-from-einstein@thelio-nixos-peter-requests-starting-the-download-to-shell-approval-idea.frontmatter.md`
- Related (later): effect-scoped passkey approvals note (2026-09-04)
