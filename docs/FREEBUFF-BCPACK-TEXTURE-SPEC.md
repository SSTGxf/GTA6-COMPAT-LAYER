# BCPack + Texture Layout Validation — implementation prompt (AFTER the +0x18 watchpoint work lands)

Do this AFTER the current [ctx+0x18] hardware-watchpoint investigation is finished and its boot is done. Do NOT start it before that — the boot loop comes first.

## Part 1 — BCPack decoder, milestone B1 only

Build a standalone C++ BCPack decoder module in ScarlettCompat. Scope is B1 ONLY — do not attempt B2–B5 in this pass.

Background (already researched, do not re-research): BCPack is the Xbox Series hardware codec for BCn texture data — an interleaved 12/16-lane 16-bit rANS entropy layer whose output is a plain BCn block stream. DirectStorage order is DEFLATE → BCPack → swizzle. Decoder input = packed chunk bytes (padding stripped) + codec id + decoded byte count + optional history tables; output = standard BCn blocks in tiled memory order. Decode FIRST, then detile unchanged after (the existing detile code applies as-is to the decoded blocks).

What to build:
1. Entropy layer (rANS) in strict mode + BC1 block rebuilder, as one standalone module with no game data and no GPU dependency.
2. Your own spec-built encoder as the test harness — the encoder exists only to generate test streams for the decoder.
3. Byte-exact BC1 round-trips at multiple block counts (1, 16, 64, 256 blocks minimum).
4. Malformed streams must be REJECTED (fail cleanly, never produce partial output as success).

Licensing (hard rule): the ONLY copyable BCPack code is bcpack-rs `rans.rs` + `tables.rs` (MIT/Apache-2.0), entropy layer only, with attribution. Do NOT copy Leonida's Python decoder or any block rebuilder from it (GPL-3.0 — reference only). Do NOT copy XCL code (GPL-2.0 — reference only).

Known limits (do not try to solve them in B1): bcpack-rs refuses some streams the Python reference accepts (e.g. `q=0` fragments) — verify its accept-set first and document what strict mode covers. The BC6H/BC7 256-entry initial history tables are unpublished — strict-mode coverage in B1 is BC1 (plus whatever BC7 falls out for free); BC6H stays 0% until calibration from real game data exists. **Reference for B2/B3 (read-only, GPL-3.0 — do NOT copy): VIctory's decoder at Marvisak/VIctory `crates/victory-formats/src/bcpack.rs` (1,594 lines) covers BC1/BC3/BC4/BC5/BC6H/BC7 (no BC2), 12/16 interleaved rANS lanes, with history tables in `bcpack-tables.bin` (~78 KB: model frequencies, rank tables, BC7 control words, 255 dictionary entries per format), wired into DirectStorage (`crates/victory-hle/src/modules/dstorage.rs`: zlib inflate first, then bcpack::decode, decode error → INVALID_DATA). Read it to understand the table structure and algorithm; derive your own tables/implementation — do not copy the code or the tables file.** B2 (BC3/BC4/BC5 + BC7), B3 (BC6H + history loading/calibrator), B4 (DirectStorage wiring), B5 (runtime validation) are SEPARATE later passes — do not start them here.

Acceptance for Part 1: B1 module builds clean in the full build (all verify runs still pass), byte-exact BC1 round-trips at the block counts above, malformed streams rejected, zero game data in the repo, zero GPL code copied.

## Part 2 — Validate the texture layout model

The current layout model is the Morton 64 KiB model in `src/xg_texture.cpp` + `src/opt_detile.cpp` / `src/opt_texcache.cpp`. Its in-tile texel order is explicitly NOT verified against the GDK's per-mode tables. Validate it:

1. Cross-check the model's per-mode tables against XCL's published GFX10 tiling model (`texture.rs` — GPL-2.0, reference only: re-express the math, do not copy the code).
2. Where the model disagrees with XCL's tables, correct the table and document the discrepancy (mode, what changed, why).
3. The GPU de-tile path is already proven on the 5700 XT (staging-buffer workaround after the RWByteAddressBuffer UAV device-removal — do not rework it). What needs validating is the LAYOUT TABLE, not the mechanism — the CPU round-trip is already exact inverses.
4. Do NOT change slot 8's copy behavior or slot 25's sizing unless a validation finding proves the current table wrong — validation first, changes only with evidence.

Acceptance for Part 2: a written validation note (per-mode table: model value vs XCL value vs verdict) in `docs/`, plus any proven table corrections, with the full build still green.

## Rules for both parts

- Full build must stay green (all verify runs pass) — if a change breaks the build or any verify run, revert it and report.
- No game data, assets, shaders, or RPF contents in the repo. No FH2 references.
- Do not touch the boot-loop files (`src/pe_loader.cpp` watchpoint code, `src/hle_registry.cpp` slot handlers) unless a validation finding requires it — and then say so first.
- Report when done: what passed, what failed, what is still unverified.
