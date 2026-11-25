# Boot #178 diagnosis + slot 86 / hash-gate (entry 33)

## What entry 33 adds

Two changes, both in `src/hle_registry.cpp`:

### 1. Slot 86 — CreatePipelineState (Codex branch, Buddy's work)

- Source: branch `codex/slot86-create-pipeline-state`, commit
  `ee9d3fd5` ("d3d12: forward Xbox slot 86 pipeline creation", by Codex).
- The branch adds a single function, `XboxDevice_CreatePipelineState`, plus
  one vtable line (`g_xboxDeviceVtable[86] = ...`). The function in this
  tree is byte-identical to the branch's (verified by diff); the only
  difference is a header comment noting the branch + commit.
- What it does: forwards the `D3D12_PIPELINE_STATE_STREAM_DESC` unchanged
  to the desktop device (`ID3D12Device2::CreatePipelineState`), and returns
  the real HRESULT. It logs the first 8 calls, every 256th call, and every
  failure (with the stream pointer, the requested IID, the HRESULT, and the
  return address).
- What it does NOT do: translate Xbox shader bytecode or emulate mesh
  stages. If the stream holds Xbox-only shaders, the desktop runtime will
  reject the create. But the game now gets a real HRESULT instead of
  `E_NOTIMPL`, so it can proceed (or fail) on real terms.
- Slot 86 was the standing PSO wall: the game calls it 28,000+ times per
  boot (boot #162). This is the first real implementation.

### 2. Hash-gate resolver (`ScDsResolvePack`)

- Before: `DsqSubmit` refused every file-sourced request whose hash was not
  the single measured shaders.rpf hash (`0xACA58068328F107A`). The game
  hit this 16+ times per hash and then int3'd at `0x141DAC734` (boot #177).
- Now: `ScDsResolvePack(hash, fileOffset, compSize, destSize)` resolves a
  hash to its RPF by probing. For each candidate RPF it reads `compSize`
  bytes at `fileOffset`, checks for a zlib header (`0x78` + valid second
  byte), and inflates — the pack whose stream inflates to exactly
  `destSize` is the source. Results are cached per hash (including misses),
  so each hash probes once. The measured shaders.rpf hash is a fast path.
- Proven offline: hash `0xb76e17e552` (boot #177) and `0xa01f10e922`
  (boot #175) — same request (offset `0x1c2767f0`, compSize 259,
  destSize 472) — both resolve to **data.rpf**. At that offset data.rpf
  holds a `0x78da` zlib stream inflating to exactly 472 bytes; the other
  six candidate RPFs hold no zlib stream there. (Note: the offset
  `0x1c2767f0` = 472,000,240 is past the end of shaders.rpf, so the old
  hardcoded pack could never have served it.)
- The probe uses `fopen_s` + `_fseeki64` (64-bit offsets; some RPFs are
  76 GB) and the existing `ScOpt_DStorageDecompress` for validation.
  `DsqSubmit` now resolves the pack first, then reads + decompresses from
  the resolved path instead of the hardcoded shaders.rpf path.

## Boot #178 — flaky early crash (neither fix tested)

- Boot #178 ran 236 lines and crashed at `0x142215C35` with
  `0xc0000005` (write to `0x0`), in the mount-probe / init phase
  (`init42`, `init43`, `boot57-arena`), before any DStorage Submit or
  slot 86 call. This is the same early crash as boot #171 (also 236
  lines, same address, same write-to-0x0). It is a flake, not a
  regression from either fix — none of the new code executes that early.
- Dump: `F:\ScarlettCompat\out\scarlett_crash.dmp`
  (5,056,139,186 bytes), archived to
  `D:\scarlett-dumps\scarlett_crash_boot178.dmp`.
  (Note: the archive copy was slow / the F: original lingered; F: was
  ~3.4 GB free during the copy. The dump-vanishing fix — archive
  immediately post-boot — is still owed.)
- Boot #179 is the retry that will actually exercise slot 86 + the
  hash-gate resolver.

## Build

- Build #178: `BUILD_EXIT=0`, winrt 9/9, B1 149/149, verify 0.
  DLL `ScarlettCompat.dll` 372,736 bytes. No new warnings from the
  entry-33 code (the `fopen` in the resolver uses `fopen_s`; the
  remaining C4996 warnings are pre-existing `getenv` / test-file
  `fopen`).

## Status

- Slot 86: implemented, builds, not yet boot-verified (pending #179).
- Hash gate: resolver implemented, proven offline for
  `0xb76e17e552` -> data.rpf, not yet boot-verified (pending #179).
- Progress stays ~44% proven toward first menu (no frame yet).
