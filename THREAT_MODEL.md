# Threat model: command lifecycle hooks and related controls

**Status:** design documentation (hooks not yet implemented)  
**Audience:** anyone writing hooks, packaging rash for agents, or describing
what rash claims to protect  
**Related:** `HOOKS_DESIGN.md` (hook shape and non-boundary stance),
`MUTATION_SAFETY_OPTIONS.md` (COW / OS rollback envelope)

This document catalogs threats to the *intended* design: LuaJIT lifecycle
hooks over the executor’s own parse tree, optional structured undo for a
catalog of operations, optional content-hash exec allowlisting, and optional
OS-level enclosure (including Nix-provided environments). It is deliberately
pessimistic. The product remains viable when claims match the threat model.

---

## 1. What we claim, and what we do not

### In scope (intended value)

| Claim | Honest strength |
|---|---|
| Policy and audit over the **same AST** the shell will execute | Strong against harness-vs-shell parser drift |
| Reduction of **accidental** damage from agents and humans | Strong; this is the primary product |
| Default hook stage sees **unexpanded** words (`$SECRET` not the secret) | Real privacy property of the default stage |
| Root-owned **enforcing** hooks that unprivileged code cannot trivially edit | Meaningful speed bump for agents and scripts inside rash |
| Catalog undo via **structured preimages** (not inverse shell text) for shadowed tools | Useful, incomplete by construction |
| Optional **content-hash** allowlist on the exec path | Raises cost of unexpected binaries through rash |
| Optional **workspace COW** / Landlock / restricted PATH | Turns speed bumps into a real session cage when deployed |

### Out of scope (must never be marketed as solved by hooks alone)

- A **security boundary** against a determined local user with another runtime
- Complete observation of **post-`exec`** syscalls and library writes
- **Universal undo** of arbitrary programs (`dd`, installers, language tools, cloud CLIs)
- Confidentiality of secrets once **post-expansion** hooks or raw argv logging are enabled
- Stopping someone who can open a non-rash shell, unless the *environment* only offers rash

**Product framing:** a structural speed bump that is hard for the careless and
annoying for the motivated; a full cage only when OS enclosure and exec
allowlisting are also present. Not a fortress.

---

## 2. Assets

| Asset | Why it matters |
|---|---|
| User/agent data on disk | Primary damage surface (truncate, delete, chmod, exfil) |
| Secrets in the environment, files, and argv | Leak via logs, post-expansion hooks, history, cloud audit |
| Integrity of hook policy and allowlists | If forged, every other control is theater |
| Audit / undo log integrity | Tampering hides damage or injects hostile “restore” records |
| Trust in the rash binary itself | Supply chain or local replace of the shell |
| Downstream SIEM / “cloud security” consumers | Log injection and secret suction |

---

## 3. Actors

| Actor | Capability | Goal |
|---|---|---|
| **Confused agent** | Uses only the tools the harness gives it; follows bad plans or poisoned prompts | Accidental destruction or leak; not deliberate escape |
| **Malicious script / untrusted repo** | Runs *inside* rash at user privilege | Disable policy, destroy data, exfil |
| **Local user who wants out** | Full account: other binaries, package installs, maybe root via other means | Bypass hooks and allowlists |
| **Hostile or buggy hook** | Code loaded as enforcing or advisory hook | Arbitrary behavior in-process |
| **Remote audit consumer** | Receives structured logs | Side-channel exfil if over-broad; poisoning if log format is weak |
| **Prompt injector** | Controls content the agent reads | Social-engineer the agent into disable/approve/escape |

Primary design target: **confused agent** and **careless human**.  
Secondary: **malicious script inside rash**.  
Tertiary (document only unless enclosure is deployed): **motivated local user**.

---

## 4. Control layers (stack, don’t substitute)

Higher layers do not replace lower ones. Gaps compound.

```
┌─────────────────────────────────────────────────────────────┐
│  L5  Session / workspace enclosure                          │
│      OverlayFS COW, Landlock, mount ns, “only rash in PATH” │
│      Optional: Nix-provided env (advised, not required)     │
├─────────────────────────────────────────────────────────────┤
│  L4  Exec allowlist (content hash, e.g. Blake3)             │
│      Hash of bytes about to execve; root-owned list         │
├─────────────────────────────────────────────────────────────┤
│  L3  Env sanitization at secure exec                         │
│      Clear LD_PRELOAD / LD_LIBRARY_PATH abuse, etc.         │
├─────────────────────────────────────────────────────────────┤
│  L2  Command lifecycle hooks (LuaJIT, this design)          │
│      AST inspect / deny / audit / catalog undo              │
├─────────────────────────────────────────────────────────────┤
│  L1  Single executor grammar                                 │
│      No second parser that can disagree with bash           │
└─────────────────────────────────────────────────────────────┘
```

| Layer | Stops (examples) | Does not stop |
|---|---|---|
| L1 | Harness regex vs real parse drift | Bad policy, post-exec effects |
| L2 | Structured mistakes (wrong redir, pipe+sudo shape) | `/bin/bash`, `python -c`, raw syscalls after exec |
| L3 | Classic library injection via env | Interpreters evaluating hostile code |
| L4 | Unexpected *binaries* on rash’s exec path | Code loaded as *data* by an allowlisted interpreter |
| L5 | Writes outside workspace; easy PATH escape | Kernel bugs; user with another machine |

Nix (or any hermetic env builder) is a **packaging and enclosure aid for L5**,
not a rash runtime dependency. See §9.

---

## 5. Threat catalog

IDs are stable for cross-reference from design and tests. Severity is relative
to the **confused-agent / in-rash** product unless noted.

### 5.1 Bypass and scope (hooks never see the act)

| ID | Threat | Severity | Notes / mitigations |
|---|---|---|---|
| T-BYP-01 | Invoke real `/bin/bash`, `dash`, or another shell | High (if PATH open) | L4 hash deny; L5 PATH with only rash (optionally `bash` → rash symlink); never claim hooks alone stop this |
| T-BYP-02 | General interpreter: `python -c`, `node -e`, `perl -e`, `lua -e` | High | L4: omit interpreters, or pair with frozen argv policy; document “allowlisting python ≈ allowlisting everything” |
| T-BYP-03 | `eval`, `source`, `bash -c` built from data | High | Post-parse cannot see payload; post-expansion can (secret cost). Policy on `eval`/`source` shapes; prefer deny in agent profiles |
| T-BYP-04 | Background jobs, coprocs, pipelines after approve | Medium | Define when hooks fire; async reaps (SIGCHLD) must not drop policy events; test job-control paths |
| T-BYP-05 | User `trap DEBUG` / redefining builtins to shadow policy | Medium–High | Enforcing hooks below user trap surface; do not implement policy *as* DEBUG trap |
| T-BYP-06 | Nested `rash -c` from a hook expecting bypass | Low if designed | Fresh shell re-runs hooks (desired). `rash.spawn()` is the only non-hook path; no env token (`HOOKS_DESIGN.md`) |
| T-BYP-07 | Forgeable bypass env (`RASH_HOOK_BYPASS=1`) | Critical if present | **Forbidden by design.** Bypass = which C function was called, not a value |

### 5.2 Injection into Lua and the host process

| ID | Threat | Severity | Notes / mitigations |
|---|---|---|---|
| T-LUA-01 | Compile user/command bytes as Lua (`load` / `loadstring` on words) | Critical | Cross boundary as opaque strings or FFI userdata only; never as source |
| T-LUA-02 | Hook templates that concatenate argv into Lua or shell | High | Safe logging API; no string-built code paths in platform examples |
| T-LUA-03 | Full LuaJIT: `debug`, `package.loadlib`, raw `ffi.cdef` | Critical for enforcing hooks | Locked env: no debug, no arbitrary FFI; host-provided types only for enforcing mode |
| T-LUA-04 | FFI memory write → disable hooks / steal credentials | Critical | Same as T-LUA-03; or out-of-process hooks with narrow IPC (heavier) |
| T-LUA-05 | Shared Lua globals / metatable pollution across commands | High | Fresh or frozen env per hook file; no mutable shared policy tables without audited API |
| T-LUA-06 | Hook infinite loop / memory bomb freezes agent | Medium | Wall-clock and allocation budgets; explicit fail-closed vs fail-open policy |

### 5.3 Policy logic and TOCTOU

| ID | Threat | Severity | Notes / mitigations |
|---|---|---|---|
| T-POL-01 | Lexical path policy bypass (`../`, symlinks, `~`, weird bytes) | High | Document limits of unexpanded stage; resolve carefully at redir seam; prefer L5 path scope |
| T-POL-02 | Approve then symlink swap before open (TOCTOU) | High | No pure userspace fix; L5 COW bounds damage; openat-style handoff is hard in a shell |
| T-POL-03 | Allowlist argv0 only (`chmod` safe, `chmod -R /` not) | High | Structure-aware policy; recursive catalog (§6) |
| T-POL-04 | Denylist `rm` but not `find -delete` / `rsync --delete` | High | Coverage lists and default-deny agent profiles beat infinite denylists |
| T-POL-05 | Encoding / log injection into SIEM (newlines, quotes in words) | Medium | Structured logs; treat every word as untrusted bytes |
| T-POL-06 | Hook error fails **open** (execute anyway) | High for enforcing | Enforcing mode: fail **closed** on hook errors for covered ops |
| T-POL-07 | Approval fatigue → auto-approve everything | Medium (social) | Rare, loud prompts for destroy-class ops; silent allow/deny elsewhere |

### 5.4 Secrets and audit channels

| ID | Threat | Severity | Notes / mitigations |
|---|---|---|---|
| T-SEC-01 | Post-expansion hooks log real secrets | Critical if enabled casually | Default post-parse; expansion stage opt-in, root-owned, labeled sensitive |
| T-SEC-02 | Cloud / remote audit becomes exfil | High | Local root-owned log default; egress allowlist for any daemon; no post-expansion off-box |
| T-SEC-03 | Stdin secrets never visible to hooks | Informational | By design; do not claim to inspect `curl \| sh` payloads on the wire |
| T-SEC-04 | History file writes bypass command hooks | Medium | Separate seam (`bashhist` / history builtins); see `MUTATION_SAFETY_OPTIONS.md` |
| T-SEC-05 | Agent enables “verbose security logging” via prompt injection | Medium | High-friction for sensitive stage; visible banner |

### 5.5 Integrity of hooks and allowlists

| ID | Threat | Severity | Notes / mitigations |
|---|---|---|---|
| T-INT-01 | User-writable “enforcing” hook directory | Critical | Root-owned dir; refuse group/world-writable; ssh/sudo permission discipline |
| T-INT-02 | `$RASH_HOOK_PATH` (or similar) from environment | High | Fixed compile-time or root config path only for enforcing loads |
| T-INT-03 | TOCTOU replace hook between stat and read | High | Open once, `fstat` fd, verify owner/mode on fd, then read |
| T-INT-04 | Malicious third-party hook package | High | Supply-chain: ship hooks with rash, or sign; document trust root |
| T-INT-05 | Advisory hooks mistaken for enforcing | Medium | Label in UX and logs; never silent upgrade of trust |

### 5.6 Undo-specific

| ID | Threat | Severity | Notes / mitigations |
|---|---|---|---|
| T-UND-01 | Universal undo promise for post-exec effects | Critical (honesty) | Forbidden claim; L5 COW or “not covered” |
| T-UND-02 | Undo log as executable shell text (filename injection) | High | **Structured records only** (path, prior mode, prior bytes ref, …) |
| T-UND-03 | Incomplete inverse (one `chmod -R` line as undo) | High | Expand recursive tools to per-path preimages (§6) |
| T-UND-04 | Walk/log then real tool sees different tree (TOCTOU) | Medium–High | Document; COW reduces impact; no perfect shell-only fix |
| T-UND-05 | Shadow missed: real binary invoked without wrapper | High | L4 hash + single exec path; function shadows alone are insufficient |
| T-UND-06 | Double-apply or hostile restore from tampered log | High | Root-owned log; integrity; restore is privileged operation |
| T-UND-07 | Large/special files skipped for `>` preimage | Medium | Explicit policy: deny, always snapshot, or COW — never silent skip |

### 5.7 Exec allowlist (content hash)

| ID | Threat | Severity | Notes / mitigations |
|---|---|---|---|
| T-HSH-01 | Path allowlist without hashing bytes | Critical | Always hash content (or fs-verity); path is not identity |
| T-HSH-02 | Hash path, then exec different file (TOCTOU) | High | Hash the fd that will be executed |
| T-HSH-03 | Allowlisted interpreter runs arbitrary code | Critical (scope) | Document; ban interpreters or constrain argv; not a hash bug |
| T-HSH-04 | `ld.so` as program + payload args | High | Treat dynamic linker invocation as first-class policy case |
| T-HSH-05 | Scripts via `#!/usr/bin/env …` | High | Hashing env/bash does not bind script body |
| T-HSH-06 | `memfd_create` / `execveat` / in-memory code | High | Kernel-dependent; may need seccomp or deny exotic exec |
| T-HSH-07 | `LD_PRELOAD` into allowlisted binary | High | L3 env sanitization on secure exec |
| T-HSH-08 | Replace allowlisted file in place (same path) | Medium if hash-at-exec | Content hash at exec time catches this; cache must key on inode identity + changeness |
| T-HSH-09 | Performance → cache forever without invalidation | Medium | Cache `(dev,ino,size,mtime/ctime) → hash` in root-owned map |
| T-HSH-10 | Approval fatigue expands allowlist to “everything” | Medium (social) | Product: strict profiles vs expandable with audit |

Algorithm choice (Blake3, SHA-256, …) is secondary. **When and what** is hashed
is the security property. Blake3 is a fine default for speed.

### 5.8 Multi-agent / social

| ID | Threat | Severity | Notes / mitigations |
|---|---|---|---|
| T-SOC-01 | Prompt injection → agent disables advisory hooks | Medium | Enforcing hooks not agent-toggleable; document |
| T-SOC-02 | Prompt injection → agent runs non-rash shell | High without L5 | Enclosure; harness only exposes rash tool |
| T-SOC-03 | Prompt injection → enable post-expansion logging | Medium | Friction + visibility (T-SEC-05) |

---

## 6. Catalog undo model (including recursive tools)

Application-level undo is a **closed catalog**, not a general reverse interpreter.

### 6.1 Structured records (required)

Each undo entry is structured data, for example:

- operation class (`redir_clobber`, `chmod_mode`, `unlink`, …)
- subject path (bytes, not shell-quoted text)
- preimage fields (prior mode, prior target hash/blob ref, prior symlink target, …)
- timestamp, session id, commanding AST fingerprint (optional)

Never: a shell script line built by interpolating paths or modes.

### 6.2 Recursive expansion (e.g. `chmod -R`)

Intended approach (hook or shadowing function):

1. Recognize recursive form (`-R` / `-R` equivalents for the catalogued tool).
2. Walk the tree **before** mutation; for each path record structured preimage
   (e.g. mode bits).
3. Either:
   - run the original recursive tool once (accepting T-UND-04), or
   - apply per-path operations so the log matches the act 1:1.
4. Undo reapplies preimages from the log (privileged, verified).

This is strictly better than logging a single recursive argv. It still requires:

| Constraint | Reason |
|---|---|
| Same treatment for `chown -R`, `rm -r`, … or explicit **not covered** | One tool’s expander is not a framework |
| Identity of the binary forced through policy (L4) | Shadows lose to absolute paths and busybox |
| Honest UX when walk is partial (EACCES, mount points) | Silent partial undo is false safety |
| COW (L5) for everything outside the catalog | Installers, language tools, `dd`, … |

### 6.3 Redirection clobber (`>` vs `>>`)

Best shell-native undo candidate at `redir.c` (resolved path, before open).
Still TOCTOU without L5; still no coverage of writes through already-open fds
inside an external program. See `MUTATION_SAFETY_OPTIONS.md`.

---

## 7. Exec allowlist design notes

**Goal:** only expected program *bytes* run through rash’s exec path.

**Sketch (not implemented here):**

1. Resolve executable candidate for `execve` / `execveat`.
2. Open immutable fd to that content; `fstat`; hash (Blake3 or other).
3. Compare to root-owned allowlist (and optional user-approved extensions with
   the same permission discipline as hooks).
4. Deny or require elevated approval on miss.
5. Optional cache keyed by `(dev, ino, size, mtime/ctime)` → hash.

**Pairs with:**

- L3: drop or ignore `LD_PRELOAD`, `LD_LIBRARY_PATH`, and similar for policy
  profiles that claim hardened exec.
- L5: no second copy of bash outside the allowlist; rash may be installed as
  `bash` on `PATH` so naive agents do not seek another binary.

**Does not replace:** interpreter policy, COW, or kernel MAC.

---

## 8. Fail-closed vs fail-open

| Mode | Behavior on hook error / timeout | Use |
|---|---|---|
| **Enforcing** | Deny execution (fail closed) | Agent production profiles, destroy-class ops |
| **Advisory** | Log and continue (fail open) | Human interactive UX, experimentation |

Mixing them without labels causes T-INT-05. Default for agent-oriented packaging
should be enforcing for the shipped root hooks.

---

## 9. Nix and other environment builders (optional, advised)

**Rash must not require Nix** to build, run, or load hooks. Nix (or Docker,
or a minimal chroot) is an **optional deployment layer** that strengthens L5.

### What Nix (or similar) can provide

| Capability | Threats reduced |
|---|---|
| Store path with **only rash** on `PATH`, optionally named `bash` | T-BYP-01 for processes that only search PATH |
| No stock bash / fewer interpreters in the env | T-BYP-02 surface area |
| Hermetic, hashed store paths | Aligns with L4 philosophy (content identity) |
| Reproducible agent sandboxes | Same controls every session |

### What Nix does not provide by itself

- Policy over command *shape* (still L2)
- Post-exec syscall mediation
- Protection if the agent can leave the env (`/run/current-system`, host mounts,
  `nsenter`, etc.) unless the sandbox is actually closed

### Documentation stance

Advise: “For agent workloads, run rash inside a restricted environment (Nix
devshell, container, or COW launcher) that does not offer a second shell or
general interpreters.”  
Do not: make core rash features `#ifdef NIX` or assume `nix` on `PATH`.

---

## 10. Mapping to existing design decisions

| Design choice (`HOOKS_DESIGN.md` / mutation doc) | Threats addressed |
|---|---|
| Not a security boundary; mistake guard | Sets claim boundary for all of §1 |
| Rack-style `hook(cmd, run)` | Composition without DEBUG-trap races (partial T-BYP-05) |
| `rash.spawn()` only bypass; no env token | T-BYP-07 |
| FFI userdata, on-demand fields | Performance; reduces need for string round-trips (T-LUA-01) |
| Mutation journal via `__newindex`; validate-only default | Silent tree edits; injection via mutation |
| Root-owned enforcing hooks; user hooks advisory | T-INT-01, T-INT-05 |
| Default post-parse (unexpanded) | T-SEC-01 default case |
| OverlayFS / Landlock session | T-UND-01, T-POL-02, post-exec writes in workspace |
| `redir.c` seam for resolved paths | Clobber undo / policy with final lexical path |

---

## 11. Suggested tests when implementing (falsifiable controls)

Prefer tests that fail if a bypass is reintroduced (MFIC-minded):

| Test idea | Threat |
|---|---|
| Env `RASH_HOOK_BYPASS` (or any documented lie) does not skip hooks | T-BYP-07 |
| Enforcing hook error denies a destroy-class command | T-POL-06 |
| World-writable hook dir → refuse load, loud error | T-INT-01 |
| Command word with quotes/newlines appears as one structured field in log | T-POL-05 |
| Absolute path to non-allowlisted bash denied when L4 enabled | T-BYP-01, T-HSH-01 |
| Allowlisted `python3 -c '…'` either denied by policy or documented as out of scope with a visible profile flag | T-HSH-03 |
| `chmod -R` catalog path writes N structured preimages for N files in a fixture tree | T-UND-03 |
| COW session: external writer under workspace discarded with upperdir | L5 / mutation doc |
| Out-of-workspace write denied or left outside rollback boundary (assert boundary) | L5 honesty |

---

## 12. Severity legend

| Word | Meaning here |
|---|---|
| **Critical** | Breaks the trust story if present in a “safe agent” profile |
| **High** | Realistic bypass or damage for in-rash malicious/confused actors |
| **Medium** | Requires extra conditions or causes partial failure |
| **Low** | Edge case or already handled if design is followed |
| **Informational** | Limit of the model; document, don’t “fix” |

---

## 13. Revision history

| Date | Change |
|---|---|
| 2026-08-04 | Initial threat model from independent design review (hooks unbuilt). Includes catalog undo, content-hash exec allowlist, optional Nix enclosure, claims/non-claims. |

When implementation lands, update threat IDs with “mitigated in commit …” rather
than deleting rows — residual risk should stay visible.
