# Boot #179 Diagnosis — amdxc64.dll crash (AMD driver), slot 86 + hash gate untested

## Summary

Boot #179 (2026-11-25 ~02:20 EST) tested the entry-33 build (hash-gate resolver
`ScDsResolvePack` + Codex slot 86 `CreatePipelineState`, DLL 373,760 bytes,
BUILD_EXIT=0). **It crashed early at 2,018 lines** — before either fix ran.
This is a NEW crash, not the hash gate and not the #171/#178 flake.

## The crash

- **Exception:** 0xc0000005 (read of 0x0) at **0x7FFBA0D6FFD0**.
- **Fault module:** **amdxc64.dll +0x52ffd0** (the AMD GPU shader compiler).
- **Params:** [0]=0x0, [1]=0x0 (null-pointer read).
- **Thread:** 34108.
- **Last layer events before the crash:**
  - `Slot74 RegisterPagePoolX call#1 -> S_OK`
  - `CreateCommandQueue -> hr=0x00000000` (success)
  - Then the AMD driver crashed.

## What did NOT run (proves neither fix caused it)

- **Slot 86 calls: 0.** `CreatePipelineState` was never invoked. The game
  crashed during early device/queue setup, before PSO creation.
- **ResolvePack: 0.** The hash resolver never ran. No DStorage file-sourced
  request reached Submit.
- **Unresolvable hash: 0.** The hash gate (0x141DAC734) was never reached.
- **CBV (copied): 0.** The game never got to constant-buffer setup.

## Interpretation

The crash is in AMD's shader compiler (amdxc64.dll), not in the game
(0x14...) and not in the layer. The game created a command queue
successfully, then something invoked the AMD driver, which dereferenced a
null pointer. This is either:

1. A flaky driver crash (early-init timing, as with the #171/#178 flake at
   0x142215C35), or
2. The game submitting a shader/PSO very early that the AMD driver
   cannot handle on this adapter/driver (32.0.21045.5002).

Neither the slot 86 handler nor the hash resolver executed, so neither
caused it. The fixes are untested — a clean boot is still needed.

## Dump

- `F:\ScarlettCompat\out\scarlett_crash.dmp` (6,365,687,765 bytes) —
  archived to `D:\scarlett-dumps\scarlett_crash_boot179.dmp`.
- F: was at ~2.2 GB free after the dump; the F: original must be deleted
  after the archive verifies.

## Next

- Re-run as boot #180 (same DLL, no code change) to see if the amdxc64
  crash is a flake. If #180 gets past early init, the slot 86 + hash-gate
  fixes get their first real test.
- If the amdxc64 crash repeats, investigate what the game submits to the
  driver during early init (shader compile at device/queue creation).
