# Rash kickoff — from Einstein

Peter asked for `pmarreck/rash`: the reversible/revertible agent shell. It is a GitHub mirror of canonical GNU Bash, with the original Savannah repository retained as fetch-only `upstream`; preserve the upstream `master` convention and GPL provenance.

Please own this first exploration pass:

1. Orient with `dirtree`, `codescan status`, and the existing source documentation. Add/update its notes as you learn the source tree.
2. Establish a clean upstream build/test baseline before modifying source. Update `PLAN.md`. Do not perform a broad replacement before a passing baseline and a clear rollback point.
3. Add Rash's purpose of existence: agent-safe, reversible operations, with Bash compatibility as an explicit constraint. Clarify what is intentionally unchanged at this stage.
4. Carefully replace the *live project surface* from Bash to Rash as Peter requested. Preserve historical Git objects, license/copyright attributions, and upstream URLs; do not falsely rewrite provenance. Make any global transformation reviewable, mechanically checked, and reversible.
5. Write an evidence-based Zig 0.16 rewrite assessment: subsystem map, what can be reused conceptually, compatibility/test burden, practical phases, and effort/risk range. This is an assessment only, not a rewrite.
6. Run the `deep-code-review` skill faithfully. Its deliverable is `CODE_REVIEW.md`; use its 13 dimensions, favor substantive findings, and respect the four-agent concurrency limit by batching if needed. Fix at least one well-verified, appropriately scoped finding via strict TDD.

Commit only green units and push them to `origin/master`. Keep this inbox note as the durable kickoff record; report progress here when a milestone is complete.
