# Boots #173–#177 — GPU-copy path for DEFAULT-heap CBV sources

**Date:** 2026-11-25 ~01:30–02:10 EST. **Builder:** Muse (PC via SSH).
**Result: the GPU copy is PROVEN — all 8 unaligned CBVs now carry real data.
The game is past the 0x1425A087D read crash. New stop: int3 at 0x141DAC734,
after DStorage refuses an unresolvable source hash (the hash gate).**

## The problem (boot #172)

Boot #172 proved the CBV unaligned-location fix (game past the
DEVICE_REMOVED assert 0x142474606), but all 8 unaligned CBVs were
zero-filled: their sources are GPU-only (not CPU-readable), so the
Map-copy path could not read them. If a constant holds a pointer or an
offset, zero gives a bad address — the leading suspect for the read AV
at 0x1425A087D (boot #172, line 15960).

## Three bugs found and fixed in the GPU-copy path

### Bug 1 — the copy only ran for DEFAULT-heap (boot #173)

The first GPU-copy branch only fired for `D3D12_HEAP_TYPE_DEFAULT`.
Boot #173: the copy never ran (no "copy queue ready" line), all 8 CBVs
still zero-filled, same crash at 0x1425A087D. The game's buffers are
Xbox-style CUSTOM-heap (recorded verbatim by slot 127), not DEFAULT.
**Fix:** the GPU-copy branch now runs for any non-UPLOAD heap
(`else` instead of `else if (== DEFAULT)`).

### Bug 2 — guest VA vs native VA (boot #174)

Boot #174: still no copy, still zero-filled, still 0x1425A087D. The
8 CBVs point into a 182 MB placed buffer at guest VA 0x223ce1c0000
(slot 133, `CreatePlacedResourceX va=0x223ce1c0000 w=182452224`).
Slot 133 recorded the resource under its **native** GPU VA
(`GetGPUVirtualAddress()`), but Model A creates it via
`CreateCommittedResource`, whose native VA differs from the guest VA
the game uses to reference it (CBV `BufferLocation`). The map lookup
therefore never found the source.
**Fix:** new `ScCbvRecordSourceAt` records slot 133 resources by the
**guest va** parameter. Slot 127 (committed resources) keeps the
native VA, which is the VA the game reads there.

### Bug 3 — UPLOAD cannot be a copy destination (boot #176)

Boot #175: the copy queue initialised ("copy queue ready") and the
game got **past 0x1425A087D** to a new int3 at 0x141DAC734 — but the
CBVs were still zero-filled. Boot #176 (with per-step logging) named
it: `List Close failed hr=0x80070057` (E_INVALIDARG), then every
subsequent `List Reset` failed the same way (the list was left in a
bad state). Cause: an UPLOAD-heap buffer cannot be a
`CopyBufferRegion` destination — it cannot leave GENERIC_READ, so the
COPY_DEST barrier made the command list invalid and Close failed.
**Fix (boot #177):** a READBACK staging buffer. The GPU copies
`src -> READBACK` (READBACK is created in COPY_DEST, no barrier
needed), then the CPU Maps the READBACK buffer and memcpys into the
UPLOAD scratch. One reusable READBACK buffer (4 KB, grown if a larger
copy is requested).

## Boot #177 — GPU copy PROVEN

- 16,084 lines (furthest yet: #172 was 16,079, #175 was 15,988).
- All 8 unaligned CBVs now **(copied)** — real data, not zero-filled:
  `loc=0x2410580ff20 size=256 -> scratch 0x315767000 (copied)`, and
  the other seven the same (sizes 256/512).
- No `CBV GPU copy` failure lines. The copy queue, fence and READBACK
  staging all work on the RX 5700 XT.
- The game is past 0x1425A087D.

## New stop: int3 at 0x141DAC734 (hash gate)

Boot #177 (and #175) crash with exception `0x80000003` (int3) at
`0x141DAC734`. Immediate context: a flood of DStorage Submit refusals
for an unresolvable source hash — `#177: 0xb76e17e552`
(`destSize=472 offset=0x1c2767f0 compSize=259`), `#175: 0xa01f10e922`
(same dest/offset/compSize). The game submits the same request 16+
times, all refused E_NOTIMPL, then hits the int3 — a deliberate
assert, most likely "failed to load required data". The hash gate is
now the blocker: the pack manager does not know which RPF holds
these hashes. Earlier unresolvable hashes: 0xd4a5bde7c2 (#169),
0xf5b62fe5b2 (#172), 0xc31174ea62 (#173), 0xe2260fe6a2 (#174).

## Other notes

- **Proof-guest flake (build 173):** the first build-173 run failed
  with `BUILD_EXIT=1` — the proof guest crashed in
  `D3DCOMPILER_47.dll+0x25a63a` after passing 17/17 stages. The
  GPU-copy code never ran during that build (no unaligned CBVs in
  the synthetic guests), so it was not the cause. A direct retest of
  the proof guest passed (`ALL PASS`, exit 0). The rebuild then
  passed `BUILD_EXIT=0`. Treat as a flake.
- **Slots 67/62 still E_NOTIMPL** (vtable +0x218 / +0x1f0, call
  #1728 in boot #177) — firing right before the crash, as in #172.
  Still unidentified; they may matter once the hash gate closes.
- **Dumps:** boot #177's dump is 0 bytes (the dump-vanishing issue).
  F: ~8.1 GB free.
- **Progress:** ~44% proven toward first menu (unchanged — the
  GPU-copy path closes a real gap, but no frame yet and the hash
  gate is the current wall).

## Next steps

1. **Hash gate (current blocker):** resolve hashes 0xb76e17e552 /
   0xa01f10e922 (and the earlier ones) to their RPF. The pack
   manager needs the mapping, or the Submit path needs to serve
   these hashes from the right file.
2. Identify slots 67/62 (E_NOTIMPL right before the crash).
3. Slot 86 (CreatePipelineState) still E_NOTIMPL — the standing
   PSO wall; shader capture is inert until it lands.
