# Owned map-lookup / parallel-entry tests — October 28, 2026

These are self-contained Windows test programs written in this workspace,
run only in its two game-free Wine prefixes. No game/protection executable,
authentication, game memory, system service or installed app is tested here.
The test helper is uninstalled. It uses the existing cooperative capability
and requires the exact private prefix plus our generated code fingerprint.

## Observable map stage

Execution-runtime SHA 5d0e8f9a10093a8af1fbae31b59917be2ad1b248c4a71bf570982ba5966c41c4
has two recovery call sites at 0xb9d8 and 0xb9ec, both targeting the lookup
routine [0x1b5b8,0x1badc). Their native return addresses are 0xb9dc and 0xb9f0.
A sampled PC in the shared routine alone does not identify its caller; only
an appropriate sampled LR correlates it with a specific recovery call.
Nested calls may change LR, so absence of correlation is inconclusive.
At the two caller return branches, the tested low bit of x0 can be recorded
as a sampled result bit. None of these samples counts distinct events.

The new observer retains a bounded list of native PC/LR and the x26 value
only for its narrow recovery/correlated lookup samples. Static code identifies
x26 as the saved ARM PC in that path. No stack walk, exception interception,
state modification or additional target-memory inspection is added. Read/
teardown failures and event overflow remain explicit. Sampling changes timing.

## Four-consumer program

Each batch publishes four separate generated functions. First-entry cases do
not call them beforehand; warm cases explicitly call each one once before the
consumer threads. During calls the main thread rewrites one valid immediate
byte, an identical byte, or an unused neighboring allocation. Idle controls
make no write. Every old/new active stream has the same instruction boundaries.

The generated guest block is 746 bytes, including a deterministic checksum,
canonical integer/x87 math, version observation bits and a hold loop. A long
guest block is not proof of a long native block: Rosetta may optimize it.
The producer's before-first-tick count observes writes before measured worker
progress, not a guaranteed private decoder/cache miss. Parallel first entry
and invalidation are deliberately separated from any undocumented guarantee.

Independent per-worker final tags, checksums, math, errors, versions and calls
are logged. Per-iteration machine checks retain any invalid version/checksum/
math result. Whole mapping bytes, protections, exact write/flush/change counts,
version checks, progress and joins complete the oracle. No guest trap or fault
handler hides an unexpected failure. Runs stop at the first non-pass.

The rebuilt cold fixture source SHA is
9465dbf1ea2ad529374ac5b4320a6ab6a094b1c05029eea5afc5c2556bc71095;
PE32 SHA 4f5751d4e2e13f01875c8fa3020ff7f0c89f5c2e824f3ca0be83ec43977460ac.
Offline generated-code disassembly confirms the branch targets, pointer
offsets, checksum/return immediates and matching integer/x87 code lengths.
The initial four stock sanity outcomes and all sixteen stock controls pass.
No claim of complete Wine conformance or a game fix follows from them.
