Rash
====

[![Mechatron Prime CI](https://img.shields.io/endpoint?url=https%3A%2F%2Fthelio-nixos.tail66c90.ts.net%2Fbadges%2Frash.json&style=for-the-badge)](https://thelio-nixos.tail66c90.ts.net/mechatron-prime/)

**Rash** — **R**eversible **A**uditable/**A**gent-safe **S**hell, a fork of
Bash — is an opinionated, experimental fork of Bash 5.3. It preserves
Bash's shell semantics while developing hermetic validation, compiler
diversity, and a scoped reversible-mutation safety model.

Rash development uses hermetic Nix commands: `./build` and `./test` use the
GCC baseline; `./build --zig` and `./test --zig` compile the same C sources
with pinned Zig 0.16; `./bm` (and `./bm --micro` from `./test`) records
wall-clock, user-CPU, and system-CPU history and fails if a result leaves a
±15% window around the last three measurements on this machine.

## Lifecycle hooks (advisory + enforcing)

Rash embeds LuaJIT for post-parse command lifecycle hooks. Root-owned hooks
under `$prefix/share/rash/hooks` are **enforcing**: they may call `rash.deny`
and fail closed on errors. Packaged policies include `deny_sudo_tee.lua`
(blocks `… | sudo -S tee` with an input redirect) and
`deny_sensitive_clobber.lua` (blocks `>` onto lexical path words matching
`.ssh/`, `id_rsa`, and similar), and `deny_symlink_replace.lua` (blocks
`mv`/`cp`/`install` onto a non-directory symlink). `warn_sudo_tee.lua` remains
as an advisory example.

Enable the installed set:

	RASH_HOOK_DIR=/usr/local/share/rash/hooks rash -c '...'

User-owned hooks need `RASH_ALLOW_UNOWNED_HOOKS=1` and are **advisory only** —
`rash.deny` is ignored with a warning. `rash.spawn({prog, ...})` runs helpers
without re-entering the hook pipeline (no forgeable bypass env var). Lua gets
only the safe `base` and `string` libraries.

`rash.before` runs on simple commands immediately after `expand_words` (aliases
already substituted at parse time; variables/globs/command-subs resolved).
`rash.after` runs when the simple command finishes, with `status` and — when
the command is not already in a pipeline — capped `stdout`/`stderr` captures.
The expanded stage can see secrets; prefer structure-only policy at parse stage.

Opt-in `approve_download_pipe.lua` intercepts `curl`/`wget` → `bash`/`sh`/`rash`
pipelines: the producer is buffered, `rash.approve_bytes` reviews on a tty, and
`rash.exec_with_stdin` runs the consumer on those exact bytes
(`INSTALLER_APPROVAL.md`).

Additional lifecycle seams (see `HOOK_SEAMS.md`): `before_pipeline` (expanded
pipe argv), `on_builtin` / `on_function` / `on_exec`, and `on_stdio_bundle` with
`RASH_CAPTURE_FD` for structured out/err/rc capture (capture.bash replacement).

Redirect sensors fire after path resolve and before `open(2)`:
`rash.on_redirect` (all path-bearing redirects) and `rash.on_clobber` (only
when a truncating redirect would overwrite an existing regular file).
`rash.snapshot_file` / `rash.undo_last` store structured preimages under
`RASH_UNDO_DIR` (default `$TMPDIR/rash-undo-<pid>`), capped by
`RASH_UNDO_MAX_BYTES` (default 256 MiB). Snapshotting a not-yet-created path
makes undo unlink it. Packaged example: `undo_precious_clobber.lua` (opt-in;
shell redirects only — not a substitute for COW).

For hook development, `RASH_HOOK_RELOAD=mtime` checks the cached hook-file
manifest before each outermost parsed command and reloads only when a hook was
touched. It detects added, removed, renamed, or edited `.lua` files and
admission-relevant metadata changes. An invalid replacement reports its load
error and leaves the prior working hook set active.

	RASH_HOOK_DIR="$PWD/hooks" RASH_ALLOW_UNOWNED_HOOKS=1 RASH_HOOK_RELOAD=mtime rash

For an imperative reload with no per-command metadata checks, start Rash with
`RASH_HOOK_RELOAD_BUILTIN=1` and run `reloadhooks`. The builtin is absent when
that startup flag is not set.

	RASH_HOOK_DIR="$PWD/hooks" RASH_ALLOW_UNOWNED_HOOKS=1 RASH_HOOK_RELOAD_BUILTIN=1 rash
	$ reloadhooks

Rash is a fork of Bash 5.3. It is a POSIX shell with interactive
command-line editing, job control on architectures that support it,
csh-like history substitution and brace expansion, and the Rash
lifecycle-hook layer described above.

For shell language details see `doc/bashref.info' and the Unix-style
man page. If the info file and the man page conflict, the man page is
definitive.

See POSIX for how defaults differ from the POSIX spec and for posix
mode. COMPAT lists user-visible incompatibilities with bash-5.0 through
bash-5.2. NEWS lists features new in this release.

Rash is free software, distributed under the terms of the GNU General
Public License as published by the Free Software Foundation, version 3
of the License (or any later version). See COPYING. 

A number of frequently-asked questions are answered in the file
`doc/FAQ'. (That file is no longer updated.)

To compile Bash, type `./configure', then `make'.  Bash auto-configures
the build process, so no further intervention should be necessary. Bash
builds with `gcc' by default if it is available.  If you want to use `cc'
instead, type

	CC=cc ./configure

if you are using a Bourne-style shell.  If you are not, the following
may work:

	env CC=cc ./configure

Read the file INSTALL in this directory for more information about how
to customize and control the build process, including how to build in a
directory different from the source directory. The file NOTES contains
platform-specific installation and configuration information.

If you are a csh user and wish to convert your csh aliases to Bash
aliases, you may wish to use the script `examples/misc/alias-conv.sh'
as a starting point. The script `examples/misc/cshtobash' is a more
ambitious script that attempts to do a more complete job.

Reporting problems
==================

This is Rash. File issues at:

	https://github.com/pmarreck/rash/issues

The installed `bashbug` helper does not send mail; it only prints that
issue URL.

Other Packages
==============

This distribution includes, in examples/bash-completion, a recent version
of the `bash-completion' package, which provides programmable completions
for a number of commands. It's available as a package in many distributions,
and that is the first place from which to obtain it.

The latest version of bash-completion is always available from
https://github.com/scop/bash-completion.

If it's not a package from your vendor, you may install the included version.

There are a number of example dynamically loadable builtin commands in the
examples/loadables subdirectory. These are built and installed when bash is
installed. If you want to test or experiment with these builtins before
installing bash, you can run `make loadables' to build them.

Copying and distribution of this file, with or without modification,
are permitted in any medium without royalty provided the copyright
notice and this notice are preserved.  This file is offered as-is,
without any warranty.
