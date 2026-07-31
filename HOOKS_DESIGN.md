# Command lifecycle hooks: design decisions

Not a security boundary against a determined local user, and it must never be
described as one. It is a structural guard against *mistakes* — which is the
dominant failure mode when an LLM drives a shell, and the reason this is worth
building.

## Shape

One primitive, wrapping execution, in the style of Rack middleware:

```lua
rash.hook(function(cmd, run)
  -- before: inspect the tree, optionally rash.deny(reason)
  local result = run(cmd)
  -- after: exit status, duration -- where the undo log lives
  return result
end)
```

Pre-exec, veto, post-exec, and composition all fall out of one function.
Ordering is explicit, and stacking is function composition. This is the
property bash's single `DEBUG` trap cannot provide: multiple independent
registrations that compose, as in PAM's stacked modules.

## Re-entrancy without a forgeable bypass

A hook will want to run commands (stat a file, call a linter). If those go back
through the hook pipeline, it recurses or hangs.

The obvious fix — an environment marker like `RASH_HOOK_BYPASS=1` that
suppresses hooks — **must not be used**. Anything inheritable is anything
forgeable: a user, a script, or a confused agent can set it and walk straight
past every hook. It would also be the first thing discovered and cargo-culted.

Instead, remove the need for a marker (physics over policy):

- `rash.spawn()` is a **separate code path in C** that forks and execs
  directly, without entering the hook pipeline. Bypass is a property of *which
  function you called*, not of a value anyone can set. There is nothing to
  forge because there is no token.
- An in-process depth counter catches accidental recursion within a single
  shell and fails loudly rather than hanging.
- A hook that deliberately runs `rash -c ...` as a subprocess gets a fresh
  shell that runs its own hooks. That is correct and desired, not recursion.

## Marshalling: on demand only

Deep-copying every `COMMAND *` into a Lua table on every command is real
latency in an interactive shell. Nodes are exposed as LuaJIT FFI userdata with
an `__index` metamethod that reads fields on access. A hook that inspects
nothing costs nearly nothing, and a hook that reads one field pays for one
field. This is the case LuaJIT's FFI is best at, and the main reason to prefer
it over PUC Lua.

## Mutation

Permitted, but never silent -- and reported without ever diffing the tree.

A whole-tree diff against a pristine copy would cost O(tree) on every command
and require retaining that copy. It is also unnecessary. Mutation can only
reach the tree through the proxy's `__newindex`, so each write is journaled at
the moment it happens as (node, field, old, new). The report is that journal.
Cost is O(mutations) -- zero for the overwhelmingly common case of a hook that
changes nothing -- and no original copy is kept.

Under validate-only, the default, `__newindex` simply raises an error and the
machinery costs nothing at all.

What you typed not being what runs is a debugging nightmare and an injection
surface, so any mutation that does occur is always echoed to stderr.

## Requiring privilege to change hooks

Hooks load from a root-owned directory, with the ownership and permission
discipline `sudo` and `ssh` already use: refuse to load anything not owned by
root or writable by group or world, and say so loudly rather than silently
skipping.

- Root-owned hooks are **enforcing**.
- User-owned hooks are **advisory**, and are labeled as such.

What this buys, stated honestly: an unprivileged user or agent can no longer
edit the rules, so the guard stops being trivially removable. What it does not
buy: that same user can still invoke a different shell, or `/bin/bash`
directly. The escalation from "guard" to "boundary" requires the OS-level work
already described in `MUTATION_SAFETY_OPTIONS.md` — a root-created mount
namespace where rash is the only shell present and the hook directory is not
writable. Hooks and that envelope are complementary; neither is sufficient
alone.

## Sensitive data

Hooks observe every command, so they are a potential leak channel. The incident
that motivated this whole line of work was itself a secret leaking into a
transcript, and it would be an unusually poor outcome to fix that by building a
better leak.

The stage distinction does most of the work:

- **Post-parse hooks see words unexpanded.** `$SECRET` is the four characters
  `$SEC…`, not its value. A hook that logs every command at this stage cannot
  log a secret held in a variable. This is a real privacy property of the
  default stage, not a mitigation bolted on.
- **Post-expansion hooks see real values** — resolved paths, and any secret
  passed as an argument. This stage is opt-in, must be labeled sensitive at
  registration, and should be rare.

Remaining measures:

- Logging goes through a rash-provided API that redacts anything originating
  from a variable expansion, rather than letting hooks write raw argv wherever
  they like.
- The audit log is root-owned and mode 0600.
- Secrets on **stdin** — the actual mechanism of the original incident — are
  never visible to any hook stage, since hooks see commands, not data. That is
  worth stating plainly: this system would not have seen the password, only the
  malformed redirect that leaked it. Which was sufficient.

## Why this matters for agents

An LLM driving a shell produces exactly one characteristic failure: plausible
shell that does something subtly different from what was intended. The
motivating case, `cmd | sudo -S tee f < ~/pass`, reads correctly at a glance
and silently replaces the pipe.

Current agent harnesses gate this with regex or command allowlists over command
*text*, which cannot express "this command is the right-hand side of a pipe and
carries an input redirect." A gate over the tree the executor is about to run
can express it exactly, with no reparse to disagree with the shell.

Against a mistaken agent — the realistic case — this is strong. Against a
deliberately adversarial one it is not, and must not be sold as such.
