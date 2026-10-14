# Deferred cleanup

Not the active slice.

- Gotos outside `execute_cmd.c` (subst, braces, redir, bashline, pathexp, shell, parser) are off the lifecycle path. `execute_cmd.c` itself has no goto.
- `--emit-ast` v2 should recurse into `for`, `if`, `group`, `subshell`, `case`, `function_def`, and `coproc`, and name `W_*` flag bits. v1 is already shipped. Line numbers for some node types live on the per-type struct.
- A Zig 0.16 rewrite is an assessment only: boundaries, compatibility, sequencing, effort. No port.
- Structured file-descriptor capture is a backlog design in `FUTURE_IDEAS.md`. Flesh it out before scheduling it. It is easier after Rack phase 5 and is not blocked on that rewrite.
- Renaming the live Bash surface needs an explicit attribution decision. GPL headers and in-source authorship stay. Invoking the binary as `bash`, `sh`, or `rbash` keeps Bash help text, with one last-line `--help` note.
