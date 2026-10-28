# Stale linked worktrees

Left in place on 2026-10-14. Do not delete them from an inbox pass.

- `rash-codex`: branch `codex-sigchld` at `075aa54e`. Its two July 31 inbox notes were the SIGCHLD race and the request to break the fix. Both are answered by the master fixes. `git status -uno` showed no tracked edits.
- `rash-id`: branch `bash-identity` at `9530eb54`.
- `rash-sig2`: branch `sigchld-inprogress` at `63d1f5ff`. No inbox.

All three tips are ancestors of master `122080aa`. Uncommitted files in `rash-id` and `rash-sig2` were not listed. Do not remove the checkouts until that listing is done.

Their `PLAN.md` files repeat boxes that master already closed (purpose, identity, hooks, the second SIGCHLD fix) or that were superseded (mailing upstream). Do not reimplement those boxes. Removal waits on a check that no unique unmerged work remains.

2026-10-28: Listing done. All three trees were clean except for an ignored nix `result` link, and no process was running in them. BDFN approved removal. They were removed with `git worktree remove` and their branches with `git branch -d`.
