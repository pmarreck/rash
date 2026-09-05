Rash
====

[![Mechatron Prime CI](https://img.shields.io/endpoint?url=https%3A%2F%2Fthelio-nixos.tail66c90.ts.net%2Fbadges%2Frash.json&style=for-the-badge)](https://thelio-nixos.tail66c90.ts.net/mechatron-prime/)

**Rash** — **R**eversible **A**uditable/**A**gent-safe **S**hell, a fork of
Bash — is an opinionated, experimental fork of GNU Bash 5.3. It preserves
Bash's shell semantics while developing hermetic validation, compiler
diversity, and a scoped reversible-mutation safety model.

Rash development uses hermetic Nix commands: `./build` and `./test` use the
GCC baseline; `./build --zig` and `./test --zig` compile the same C sources
with pinned Zig 0.16; `./bm` compares their release-mode execution times and
records wall-clock, user-CPU, and system-CPU history.

## Lifecycle hooks (advisory + enforcing)

Rash embeds LuaJIT for post-parse command lifecycle hooks. Root-owned hooks
under `$prefix/share/rash/hooks` are **enforcing**: they may call `rash.deny`
and fail closed on errors. Packaged policies include `deny_sudo_tee.lua`
(blocks `… | sudo -S tee` with an input redirect) and
`deny_sensitive_clobber.lua` (blocks `>` onto lexical path words matching
`.ssh/`, `id_rsa`, and similar). `warn_sudo_tee.lua` remains as an advisory
example.

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

GNU Bash is the GNU Project's Bourne
Again SHell, a complete implementation of the POSIX shell spec,
but also with interactive command line editing, job control on
architectures that support it, csh-like features such as history
substitution and brace expansion, and a slew of other features. 
For more information on the features of Bash that are new to this
type of shell, see the file `doc/bashref.info'.  There is also a
large Unix-style man page. If the info fie and the man page conflict,
the man page is the definitive description of the shell's features. 

See the file POSIX for a discussion of how the Bash defaults differ
from the POSIX spec and a description of the Bash `posix mode'.

There are some user-visible incompatibilities between this version
of Bash and previous widely-distributed versions, bash-5.0, bash-5.1,
and bash-5.2. The COMPAT file has the details. The NEWS file tersely
lists features that are new in this release. 

Bash is free software, distributed under the terms of the [GNU] General
Public License as published by the Free Software Foundation,
version 3 of the License (or any later version).  For more information,
see the file COPYING. 

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

Reporting Bugs
==============

Bug reports for bash should be sent to:

	bug-bash@gnu.org

using the `bashbug' program that is built and installed at the same
time as bash.

The discussion list `bug-bash@gnu.org' often contains information
about new ports of Bash, or discussions of new features or behavior
changes that people would like.  This mailing list is also available
as a usenet newsgroup: gnu.bash.bug.

The `help-bash@gnu.org' mailing list is used for questions about
using bash.

When you send a bug report, please use the `bashbug' program that is
built at the same time as bash. If bash fails to build, try building
bashbug directly with `make bashbug'. If you cannot build `bashbug',
please send mail to bug-bash@gnu.org with the following information:

	* the version number and release status of Bash (e.g., 2.05a-release)
	* the machine and OS that it is running on (you may run
	  `bashversion -l' from the bash build directory for this information)
	* a list of the compilation flags or the contents of `config.h', if
	  appropriate
	* a description of the bug
	* a recipe for recreating the bug reliably
	* a fix for the bug if you have one!

The `bashbug' program includes much of this automatically.

Questions and requests for help with bash and bash programming may be
sent to the help-bash@gnu.org mailing list.

If you would like to contact the Bash maintainers directly, send mail
to bash-maintainers@gnu.org.

While the Bash maintainers do not promise to fix all bugs, we would
like this shell to be the best that we can make it.

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

Enjoy!

Chet Ramey
chet.ramey@case.edu

Copying and distribution of this file, with or without modification,
are permitted in any medium without royalty provided the copyright
notice and this notice are preserved.  This file is offered as-is,
without any warranty.
