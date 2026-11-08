# Owned instruction-layout control and protection race — 2026-10-28

The PE32 code in this workspace is written for the investigation and uses only
the two isolated game-free prefixes. No game, protection executable, existing
process, credentials or installed application is inspected by these tests.

The original layout source/binary remain frozen:

- source e2fd4352a9920ac179a948891cb3719aaa0c3935ac271c90927f755f6e876bc5
- PE32 90f3e3f596c0ff1bde763f0e2993e5ec4879104b01d383652e1d864500651808

At generated entry+32, changing B8 to 90 changes `MOV EAX,90909090` into five
NOPs. All other bytes remain identical; both streams have checked version,
checksum, math and return oracles. Old/new integer/x87 streams were independently
disassembled in `runs/map-lookup-preflight-oct28/`. This is bounded concurrent
cross-modification stress, not an architectural ordering guarantee. The code
is 767 guest bytes; its native block size/decoder timing are not assumed.

The first stock changed-byte/x87/first-entry case exited 5 without final output.
The exact SEH diagnostic retry and eight further exact retries passed. Its
exception cause is unrecorded; do not retroactively label it as the later fault.

## Reporter and captured negative

The separate `layout_exception_probe.c` includes the unchanged original source
and registers an unhandled-exception reporter. It records only its own exception
code, instruction address, selected x86 registers and access-fault parameters,
then returns EXCEPTION_CONTINUE_SEARCH. It does not recover, mask or modify a
fault. The selfcheck deliberately writes to its own PAGE_NOACCESS allocation;
the correct report and exit 5 were verified before stress, and again under the
candidate. No test failure is hidden by treating an unexpected fault as success.

Reporter source SHA c2f6d7dbacb7d1604338d9114ca17027931e43aa813288a0fc8c713f5f745ec6;
PE32 SHA 7882ca4b0ba67cf7f3b65f07597f3cc124d039df1cc100818a135dbc73c898f8.
Sixteen stock changed-byte/x87 reporting runs passed. A broader paired sequence
then passed its four stock idle cases and FAILED its first stock same-byte/
integer/first-entry case. It stopped before either helper configuration.

Exact negative: `runs/layout-exception-paired-oct28/stock-same-byte-integer-first.log`.
Exception c0000005, access 8 (execute), EIP and fault address 0x25000024.
That is the first NOP after the own function's argument load. Rewriting the
identical B8 byte changes no instruction boundary. Thus this captured negative
does not require x87 instructions, a decoder wrapper, changed bytes, or the
game. It is not the found_x86_offset assertion.

## Source-supported candidate

`toggle_executable_pages_for_rosetta` removes and restores EXECUTE through two
NtProtectVirtualMemory calls. Each takes/releases virtual_mutex separately,
temporarily exposing non-executable guest metadata. `virtual_handle_fault`
locks that same mutex; its Apple execute-fault retry requires current EXECUTE
metadata. The source initializes this mutex as PTHREAD_MUTEX_RECURSIVE.

The candidate wraps the whole local query/toggle/restore transaction in one
signal-masked recursive lock. It changes no public protection API, suppresses
no explicit fault and leaves the existing current-protection/region walk in
place. It is limited to process == NtCurrentProcess(). Other process handles,
including an explicit handle for the same process, retain the old APC pairs.
A separate target-side transaction is needed before calling this a complete
remote invalidation correction.

`build/wine-atomic-self/` holds original/candidate source, original-build object
hashes, exact compile/link argv and SDK metadata, one new virtual.o/ntdll.so,
and an APFS clone of the existing 370 MiB runtime. Only the intended native
ntdll differs. No shared Wine source/build or installed runtime was replaced.
Source diff: `references/wine-atomic-self.patch`; build log:
`references/wine-atomic-self-build.log`.

Candidate ntdll SHA 1d7422cf6ce0c1223edcae6ec59a88300e29cd6ed2d1022eee3770170345ff4e.
All 48 combinations pass with independently checked oracles: stock plus the
same upstream helper's decoder off/on; four modes; integer/x87; first/warm;
sixteen batches. Sixteen existing PE32/PE64 controls also pass, covering stale
code, executable allocations, explicit protection toggles, mixed regions,
writes to a child created by the program itself and threaded INT3/mutation.
The owned child test has synchronized execution and does not cover concurrent
remote invalidation. The original stock failures remain preserved.

All tests use current macOS 27.2/26B123 and recorded Rosetta hashes. Historical
QA login success on 27.0.1 is not a game validation on this host. No new game
test or functional x87sidecar correction is claimed. Earlier native mixed-
signal failures remain open. Next: an owned concurrent parent/child reporter
case, then a clean target-side invalidation design if that race reproduces.

Supplemental component checks: the standalone baseline's whole-file ntdll
hash is 821d97e002f1b7a630c717918fe5e07e32ab1c147fd18d313dc6838efaf5c753;
the original build's hash is 86615f8587e5f7fcbc6aa92e62e20b97f59475c98c7e5b51b700e5f941048512.
All stored Mach-O sections match exactly between them; use the actual per-run
whole-file identities rather than assuming identical packaging. Raw section
hashes are in `runs/map-lookup-final-audit-oct28/baseline-native-sections.json`.
The separate one-case dyld check passed, showing the candidate native ntdll
path/UUID and no baseline ntdll image; it has its own logs and frozen inputs in
`runs/layout-atomic-self-load-check-oct28/`. This is in addition to, not silently
included in, the 175-outcome audit.
