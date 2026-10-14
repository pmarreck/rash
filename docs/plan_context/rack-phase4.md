# Rack phase 4

Accepted direction: `intents/luajit-rack-executor.md` section 7A.4.

Waist 1 is `rash.on_redirect_policy`. It runs inside `rash_stage_apply_simple_redirects` when the redirect list is non-empty. `rash.deny` skips `do_redirections`. Nil, or no registered policy hook, lets that C port run once. A Lua return value is not a second redirect list. `on_redirect` and `on_clobber` stay inside the port, so a denied apply does not snapshot.

Still open for the rest of phase 4: Lua owns the redirect intent list, clobber snapshot moves onto that middleware, and download-pipe rebases onto `cmd`. Heredoc temp files stay inside the apply port.

Canaries: `tests/rack-redirect.*`, plus `run-redir`, `run-heredoc`, `run-herestr`, `run-vredir`, clobber-undo, and installer-approval.
