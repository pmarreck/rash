# Rash deep code review

Reviewed at commit `c56721698632` (GNU Bash 5.3 patch 15 baseline) on 2026-07-23.
Scope: the shell, tests, build surface, resource/error paths, dynamic builtins,
and all 13 requested review dimensions. Generated documentation and vendored
compatibility code were not treated as Rash-authored defects.

Summary at the reviewed baseline: **1 critical**, **7 warnings**, and **1
advisory**. The aggregate-runner critical finding and the coprocess
pipe-exhaustion warning are resolved in the current review units; the
remaining source-level findings are tracked below. There are no databases in
this project, and no other verified FFI boundary apart from dynamically loaded
builtins.

## 1. Incomplete or inconsistent functionality

No independently verified shell-language, parser, executor, or builtin
semantic defect was found in this dimension. The false-green test aggregation
problem is recorded under dimensions 2 and 3 rather than misclassified as a
shell semantic change.

## 2. Inadequate test coverage

### RESOLVED CRITICAL: the complete test suite could report success after a failed driver

`tests/run-all:61-70` invokes every `run-*` driver but discards each exit
status and unconditionally exits zero. `Makefile.in:1079-1083` makes that
script the complete test command, and `flake.nix:77-80` selects that target
for the hermetic check. An isolated fixture containing a `run-failing` driver
that exits 23 proved that `run-all` still exits zero.

Resolved with a self-contained regression harness that proves a failing child
returns nonzero, reports its captured diagnostics, and does not prevent a
later child from running. `run-all` now accumulates failures and returns a
nonzero status after all drivers have run.

## 3. Futile tests

### RESOLVED CRITICAL: diff assertions could not fail CI

This was the same root cause as dimension 2. Individual assertions, such as
`tests/run-dynvar:1-2` and `tests/run-test:3-4`, return nonzero on a diff, and
the repaired `tests/run-all` now propagates that result to CI without stopping
subsequent drivers.

## 4. Test speed and determinism

### WARNING: EPOCHREALTIME test does not validate realtime

`tests/dynvar.tests:80-101` calculates `dsec` but never uses it; the only
assertion (`dmsec < 1000000`) accepts `0.0` followed by `0.0`. Its negative
branch also references unset `desc` at line 97. Assert an independently
sampled epoch-second value and a syntactically valid microsecond field, with a
small explicit boundary allowance and no sleep.

### WARNING: test builtin fixtures are globally shared and use sleeps

`tests/test.tests:72-78,94-99,126-131,153-188,254-296,409-427` mutates
predictable `/tmp/test.*` and `/tmp/abc|def|ghi` paths without private
namespacing or cleanup traps. `tests/test.tests:185,254-256` additionally
sleeps solely for timestamp ordering. Parallel or interrupted runs can race or
leave modified files. Use one `mktemp -d` directory under `TMPDIR`, trap its
cleanup, and set explicit ordered timestamps with `touch -a/-m -t`.

## 5. Superfluous or duplicated functionality

### WARNING: latent array helper branch has divergent indexing

`array.c:828-840` contains nonempty-array paths that the sole current caller
cannot reach: `builtins/complete.def:764-767` uses
`builtin_find_indexed_array`, which flushes the array at
`builtins/common.c:1011-1012`. In the dead default-backend loop,
`array.c:835` repeatedly uses `vec[0]`; the alternate implementation correctly
uses `vec[i]` at `array2.c:846`. Add a direct helper test over a prepopulated
two-element array, then correct the index. Deciding whether to remove the
redundant flush or simplify the helper is a separate compatibility refactor.

### WARNING: the Nix derivation constructor is duplicated

`flake.nix:19-50` and `flake.nix:61-92` maintain two equivalent `mkBash`
definitions. A later dependency or sandbox change can diverge between builds
and checks. Safely factor a shared per-system constructor only after choosing a
refactor scope and validating with `nix flake check` and `./test`.

## 6. Organization and clarity

No additional substantive issue found. The large parser, executor, and
variable modules reflect compatibility scope; splitting them without a tested
seam would currently increase risk. The `array.c`/`array2.c` split is an
intentional configure-time time/space trade-off.

## 7. Algorithmic complexity

### ADVISORY: associative-array joining performs avoidable temporary copies

`assoc.c:537,549-568`, used by `subst.c:7783-7786`, duplicates each value into
a temporary reversed `WORD_LIST` before the final joining pass. The work is
still linear, but large associative-array expansions incur an extra node and
string copy per value. Benchmark a direct two-pass iterator before changing it;
shell quoting and null-element behavior make this an optimization, not a quick
cleanup.

## 8. Files with unclear purpose

### WARNING: README leads contributors to a non-hermetic build path

`README:32-46` presents raw `./configure && make` as the build path, while the
committed project interface is `./build` and `./test` through the pinned Nix
flake. Add a short Rash-specific contributor section that makes the wrappers
the default, retaining the Autoconf commands only as upstream-maintainer
reference.

## 9. Language-feature use

No reportable issue found. The C and Autoconf feature set deliberately supports
old platforms and already centralizes checked allocation and portability
probes. Replacing it with newer C-only features would reduce compatibility
without demonstrated benefit.

## 10. Memory safety and resource handling

### RESOLVED WARNING: failed coprocess pipe creation used uninitialized descriptors

`execute_cmd.c:2520-2521` ignores both fallible `sh_openpipe` calls.
`general.c:750-760` returns failure without initializing the output pair; the
subsequent code at `execute_cmd.c:2527-2566` closes, duplicates, and exports
those arbitrary values. The exact Nix-built shell reproduces this with
`ulimit -n 4; coproc c { :; }`: it returns zero, exports implausible descriptors,
and the child reports failed `dup2` operations.

Resolved by initializing pipe pairs, checking both `sh_openpipe` calls, closing
the first pair if the second fails, and returning the normal pipe failure before
forking or exporting coprocess variables. A low-`RLIMIT_NOFILE` regression in
`tests/coproc.tests` asserts nonzero status and a `coproc: pipe error` diagnostic.

## 11. FFI boundary correctness

### WARNING: replacing a dynamic builtin leaks the old loader handle

When replacing a dynamically loaded builtin, `builtins/enable.def:451-462`
overwrites the old descriptor and loses its `dlopen` handle. A later unload at
`builtins/enable.def:557-596` can only clean up the new module. Two temporary
ABI-compatible modules demonstrated that replacement followed by one unload
leaves the first module mapped and invokes only one unload hook.

This requires a separate lifecycle design: either reject replacement or retain
the old handle, run its unload hook, and balance `dlclose` only when no
registration still uses it. Cover replacement and same-object reference counts
with a focused loadable-builtin integration test.

## 12. Error handling

### RESOLVED WARNING: coprocess pipe exhaustion was incorrectly reported as success

This was the visible error-handling aspect of the dimension-10 defect:
`EMFILE` reached neither a pipe diagnostic nor a failure status before
`make_child`. The shared regression now requires a controlled diagnostic and
nonzero status.

## 13. Database access

Not applicable. No SQL, ORM, ODBC, SQLite, PostgreSQL, MySQL, or similar
database-client path exists in the repository.

## Recommended order

1. ~~Repair the aggregate test runner with a red–green regression harness.~~
2. ~~Repair the coprocess pipe-failure path with a contained regression test.~~
3. Address test determinism, documentation, and dynamic-builtin lifecycle in
   separately reviewed units.
