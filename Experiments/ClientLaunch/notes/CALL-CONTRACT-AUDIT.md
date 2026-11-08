# Decoder call-contract audit — 2026-10-12

Machine-local investigation; not a shipping correction or exact assertion
reproduction. Production and MTLD3D QA are excluded.

## What the failed linked-frame comparison establishes

The v2 helper preserves the previous pure wrapper's 96-byte frame and x0/x1,
x2/x3 and caller-LR spills, adds a normal linked FP/LR record, and performs no
post-decode inspection or optional alias substitution. All native frame/spill
and decoder gates pass, but standard QA still asserts before login at
`0x202aaa018`. Adding a frame-pointer link is not sufficient to fix this run.
QA is stopped, with its identities/preferences and other installs preserved.

## ABI checks and limits

[Apple's ARM64 ABI documentation](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms)
requires 16-byte SP alignment, reserves x18 and describes a valid x29 frame
record. Public ABI rules do not document Rosetta's private decoder or recovery
mechanism. The local documentation snapshot is `references/apple-arm64-abi.md`.

The cached local decoder prologue/body uses a 112-byte frame, takes the observed
context/offset/output/output-length values in x0..x3, preserves x19..x30, and
returns status through w0. No incoming stack-argument load has been identified
in that recovered body. The pure wrapper leaves the argument registers and
status intact and remains 16-byte aligned. These checks do not prove a private
signature, hidden TLS/recovery contract, or that all nonlocal exits are normal
returns. Do not create a stack-argument fix without further evidence.

The added call changes original-decoder entry SP and LR, even with a linked
frame. Own frame walking is not equivalent to private fault recovery or compact
unwind metadata. A timing/translation-lifecycle effect also remains possible.
The existing extra-read hazard under simulated mapping revocation is separate:
the pure wrapper avoids it and still fails in QA.

## Refreshed upstream baseline

Cached upstream master is now pinned to
`3726b958791cd2e14f7db4fe1104434a5fb68bf6`, newer than our v1.7.0 experiments.
The source archive is SHA-256
`b1dad32cb2902b628757a5b30177d64d3c6e6482beddef445ed974b8f3b89829`.
The archive's sole symlink is internal:
`compile_commands.json -> build/compile_commands.json`; never follow it for
cleanup. No AGENTS instruction file is present in this snapshot.

[PR 39](https://github.com/athei/x87sidecar/pull/39) fixes allocation/state/flags
bugs and adds an opt-in precise first-access fault path. Its documented fault
message is different from this game's `found_x86_offset` assertion. Later
floating-point accesses still have limitations; the option is not a universal
recovery fix and can have substantial performance costs. Do not enable it as a
game default or report its benchmarks as our measured game performance.

[PR 38](https://github.com/athei/x87sidecar/pull/38) adds cooperative unhooked
fallback/rollback, so every accelerated fixture gate must reject an unhooked
fallback even if the program returns correct results.
[PR 42](https://github.com/athei/x87sidecar/pull/42) changes default-attach release;
it must not be presented as a proven cooperative-game fix. Host-opcode PR 37
remains open. No upstream change claims to fix this exact game assertion.

The refreshed `stub_asm.cpp`, including its decoder wrapper emitter, is
byte-identical to v1.7.0. Main translation/state and loader behavior changed.
Build the new upstream helper separately and run the owned decoder matrix plus
upstream correctness/fault samples before considering another ordinary QA run.
Use no protected-game tracing, stack/memory inspection, anti-cheat modification,
privileged attach, new dependency or persistent installation change.

## Owned fault/lifecycle and return-call audit — 2026-10-14

The pinned October 12 current helper still fails the ordinary game launch;
that run is stopped. New static fetch/data boundary controls pass 72 outcomes
with exact PCs/fault addresses, one-shot guards, retry/tag/math/INT3 checks.
An immutable RET-ending function plus a concurrently protected unused neighbor
passes all 96 full-matrix outcomes with measured progress across consumer-call
intervals. Current, decoder-off and pure linked/unlinked PE32 variants are
verified, alongside stock PE32/PE64. No matching assertion was reproduced.
These controls do not deterministically force an internal decoder read or
nonlocal recovery, and are not complete Windows/Wine conformance.

Reused `references/stock-runtime-disassembly.txt`, no new runtime extraction:

- Direct BL at text offset `0x7d4c` targets `0x4cb60`; the next instruction
  is CBNZ w0. Its observed success path loads the output length, then compares
  the output mnemonic before conditional flag-dependent branches.
- Direct BL at `0x8620` targets the same decoder; CMP w0,#2 and CMP w0,#1
  set new flags before their conditional branches, then the length is reloaded.
- The pure emitter at `source-passthrough/rosetta_loader/src/stub_asm.cpp`
  instruction 5 is an unconditional DONE branch rather than CMP. This path
  does not inspect status, NZCV, output/context or rejected instruction bytes.

No stale incoming/returned-NZCV dependency is identified at these two direct
calls, and no return-flags correction is justified from them. Indirect callers,
abnormal returns and a proprietary SP/LR/recovery contract remain unproven.
Next owned test direction is cold translation combined with asynchronous
signal/context delivery: prior native signal tests largely reused a hot gate,
and these new cold/page tests had no independent signal producer. Scope the
oracle first; do not turn this hypothesis into another blind game variation.

## Windows callback/context transfer controls — 2026-10-26

The separately built October 19 upstream helper still fails ordinary QA on
macOS 27.2 with the exact assertion. Two new owned full matrices pass 216
declared outcomes: nested cold child calls from Windows exception callbacks,
and saved-PC redirection across separate published allocations. Each covers
stock PE32/PE64 and four verified helper PE32 selections, stable/WX/self-WPM,
first/prewarmed entries, depths 0/1/3 and 512 rounds. Stack/argument/parent
context, exact traps/transfers, return tags and stored canonical math match.
No assertion, timeout, unknown exception or fallback. All inputs and installed
components/preferences are audited unchanged; only private test prefixes run.

These oracles do not force a fault/nonlocal exit from Rosetta's private decoder
while the additional wrapper SP/LR frame is active, nor prove its private
unwinding/recovery contract. A published-code change does not prove an internal
decoder cache miss. Pure controls have an older core, limiting causal use of
any future differing result. No additional game test or helper edit follows
solely from these negative results.

Before extending the old static SP/LR audit, pin current-host decoder/caller
evidence and determine whether relevant unwind/recovery metadata is present.
The prior detailed disassembly is from a different Rosetta hash. Offsets staying
the same is not enough to assert private body/recovery behavior is unchanged.
Keep earlier native stock/helper mixed-signal `_sigtramp` mismatches separate,
and derive an owned differentiating recovery case before claiming a deeper fix.

## Current static recovery evidence — 2026-10-27

The current decoder library's 80,480 decoded instruction words match the older
cached text, including the previously omitted executable sections. Both the
decoder library and the separately pinned execution runtime lack standard
unwind/function-start tables. No incoming-stack-argument, stale-flags or
standard-unwind-table correction is established.

The exact `found_x86_offset` fatal branch is in `/usr/libexec/rosetta/runtime`,
SHA `5d0e8f9a10093a8af1fbae31b59917be2ad1b248c4a71bf570982ba5966c41c4`.
Its current-hash path recognizes native BRK #7, identifies the ARM fragment and
fails two instruction-map lookups; this failing lookup does not walk a normal
C frame chain. Runtime code also writes that word while patching translated
regions. Extra decoder SP/LR could still matter indirectly, but ordinary C
unwind metadata is no longer the best-supported immediate experiment.

Prefer a narrowly paired owned active-page invalidation control with immutable
instructions and concurrent measured execution, after reviewing existing
threaded fixtures. No new probe or helper change was made in this static phase.
Full raw metadata, exact paths/offsets, limits and planned oracles are in
`references/rosetta-static-oct27/RECOVERY-AUDIT.md`; all installed inputs remain
unchanged and no game process was launched or inspected.
