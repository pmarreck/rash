# Rash Plan

- [x] Mirror the canonical GNU Bash repository to `pmarreck/rash`, retaining Savannah as the fetch-only `upstream` remote. (2026-07-21 11:15 EDT)
- [x] Establish and record a clean, reproducible upstream build/test baseline before any broad rename. Curiosity poke: preserve the upstream `master` branch convention and GPL provenance. (2026-07-22 21:32 EDT; pinned Nix flake, `./build`, and complete `./test` suite pass.)
- [ ] Define Rash's reversible/agent-safe purpose and document the initial safety architecture without changing shell semantics accidentally.
- [ ] Rename the live project surface from Bash to Rash carefully, with a tested reversible mapping and an explicit decision about historical/GPL attribution.
- [ ] Assess a compatible Zig 0.16 rewrite: subsystem boundaries, compatibility risks, sequencing, and a realistic effort range.
- [ ] Complete the 13-dimension deep review in `CODE_REVIEW.md`, fix a small verified finding with a regression test, and commit/push each green unit.
