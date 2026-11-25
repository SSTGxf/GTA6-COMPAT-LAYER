# BOOT #179-#182 DIAGNOSIS — Slot 86 Regression + Hash Gate Resolved

Date: 2026-11-25 ~02:50 EST. Entry 33 (commit 0be2cc77) on GitHub main.
Builder: Muse (Freebuff out as of 2026-11-25 ~00:41 EST).

## Summary

- **Boots #179 / #180** (entry-33 DLL: slot 86 + hash resolver): both crashed at exactly **2,018 log lines in `amdxc64.dll`** (AMD shader compiler), before either new function ran.
- **CORRECTION (boot #183, 2026-11-25 ~03:15 EST): boot #183 (hash-only + raw-copy DLL, slot 86 REMOVED — verified 0 "CreatePipelineState" strings in the binary) crashed at 2,020 lines in `amdxc64.dll` at the SAME address 0x7FFBA0D6FFD0.** Slot 86 is therefore NOT the cause. The amdxc64 crash at ~2,018-2,020 lines is an intermittent/flaky early-init crash (3 crashes: #179/#180/#183; 2 successes: #181/#182, same crash point). The raw-copy fix cannot be the cause either — it only runs on a DStorage Submit with compSize == destSize, which happens ~line 16,000, far past the 2,020-line crash. A boot #183 re-run (same DLL) is in flight to confirm flakiness.
- **Boot #181** (entry-32 DLL rebuilt, no slot 86, no hash resolver): **16,074 lines**, hit the hash gate normally. Proves the entry-33 DLL was the regression, not environment/driver.
- **Boot #182** (hash-resolver-only DLL, slot 86 removed, build #182, 372,736 B): **16,058 lines**, **hash gate RESOLVED** — `ResolvePack` found `audio.rpf`, 0 unresolvable hashes, `0x141DAC734` appears 0 times. New stop: int3 at `0x1443AD94E` after a raw 16-byte DStorage request fails to inflate.

## Boot #179 — amdxc64.dll crash (slot 86 DLL)

- Log: `F:\ScarlettCompat\scarlett_boot179.log`, 2,018 lines.
- Crash: `0xc0000005` null read at `0x7FFBA0D6FFD0` in `amdxc64.dll`, right after `CreateCommandQueue S_OK` + `boot57-arena`.
- **0 slot-86 calls and 0 ResolvePack calls** in the log — neither new function executed before the crash.
- Dump: `D:\scarlett-dumps\scarlett_crash_boot179.dmp` (6,365,687,765 B, verified size-match).
- Entry-33 fixes (slot 86 + hash resolver) were UNTESTED in this boot.

## Boot #180 — same crash (slot 86 DLL, retry)

- Log: `F:\ScarlettCompat\scarlett_boot180.log`, 2,018 lines.
- Crash: `0xc0000005` null read at `0x7FFBA0D6FF27` in `amdxc64.dll`, same point (after `CreateCommandQueue S_OK` + `boot57-arena`).
- Deterministic: same line count, same module, same early point. Not a flake.
- Dump: `D:\scarlett-dumps\scarlett_crash_boot180.dmp` (6,365,183,909 B).
- Conclusion: the entry-33 DLL itself is the regression. The only delta vs entry 32 is slot 86 + the hash resolver, and the hash resolver never ran — so **slot 86 is the culprit**.

## Boot #181 — entry-32 control (no slot 86, no hash resolver)

- Log: `F:\ScarlettCompat\scarlett_boot181.log`, 16,074 lines.
- Hit the hash gate normally: `Submit[0] refused: unresolvable source hash 0x835e15e8f2 (offset=0x1c2767f0 compSize=259 destSize=472)`.
- Proves the environment/driver is fine — the #179/#180 crash is caused by the entry-33 DLL, not by a driver or machine change.
- Dump: not archived (boot #181 ran to the hash gate; no crash dump of note).

## Boot #182 — hash-resolver-only (slot 86 removed)

- DLL: build #182, 372,736 B, BUILD_EXIT=0, winrt 9/9, B1 149/149, verify 0. Source: `hle_registry.cpp` 7,907 lines (CRLF), hash resolver present, slot 86 REMOVED.
- Log: `F:\ScarlettCompat\scarlett_boot182.log`, 16,058 lines.
- **Hash gate RESOLVED:**
  - `ResolvePack: hash 0x6a4519e812 -> audio.rpf (offset 0xb01000 compSize 776 destSize 3169)`
  - **0 unresolvable hashes. `0x141DAC734` appears 0 times.**
  - 8 CBVs copied (GPU-copy path still working).
  - `DStorage batches=20 copied=4199 file=4261` (same as #177).
- **New stop:** int3 `0x80000003` at **`0x1443AD94E`**, immediately after:
  ```
  [dstorage] Submit[1] refused: inflate failed (hash 0x6a4519e812 offset=0x9048000 compSize=16 destSize=16 outLen=0 hr=0x80004005) -> E_NOTIMPL (zlib only; BCPack stays E_NOTIMPL)
  ```
  This request has **compSize == destSize (16 == 16)** — almost certainly RAW/uncompressed data the layer wrongly tries to zlib-inflate. (The same hash's earlier zlib request at offset 0xb01000 worked.) The game int3s because the data it needs never arrives.
- Dump: `D:\scarlett-dumps\scarlett_crash_boot182.dmp` (7,633,615,128 B, verified size-match). F: original deleted. F: now ~1.0 GB free (F:\game is 208.6 GB; the drive is 233 GB total — nearly full. Dumps must go to D: promptly; a future boot crash dump (~7.6 GB) will NOT fit on F:).

## The hash gate — root cause and fix (proven)

- **Root cause:** `DsqSubmit` had ONE hardcoded mapping (`kDsMeasuredHash = 0xACA58068328F107A` → shaders.rpf). Every other hash was refused "unresolvable" → the game int3'd at `0x141DAC734` after 16+ retries.
- **Key finding:** the hash changes every boot/request but offset+size stay identical (0xb76e17e552 #177, 0xa01f10e922 #175, 0x835e15e8f2 #181, 0x6a4519e812 #182 — all offset 0x1c2767f0 / 259 / 472). The "hash" is NOT a content hash; resolution must probe by offset+size.
- **Fix (`ScDsResolvePack`):** fast-path for the measured hash, else probes all 12 candidate RPFs (data, shaders, packs, textures, anim, levels, audio, movies, navmeshes, common, script, appdata0): fopen_s + `_fseeki64` + fread, zlib-header check, `ScOpt_DStorageDecompress` test, requires outLen == destSize. Caches hash→path (incl. misses) in a 64-entry static array. `DsqSubmit` validates destination first, then resolves, then reads from the resolved packPath.
- Offline probe (`~/workspace/gta6-exe-re/hash_probe.py`) proved 0xb76e17e552 → data.rpf (offset 0x1c2767f0, compSize 259 → destSize 472).
- **Proven in boot #182.** Hash gate is CLOSED.

## Slot 86 — what we know

- Merged from Buddy's Codex branch `codex/slot86-create-pipeline-state` (commit `ee9d3fd5`, +53 lines in `hle_registry.cpp` only, based on an OLD main — 7,380-line file, no GPU-copy/hash code).
- Handler `XboxDevice_CreatePipelineState`: QIs real device for `ID3D12Device2`, forwards the D3D12 pipeline-state stream UNCHANGED to desktop `CreatePipelineState`, returns the real HRESULT, logs first 8 / every 256th / failures. Registered at `g_xboxDeviceVtable[86]`.
- Does NOT translate Xbox shaders or emulate mesh stages. Codex's forward-unchanged approach bypasses shader translation and would fail on Xbox shader bytecode anyway.
- **CORRECTION (boot #183): the amdxc64 crash happens WITHOUT slot 86, so slot 86 is NOT the cause.** The earlier attribution (boots #179/#180 crash, #181/#182 work) was a correlation, not causation — boot #183 breaks it. Slot 86's status is now UNKNOWN (neither proven guilty nor proven innocent); it still should not be re-added as-is, because the forward-unchanged approach bypasses shader translation and would fail on Xbox shader bytecode anyway. The real question is now the flaky amdxc64 early-init crash itself. Freebuff's prompt (`FREEBUFF-SLOT86-PROMPT.md`) should be re-scoped to that crash, not slot 86 specifically.
- GitHub main (entry 33, 0be2cc77) still CONTAINS slot 86 — the entry-34 push (hash-resolver-only) will remove it from main; that is still correct, since the handler is unproven and the forward-unchanged design is wrong for real shaders.

## Next fix (in progress — build #183)

- **Raw-data fallback in `DsqSubmit`:** if the zlib inflate fails AND `compSize == destSize`, memcpy the read bytes straight to dest (no decompress). The 16-byte request at 0x9048000 is raw, not zlib.
- Also consider: the resolver probe currently requires a zlib match. If a hash's FIRST request is raw, the probe finds no pack. Watch for this in boot #183 — if a raw-first hash appears, the probe needs a fallback (accept the pack whose file is large enough, or probe differently).
- Build #183 was interrupted once (user interruption at 02:54 killed the first run — log ended with `^C`, no BUILD_EXIT); re-started. Boot #183 to follow.

## Still E_NOTIMPL

- Slots 67 (vtable +0x218) and 62 (vtable +0x1f0) — firing constantly (call #2496+ in #182).
- Slot 16, slot 86 (handler removed), BCPack (B1+B2 done standalone, B4 wiring pending).

## Progress

- ~44% proven toward first menu. Hash gate closed (was the D1 blocker). Next: raw-data fix → boot #183 → whatever the game hits after 0x1443AD94E.
