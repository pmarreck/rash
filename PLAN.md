# PLAN

Open work, in order. The pre-2026-10-14 checklist remains at `122080aa:PLAN.md`.
Completed items after this rewrite go to `docs/PLAN_LOG.md`.
Background for an item is in `docs/plan_context/`. Purpose stays in `INTENT.md`.

## Rack

- [ ] Finish Rack phase 4: download-pipe rebased onto cmd, and decide whether policy may return a replacement redirect list. (context: docs/plan_context/rack-phase4.md)
- [x] Phase 4 slice B: pre-expand safe-shape redirect words once; policy rows gain path/exists/will_clobber; C applies them without re-expanding. (context: docs/plan_context/rack-phase4.md) (done 2026-10-23 11:20 EDT)
- [x] Phase 4 slice C: `rash.snapshot_file` once per path per simple-command apply; packaged undo hook snapshots at policy with on_clobber fallback. (context: docs/plan_context/rack-phase4.md) (done 2026-10-23 11:55 EDT)
- [ ] Rack phase 5: lower simple commands to a Lua IR so the blessed stack owns order. (context: intents/luajit-rack-executor.md)
- [ ] COW / OverlayFS workspace launcher, after the phase 4 redirect slice is green. Do not start it during that slice. (context: MUTATION_SAFETY_OPTIONS.md)
- [x] Policy and parse-stage redirect tables no longer read fd-number redirects (2>&1) as words; that segfaulted. (done 2026-10-23 10:45 EDT)
- [x] Rack phase 4 waist 1: `rash.on_redirect_policy` may deny before `do_redirections`; nil uses the C port once. (done 2026-10-14 14:50 EDT)
- [x] Rack phase 3: one Lua or C expand path per simple command. (done 2026-09-28, 122080aa)
- [x] Rack phase 2: `rash.on_dispatch` chooses builtin or disk after expand. (done 2026-09-28, 90ead6cf)
- [x] Rack phase 1: `rash.use` around outermost execute, including `-c`. (done 2026-09-28)

## Safety holds

- [ ] Content-hash exec allowlist on `on_exec` (Safety C). Not the active slice.
- [ ] Effect-scoped passkey approvals stay shelved until an owner decision. (context: docs/plan_context/passkey-approvals.md)
- [ ] Fourth SIGCHLD loss and the trap allocator stay documented, not patched, and not mailed. (context: docs/plan_context/sigchld-remainder.md)
- [ ] Widen safe redirect shapes to "$f" and $NAME/literal concatenations (e.g. $HOME/.ssh/x) so policy sees real paths.
- [ ] Silence the pre-existing -Wformat-truncation warnings in the hooks.c snapshot path builders.

## Later

- [x] 2026-10-28 14:15 EDT Retired rash-codex, rash-id and rash-sig2 after confirming clean trees and merged tips, and deleted nine local upstream-mirror branches identical to origin. (context: docs/plan_context/stale-worktrees.md)
- [ ] Goto cleanup, emit-ast v2, Zig assessment, structured FDs, and the live-surface rename stay deferred. (context: docs/plan_context/deferred-cleanup.md)
