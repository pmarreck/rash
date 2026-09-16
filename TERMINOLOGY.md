# Rash terms

Not a general Unix glossary. Definitions that leak across `INTENT.md`, hooks docs, and tests.

| Term | Meaning |
|---|---|
| **Rash** | This Bash 5.3 fork. Backronym: Reversible Auditable/Agent-safe Shell. |
| **Presenting as bash** | `argv[0]` basename is `bash`, `sh`, or `rbash` (login `-bash` included). `--help` / `--version` / `--about` keep GNU Bash wording. Transparency note on `--help` only, after the option list. |
| **Presenting as rash** | Any other invocation name. Product name `Rash`; `Long options` not `GNU long options`. |
| **Enforcing hook** | Root-owned, not group/world-writable (or `RASH_HOOK_ENFORCE_UNOWNED=1` in tests). `rash.deny` aborts; Lua errors fail closed. |
| **Advisory hook** | Unowned file allowed only with `RASH_ALLOW_UNOWNED_HOOKS=1`. `rash.deny` ignored; errors usually fail open. |
| **Parse stage** | `rash.hook(cmd, run)` at outermost `execute_command`, unexpanded words, aliases already applied. |
| **`cmd` (Rack proposal)** | Lifecycle object for a future Lua middleware stack. Not Bash `environ`. Not accepted yet. |
| **`will_clobber`** | Truncating redirect onto an existing **regular** file (`lstat` + `S_ISREG`). A symlink dest is not this; `>` follows the link. |
| **Symlink replace** | `mv` / `cp` / `install` onto a dest that is a symlink and not a directory after follow. Rename replaces the link inode. Guarded by `deny_symlink_replace.lua`. |
| **`rash.spawn`** | C `fork`/`execvp` that never re-enters hooks. The unforgeable override; not an environment token. Returning its status from a parse-stage hook skips the original command. |
