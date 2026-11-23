# Status

Per-function implementation status (Xenia-style tags). Updated 2026-11-22 —
scaffolding day 1. The source of truth is `src/hle_registry.cpp`; this file
is the human-readable mirror.

## Loader

| Item | Status | Notes |
|---|---|---|
| PE32+ parse + validation | implemented | MZ, machine x64, PE32+, executable flag |
| `.xbld` validation + GXDK strings | implemented | Edition + full product build parsed |
| Section mapping | implemented | Headers + sections; RWX for now (per-section protections TODO) |
| Base relocations (`DIR64`) | implemented | Only when preferred base is busy |
| Import resolution — Xbox DLLs | implemented | Via HLE registry; never `LoadLibrary`'d |
| Import resolution — api-ms-win-*/system | implemented | Real `LoadLibrary` + `GetProcAddress` |
| Ordinal imports | not implemented | Logged; needs ordinal tables per Xbox DLL |
| TLS directory | not implemented | Detected + logged |
| `xgameruntime.dll` PATH collision | handled | Loader never searches PATH for shim modules |
| Package catalog (`Catalog000.bin`) | not implemented | Verify-vs-skip decision needs the exe |

## HLE exports

| Module | Function | Status | Notes |
|---|---|---|---|
| `xgameruntime.dll` | `XGameRuntimeInitialize` | stub | First GDK call; returns S_OK |
| `xmem.dll` | `XMemAlloc` | implemented | Real: process heap |
| `xmem.dll` | `XMemFree` | implemented | Real: process heap |
| `d3d12_xs.dll` | `D3D12XboxCreateDevice` | stub | Signature assumed D3D12CreateDevice-shaped; confirm vs exe. Real translation pending |
| `d3d12_xs.dll` | `SetFrameIntervalX` | stub | No real frame pacing yet |
| `d3d12_xs.dll` | `ScheduleFrameEventX` | stub | — |
| `d3d12_xs.dll` | `WaitFrameEventX` | stub | Real pacing needed for stability |
| `XAudio2_9.dll` | `XAudio2Create` | stub | XMA2 decode 0% everywhere |
| `gameinput.dll` | `GameInputCreate` | stub | — |
| `dstorage_xs.dll` | `DStorageGetFactory` | stub | No queues/files yet |
| `xgameruntime.dll` | `XUserAddAsync` | not implemented | User model; Simple User Model default |
| `xgameruntime.dll` | `XUserGetId` | not implemented | — |

## Areas tracked but not yet in the registry

XGameSave / XGameSaveFiles, XPackage (mount/chunks), XAsync / XTaskQueue,
XSAPI (profile/achievements/sessions — stub/fail-soft only, live is 0%),
XGameUI / TCUI, XDisplay, XSystem, XStore, Game Chat, AcpHal / XMA,
DirectStorage queues + BCPack, shader lifting (DXIL route), texture
de-tiling (`XBOX_TILEMODE_SCARLETT`), unified memory (`MEM_GRAPHICS`).

## Verification results

| Check | Result | When |
|---|---|---|
| Synthetic guest boots through loader | **PASS** — 12/12 subsystems (loader, runtime, memory, gfx_xs, present, frame_evt, frame_wait, audio, input, storage, gfx_real); real desktop D3D12 device + queue + clear + fence OK on RX 5700 XT at FL 11_0; child exit 0; `scarlett_verify` verdict ALL PASS | 2026-11-22 ~21:55 EST |
| First `d3d12_xs` frame on screen | not attempted (device call is still a stub; real translation pending) | — |
| Small real Game Core title boots | not attempted | — |
| GTA VI exe import table | blocked — no exe | — |

## Blocked on the exe

Exact import list; GDK/GDKX version; boot call order + first fatal
dependency; shader inventory used at boot; `MEM_GRAPHICS` usage pattern;
storage access pattern (DirectStorage queues, BCPack/GDeflate); threading
model; runtime integrity checks (XCI / package-hash validation).
