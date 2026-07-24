# Reversible Mutation Safety: Evidence and Options

Rash can make filesystem mutation safer, but a Bash-only hook cannot honestly
promise to undo arbitrary disk changes. A shell hook sees syntax and selected
builtins; external programs can later write through syscalls, mmap, inherited
file descriptors, rename/unlink operations, or remote APIs. Reliable rollback
therefore needs a copy-on-write or snapshot execution envelope below Bash.

## Bash source seams

| Location | Value | Limitation |
| --- | --- | --- |
| `parse.y` redirection construction (around lines 532–802) | Can inspect a whole command AST before execution. | Targets remain unexpanded; command substitutions during expansion may already mutate state. Treat as planning/audit only. |
| `redir.c:do_redirection_internal()` (between target expansion and `redir_open()`, around lines 915–926) | Best central execution hook for resolved `>`, `>>`, `>|`, `<>`, `&>`, `&>>`, `exec >`, null-command, function, builtin, and external-command redirections. It has the final lexical pathname, redirection type, open flags, mode, and destination fd before a destructive open. | It sees an open intent, not later writes by an external command; a pathname backup here has TOCTOU races without a filesystem transaction. |
| `builtins/history.def` and `bashhist.c` history writers | Required second seam for `history -a/-w` and shutdown history writes, which bypass `redir.c` and can truncate, mmap, rename, and change metadata. | Does not cover arbitrary shell commands. |
| `trap DEBUG` in `execute_cmd.c` | Useful unprivileged UX prototype: it logs printable simple commands before dispatch. | Not a policy boundary: mutable by shell code, lacks final paths, and cannot observe exec'd descendants. |

Existing `RX_UNDOABLE` redirection support restores descriptor topology only;
it does not recover file contents, names, or metadata. Bash's restricted mode
also proves `redir.c` is a policy seam, but it is a coarse redirection denial
feature rather than an undo mechanism.

## Coverage boundary

No Bash-level hook can fully cover writes performed after `shell_execve`,
including direct syscall and library writes, mmap writeback, mutations through
inherited fds, unlink/rename/link operations, metadata/xattr changes, or
effects outside a selected filesystem subtree. Here-documents and process
substitution may create temporary pipes, files, or FIFOs; they are internal
resources and must not be confused with user-target writes.

## Recommended architecture

For Linux, start with a narrowly root-owned launcher that creates a private
mount namespace and an OverlayFS session for an explicitly selected workspace.
The existing Rash binary and all descendants then run unprivileged inside that
copy-on-write view. Discarding the session upperdir rolls back scoped changes;
preserving it yields an inspectable delta. Pairing the session with Landlock
can deny writes outside the declared mount, but Landlock is deny-only and does
not retain preimages.

This is filesystem-agnostic at Rash's interface, not literally universal:
OverlayFS has upperdir requirements and only protects mounted paths. macOS
would need an Endpoint Security plus APFS-snapshot design; Windows needs a
filesystem minifilter or equivalent privileged layer. Seccomp user notification,
eBPF LSM, and fanotify are later audit/policy tools, not a first rollback
engine.

## Ranked next experiments

1. With sudo, create an OverlayFS mount-namespace fixture and run redirection,
   `history -w`, an external editor, `rm`, `rename`, and an mmap-writing helper.
   Assert the lower-tree digest is unchanged and deleting the session upperdir
   restores the original view. Include an intentional out-of-scope write to
   prove the boundary.
2. After the COW envelope works, add compile-time/root-configured audit-only
   events at the `redir.c` seam and before the first history-file mutation.
   Send structured intent to a root-owned Unix-socket daemon; do not make Bash
   setuid or trust a mutable environment variable for policy.
3. Add an optional Landlock boundary for descendants, with a kernel ABI probe
   and explicit tests for inherited writable descriptors and out-of-scope paths.
4. Consider syscall interception or platform-native filters only if the product
   requires arbitrary host paths beyond a bounded workspace.

The complete read-only source investigation is retained at
`/tmp/dispatch-log/mutation-hooks-final.md` for this development session.
