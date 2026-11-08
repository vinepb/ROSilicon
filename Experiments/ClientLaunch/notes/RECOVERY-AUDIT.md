# Current-host decoder and JIT recovery audit — 2026-10-27

Offline file evidence only. No app/game launch, prefix bootstrap, live target
inspection, binary modification or dependency installation. Raw commands,
stdout/stderr, identities, selected ranges and derived comparisons are cached
in this folder. The client date is October 27, America/Sao_Paulo.

## Inputs and scope

- Host: macOS 27.2 / 26B123 (October 26 was 26B118).
- Decoder library: `/Library/Apple/usr/libexec/oah/libRosettaRuntime`, SHA-256
  `a9bf23ac1b8a785060d4bec7337e4403d6bb4ad234e99100a489c5c3de4f292e`.
- Execution runtime: `/usr/libexec/rosetta/runtime`, SHA-256
  `5d0e8f9a10093a8af1fbae31b59917be2ad1b248c4a71bf570982ba5966c41c4`.
  `/Library/Apple/usr/libexec/oah/runtime` is its legacy symlink. No earlier
  execution-runtime input hash is assumed from the decoder-library hash.
- Existing Xcode LLVM 21 tool path/hash and SDK path are in `identity.json`.
  Neither system component is an experiment-owned cleanup artifact.

## Decoder observations

All 80,302 decoded __text instruction words and 178 words in the two extra
executable sections match the historical cached disassembly at the same
addresses. The initial 178 “old-only” words were section-selection differences,
not changed instructions. Tool-skipped zero padding and non-code data are not
claimed byte-identical. The historical text has its own SHA; this audit does
not retroactively assert possession of a raw historical runtime binary.

The decoder at `0x4cb60` and the inspected direct calls at `0x7d4c`/`0x8620`
are unchanged. The observed ordinary decoder frame is still 112 bytes and the
four observed arguments are passed in registers. The inspected code supplies
no new incoming-stack-argument or stale-return-flags correction. Public ABI
checks still do not prove the private recovery contract.

Both system files lack standard unwind sections, DWARF frame records and a
function-starts load command. Raw metadata and empty decoded results agree.
There is therefore no evidence for a fix consisting only of registering a
normal unwind table for the added decoder frame. This is not proof that SP/LR
can never matter indirectly.

## Where this exact assertion occurs

The `found_x86_offset`/line-197 assertion text is in the execution runtime,
not the decoder library. Selected strings, candidate string references and
normal/cold paths are saved separately. Static observations at this exact
current-runtime hash:

1. The path at `0xb598..0xb5d0` reads the ARM word at the saved PC and recognizes
   `0xd42000e0`, ARM `BRK #7`. This leads to the ARM-interval fragment-tree walk.
2. At `0xb608..0xb628`, that walk uses an ARM start/size interval to find the
   containing metadata node. A missing node is a DIFFERENT assertion path.
3. `0xb9b8..0xb9f0` requires the node's byte at `+0x40` to be 4, computes the
   ARM offset, and calls the lookup at `0x1b5b8` with two map descriptors
   (`node+0x60`, then `node+0x78`). If both return false, execution branches to
   `0xc284` and reports `found_x86_offset` at source line 197.
4. The lookup supports several packed/stream forms and exact ARM-offset
   comparisons; its failure return is at `0x1ba4c`. This is an instruction-map
   lookup, not ordinary C stack unwinding. Do not assume the sampling helper's
   simplified map names describe every variant in this path.
5. The runtime itself constructs/stores that same BRK word while patching
   translated regions, for example `0x802c..0x8098` and `0x13b50..0x13b60`.
   No changes to these paths are proposed or made.

This ties the message more closely to runtime-managed translated-code
breakpoint recovery and its instruction maps. It does not yet identify why
the decoder wrapper causes a later missing entry, prove the relevant game
mutation pattern or establish that any particular invalidation API triggers it.
No live game code/protection data was consulted.

## Consequence for the next owned experiment

The 216 passing Windows exception/cold-entry/context-transfer outcomes do not
guarantee delivery of this specific runtime-managed recovery condition.
Prefer a narrowly paired active-code invalidation experiment over another
general SEH/unwind matrix: keep an owned function executing while a second
owned thread requests cache invalidation/protection transitions on its page,
with the instruction bytes immutable and execution allowed throughout.

Use unchanged-page/no-invalidation and unused-neighbor controls. Start with an
integer loop and only then an x87 loop; compare stock before decoder-on/off.
Require bounded writer progress overlapping measured consumer execution,
unchanged instruction bytes, exact final tag/math and a joined consumer; retain
any assertion, unexpected fault or shared stock failure. Do not modify active
instruction bytes, mask signals, access another program or change a helper to
force a pass. First review existing threaded fixtures to avoid duplicating an
equivalent case. A passing run without an observed BRK/recovery marker cannot
be called coverage of the private path.

This experiment is scoped/planned, NOT implemented or run by this static audit.
The deeper fix remains unresolved; no new game comparison is queued.

Existing-fixture review: `probes/src/breakpoint_probe.c:thread_test(1)` already
runs four consumers for 4,000 total short function calls while the producer
atomically alternates the first byte between INT3 and NOP and flushes it 128
times. It explicitly describes itself as crash stress, not cache-ordering
conformance. Do not claim concurrent mutation/invalidation has never been
tested. The proposed refinement is a long-lived immutable function, separately
controlled protection/cache operations on its active page and observed progress
overlap, avoiding the extra guest INT3 and changed-byte variables. It still
needs a coverage marker before a pass proves the private BRK #7 path ran.
