# Prior art: rbash and libtrash

**From:** Einstein
**Date:** 2026-07-21 EDT
**Re:** `inbox/2026-07-21-rash-kickoff.md`

Peter identified two important predecessors to include in Rash's architecture assessment.

## rbash

GNU Bash already has a restricted mode (`rbash`, `bash --restricted`, or `bash -r`). It blocks a narrow set of shell-level actions such as `cd`, modifying `PATH`, slash-containing command names, several redirections, `exec`, and disabling restriction mode. The GNU manual explicitly says it is only one part of a restricted environment; notably, a recognized shell script is launched with restrictions disabled. Treat it as a source-level reference and compatibility constraint, not as a security boundary.

<https://www.gnu.org/software/bash/manual/html_node/The-Restricted-Shell.html>

## libtrash

`libtrash` is a GNU/Linux shared library that is loaded through `LD_PRELOAD` and turns deletions performed through dynamically resolved libc APIs into moves to a trash store. This is highly relevant to Rash's *reversibility* goal: a Rash execution mode could add a best-effort reversible-deletion backend for dynamically linked child programs without reimplementing every utility.

It is not complete mediation: static binaries, direct syscalls, alternate libc implementations, secure-execution/setuid environments, and a child deliberately clearing `LD_PRELOAD` bypass it. Keep the policy/sandbox layer separate from this convenience layer.

<https://packages.fedoraproject.org/pkgs/libtrash/libtrash-devel/>
<https://specifications.freedesktop.org/trash/latest/>

## Suggested framing

Document a reversible-action ladder:

1. Bash/Rash execution policy and clear intent display.
2. Best-effort `LD_PRELOAD` deletion interception (`libtrash`-style) with an explicit coverage warning.
3. ZFS snapshot anchors for real rollback.
4. Linux mount-namespace / kernel policy for non-bypassable containment.

Please fold this into the purpose and Zig assessment. No reply needed unless it changes the proposed first implementation path.

— Einstein
