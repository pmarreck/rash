# PLAN

Open work, in order. The pre-2026-10-14 checklist remains at `122080aa:PLAN.md`.
Completed items after this rewrite go to `docs/PLAN_LOG.md`.
Background for an item is in `docs/plan_context/`. Purpose stays in `INTENT.md`.

## Rack

- [ ] Finish Rack phase 4 after the deny-before-apply waist: Lua redirect intent list, clobber snapshot on that middleware, download-pipe rebased onto cmd. (context: docs/plan_context/rack-phase4.md)
- [ ] Rack phase 5: lower simple commands to a Lua IR so the blessed stack owns order. (context: intents/luajit-rack-executor.md)
- [ ] COW / OverlayFS workspace launcher, after the phase 4 redirect slice is green. Do not start it during that slice. (context: MUTATION_SAFETY_OPTIONS.md)
- [x] Rack phase 4 waist 1: `rash.on_redirect_policy` may deny before `do_redirections`; nil uses the C port once. (done 2026-10-14 14:50 EDT)
- [x] Rack phase 3: one Lua or C expand path per simple command. (done 2026-09-28, 122080aa)
- [x] Rack phase 2: `rash.on_dispatch` chooses builtin or disk after expand. (done 2026-09-28, 90ead6cf)
- [x] Rack phase 1: `rash.use` around outermost execute, including `-c`. (done 2026-09-28)

## Safety holds

- [ ] Content-hash exec allowlist on `on_exec` (Safety C). Not the active slice.
- [ ] Effect-scoped passkey approvals stay shelved until an owner decision. (context: docs/plan_context/passkey-approvals.md)
- [ ] Fourth SIGCHLD loss and the trap allocator stay documented, not patched, and not mailed. (context: docs/plan_context/sigchld-remainder.md)

## Later

- [ ] Confirm rash-codex, rash-id, and rash-sig2 have no unique unmerged work before removing those worktrees. (context: docs/plan_context/stale-worktrees.md)
- [ ] Goto cleanup, emit-ast v2, Zig assessment, structured FDs, and the live-surface rename stay deferred. (context: docs/plan_context/deferred-cleanup.md)
