# Rack phase 4

Accepted direction: `intents/luajit-rack-executor.md` section 7A.4.

Waist 1 is `rash.on_redirect_policy`. It runs inside `rash_stage_apply_simple_redirects` when the redirect list is non-empty. `rash.deny` skips `do_redirections`. Nil, or no registered policy hook, lets that C port run once. A Lua return value is not a second redirect list. `on_redirect` and `on_clobber` stay inside the port, so a denied apply does not snapshot.

Still open for the rest of phase 4: Lua owns the redirect intent list, clobber snapshot moves onto that middleware, and download-pipe rebases onto `cmd`. Heredoc temp files stay inside the apply port.

Canaries: `tests/rack-redirect.*`, plus `run-redir`, `run-heredoc`, `run-herestr`, `run-vredir`, clobber-undo, and installer-approval.

## Decisions, 2026-10-23 (BDFN)

A fd-number redirect (`2>&1`, `3>&-`) stores an int in the `REDIRECTEE` union. `rash_push_redirects` read it as a word pointer and segfaulted. That happened in the parse-stage `command.redirects` table since the 2026-08-11 lifecycle hooks, so the packaged `deny_sensitive_clobber.lua` crashed on `ls nope 2>&1`. It also happened in the policy rows. Those rows now have `word = nil`. Pins: `tests/deny-hooks.*` and `tests/rack-redirect.*`.

Slice B (safe-shape pre-expand). The policy stage sees unexpanded words, and `do_redirections` expands each word right before applying it. Expanding in the stage and again in the port would run `> $(cmd)` twice. Expanding every word up front changes Bash ordering (in `cmd <f >$(head -1)`, `head` reads `f`). So only safe shapes pre-expand. Every word-target redirect must be a literal or a plain `$NAME`, using phase 3's rule. The `$NAME` must be set, scalar, and expand to one field with no glob, quote, `$`, backslash, or tilde. fd-number dups and closes are allowed. A heredoc, here-string, `>&word`, or `{var}>` keeps the whole command on today's path. For a safe shape, policy rows gain `path`, `exists`, and `will_clobber`, and C applies those exact filenames on a disposable copy of the list. The parsed tree is not mutated.

Slice C (no double snapshot). `on_clobber` keeps firing inside the port exactly as before. Skipping it would let one hook file silently disable another file's `on_clobber` deny. Instead, `rash.snapshot_file` is once per path per simple-command apply, enforced in C. The packaged undo hook snapshots at policy and keeps `on_clobber` as the fallback for non-safe shapes.
