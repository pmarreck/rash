# Rash Plan

- [x] Mirror the canonical GNU Bash repository to `pmarreck/rash`, retaining Savannah as the fetch-only `upstream` remote. (2026-07-21 11:15 EDT)
- [x] Establish and record a hermetic upstream build baseline before any broad rename. Curiosity poke: preserve the upstream `master` branch convention and GPL provenance. (2026-07-22 21:32 EDT; pinned Nix flake and `./build` pass. The review later proved the original test runner false-green, so complete-suite portability is tracked separately.)
- [ ] Define Rash's reversible/agent-safe purpose and document the initial safety architecture without changing shell semantics accidentally.
- [ ] Rename the live project surface from Bash to Rash carefully, with a tested reversible mapping and an explicit decision about historical/GPL attribution.
- [ ] Assess a compatible Zig 0.16 rewrite: subsystem boundaries, compatibility risks, sequencing, and a realistic effort range.
- [x] Complete the 13-dimension deep review in `CODE_REVIEW.md`, repair the aggregate test runner and coprocess pipe-exhaustion paths with regressions, and make every upstream driver pass in a hermetic Nix environment. (2026-07-24 03:06 EDT) Curiosity poke: a green check must never suppress a driver failure or borrow undeclared host state.
- [x] Rename the top-level README to `README.md` so GitHub renders the canonical Mechatron Prime CI badge, and update the install manifest to ship the renamed file. (2026-07-24 13:07 EDT; hermetic package and full test targets pass.)
- [ ] Add a pinned Zig 0.16 C-compiler variant and prove its hermetic package and complete upstream test targets. Curiosity poke: distinguish compiler diagnostics from Bash compatibility regressions.
- [ ] Establish a repeatable GCC-versus-Zig release benchmark with wall-clock and CPU-time history. Curiosity poke: avoid mistaking process startup, debug code, or host scheduling noise for evaluator performance.
- [ ] Prototype a sudo-created Linux COW mutation envelope before adding Bash-level policy hooks. Curiosity poke: Bash can gate resolved redirections, but external descendants and non-redirection writes require an OS-level rollback boundary.
- [ ] Eliminate remaining test timing/global-fixture risks and assess the dynamic-builtin lifecycle leak in separate red–green units.
