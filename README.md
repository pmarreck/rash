Rash
====

[![Mechatron Prime CI](https://img.shields.io/endpoint?url=https%3A%2F%2Fthelio-nixos.tail66c90.ts.net%2Fbadges%2Frash.json&style=for-the-badge)](https://thelio-nixos.tail66c90.ts.net/mechatron-prime/)

Rash is an opinionated, experimental fork of GNU Bash 5.3. It preserves Bash's
shell semantics while developing hermetic validation, compiler diversity, and a
scoped reversible-mutation safety model.

Rash development uses hermetic Nix commands: `./build` and `./test` use the
GCC baseline; `./build --zig` and `./test --zig` compile the same C sources
with pinned Zig 0.16; `./bm` compares their release-mode execution times and
records wall-clock, user-CPU, and system-CPU history.

## Advisory lifecycle hooks

Rash embeds LuaJIT for post-parse command lifecycle hooks. The first shipped
hook, `warn_sudo_tee.lua`, recognizes the exact right-hand pipeline stage
`sudo -S tee` with an input redirection. It warns that the redirect replaces
the pipe and can expose a secret sent to standard input. It always executes
the parsed command unchanged.

Installed hooks live in `$prefix/share/rash/hooks`; enable them explicitly:

	RASH_HOOK_DIR=/usr/local/share/rash/hooks rash -c '...'

Each hook must be a regular file owned by root and not writable by group or
world. To try a user-owned hook during development, set both variables:

	RASH_HOOK_DIR="$PWD/hooks" RASH_ALLOW_UNOWNED_HOOKS=1 rash -c '...'

Rash prints `WARNING: RASH_ALLOW_UNOWNED_HOOKS=1` for every such mutable hook.
This opt-in loads lower-trust advisory code; it never bypasses another hook.
All hooks in this first release are advisory: errors, a missing `run()`, or an
instruction-limit failure still continue to the original command. Lua's
general-purpose standard libraries are not exposed to hook files.

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
