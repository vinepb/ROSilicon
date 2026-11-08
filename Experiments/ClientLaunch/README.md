# Archived Wine / x87sidecar compatibility investigation

This directory preserves source, experimental patches and findings from the
October–November 2026 launch investigation. It is an archive, not shipping
runtime code. Do not apply its experimental patches to normal builds by default.

The reported game failure was Rosetta's `found_x86_offset` assertion. An earlier
Wine correction fixed stale translated code after WriteProcessMemory when
current protections differed from the original allocation protections. QA
reached login with stock Rosetta, while the optional decoder wrapper remained
associated with failures. The exact game assertion was not reproduced by the
standalone programs, and no deeper decoder-enabled game fix was established.

Owned standalone programs also exposed a separate protection-transition race:
temporarily removing/restoring execute permission in separately locked calls
can expose a non-executable page to another thread. A local lock candidate
passed 48 reporting combinations and sixteen focused regressions. An owned
parent/child case then reproduced the corresponding remote-write fault. The
target-side transaction candidate adds one internal APC, versions its matching
Wine client/server protocol from 962 to 963, and passed its initial remote
sanity case. Its broader matrix and game validation were not completed.

On November 7 the user reported the unmodified standard app working again after
a game update. Its Wine 11.15/r17 and x87sidecar v1.7.0 had not received these
experimental fixes. That observation does not identify the exact changed game
component or establish that every independently found runtime defect is gone.

## Contents and replay limits

- `qa/` preserves owned C/C++ fixtures, builders and bounded runners with their
  original relative directory layout. The programs operate only on isolated
  test prefixes and, for remote tests, children created by the program itself.
- `patches/` preserves the separate self-write and target-transaction candidates.
  The target patch requires matching ntdll and wineserver builds and regenerated
  protocol headers; never mix protocol versions in a running prefix.
- `helper-patches/` records loader experiments against the base snapshot named
  in `source-manifest.json`. Restore that upstream source before replaying them.
- `notes/` keeps findings, negative results and coverage limits. Earlier native
  signal failures remain unresolved; passing focused cases are not full Wine
  conformance or proof that the game works.

Original tested sources and exact hashes remain in the machine-local ignored
investigation inventory. Exported paths use `__INVESTIGATION_ROOT__`,
`__WORKSPACE__` and `__USER_HOME__`; configure these for a new isolated replay.
The source manifest records hashes before and after path redaction. Native
helper source pins include v1.7.0 and upstream commit
`499e0f7e60d2961d379df459dd96bc79ecfe59fd`; Wine experiments use WineAndAqua
11.19 at `479d46a96a325caff09e6143cb0636fcbe565ff9`.

Build products, SDKs, binaries, prefixes, game data and raw game/protection logs
are deliberately excluded. No anti-cheat, authentication or access-control
modification is part of the owned synthetic test programs in this archive.
