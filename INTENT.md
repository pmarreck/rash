# Rash intent

Canonical purpose. Implementation status lives in `PLAN.md` and `HOOKS_DESIGN.md`. Do not rewrite this file to match a shortcoming in the code.

**Sources:** Peter (owner), via `HOOKS_DESIGN.md`, `THREAT_MODEL.md`, `DIVERGENCE.md`, `README.md`, and the 2026-08-05 handoff purpose statement. Compatibility bar and `cmd` naming: Peter, 2026-09-10 (`intents/luajit-rack-executor.md`).

## Purpose and users

Rash is a fork of Bash 5.3 whose reason to exist is that an LLM driving a shell produces one characteristic failure: plausible-looking shell that does something subtly different from what was intended. The motivating incident was `cmd | sudo -S tee f < ~/pass` — it reads correctly at a glance and silently replaces the pipe, leaking a secret into a transcript.

Regex and allowlists over command *text* cannot express “this is the right-hand side of a pipe and carries an input redirect.” A gate over the parse tree the executor is about to run can. Put the safety boundary **inside the shell, at the tree**, so analyzer-versus-executor divergence becomes inexpressible.

**Users:** operators who give a shell to an LLM agent (Peter first); anyone who wants Bash with composable deny/log/approve/undo policy without a second parser.

The language stays Bash. Policy is LuaJIT hooks, not a new dialect.

## Desired outcomes

- An operator can give an agent Rash without holding their breath: destructive or exfiltrating *mistakes* are caught at the command tree by composable, preferably root-owned, hooks.
- Adding a new guard is a `.lua` file, not a C patch.
- Damage that still gets through is reversible where we have chosen a catalog (structured undo today; OS-level COW if later accepted).
- Invoked as `bash` / `sh` / `rbash`, Rash keeps Bash identity and GNU `--help`/`--version` wording so existing scripts keep working. Invoked as `rash`, it names itself.
- Every observable divergence from Bash 5.3 is recorded in `DIVERGENCE.md` with a test. “An entry with no test is not a divergence, it is a rumor.”

## Scope and non-goals

**In scope**

- Bash 5.3 language and interactive/job-control surface.
- LuaJIT lifecycle hooks over the executor’s own tree (`HOOK_SEAMS.md`): parse-stage `rash.hook`, expanded `before`/`after`, pipeline, builtin/function/exec, redirects, download-pipe approval.
- Packaged examples of the failure modes we actually see: `curl|bash`, clobber of precious files, `mv`/`cp`/`install` onto a non-directory symlink.
- Hermetic GCC and Zig builds, the upstream test suite plus Rash drivers, and a logged microbenchmark gate.

**Non-goals (must not be sold as solved by hooks alone)**

- A **security boundary** against a determined local user who has another runtime. In-process hooks are a guard against mistakes. The adversary owns the process. Escalating from guard to boundary needs OS enclosure (namespace, hook directory not writable by the agent). (`HOOKS_DESIGN.md`, `THREAT_MODEL.md`)
- Universal undo of arbitrary programs (`dd`, installers, language runtimes, cloud CLIs).
- Stopping Write tools that never enter Rash (`rename` inside another process).
- A new shell language, or silently changing Bash semantics for convenience.
- Filing Rash bugs with Bash mailing lists or the historical Bash maintainers.

## Constraints and tradeoffs

- **GPLv3+** (or later), Bash provenance. Copyright and license headers stay.
- **Compatibility is a constraint, not an aspiration.** 100% Bash behavior unless physically impossible or an explicit, tested `DIVERGENCE.md` entry. POSIX where it already bound Bash.
- **Physics over policy:** no env var that forges a bypass of enforcing hooks. `rash.spawn` is a C port that does not re-enter hooks.
- Hook Lua is a **sandbox**: `base` + `string` only; no `io` / `os` / `package` / FFI. Side effects go through C ports.
- Default parse-stage hooks see **unexpanded** words (`$SECRET` stays `$SECRET`). Expanded stages can see secrets; prefer structure-only policy at parse stage.
- Primary branch of this fork stays `master` (upstream convention), not `yolo`.

## How success is verified

- `./test` (GCC Nix check = upstream `tests/run-all` plus Rash drivers) and `./test --zig`.
- Divergence entries name their tests (`tests/identity.*`, `tests/prettyprint.*`, hook suites, `tests/symlink-replace.*`, …).
- `./bm --micro` (from `./test`) and `./bm`: last-three average, two-sided 15% window, this machine.
- Claims in `THREAT_MODEL.md` must match what hooks actually observe. Do not weaken accepted outcomes to match a missing sensor.

## Open questions

- OverlayFS / COW workspace launcher: Peter still thinking (`PLAN.md` Safety D). Not accepted.
- LuaJIT Rack-style executor (`intents/luajit-rack-executor.md`): proposed 2026-09-10, **not accepted** until explicitly prioritized. C stage extraction is prep, not the Rack rewrite.
- Embedded LuaJIT as a general script host (`rash foo.lua`, shebang, `package.path`): possible, **not decided**. Must not be the hook VM if built.

## Links

| Document | Role |
|---|---|
| `TERMINOLOGY.md` | Rash-specific terms |
| `PLAN.md` | Current work |
| `HOOKS_DESIGN.md` / `HOOK_SEAMS.md` | Hook architecture and seam contracts |
| `THREAT_MODEL.md` | What we claim vs do not |
| `DIVERGENCE.md` | Observable Bash differences |
| `intents/luajit-rack-executor.md` | Proposed direction only |
