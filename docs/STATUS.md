# Status

Per-function implementation status (Xenia-style tags). Updated 2026-11-22 —
phase 2, milestone A done. The source of truth is `src/hle_registry.cpp`;
this file is the human-readable mirror.

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
| `d3d12_xs.dll` | `D3D12XboxCreateDevice` | implemented | Real passthrough to desktop `D3D12CreateDevice` (phase 2). Signature is D3D12CreateDevice-shaped (assumed); real Xbox form takes `D3D12XBOX_CREATE_DEVICE_PARAMETERS` — confirm vs exe |
| `d3d12_xs.dll` | `SetFrameIntervalX` | implemented | Real pacing (phase 2): stores the interval |
| `d3d12_xs.dll` | `ScheduleFrameEventX` | implemented | Real pacing (phase 2): stamps the frame |
| `d3d12_xs.dll` | `WaitFrameEventX` | implemented | Real pacing (phase 2): waitable-timer sleep to the frame deadline (~16.7 ms at interval 1) |
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
| Synthetic guest boots through loader | **PASS** — 12/12 subsystems (loader + 11 guest incl. new `frame_xs`); real desktop D3D12 device + queue + clear + fence OK on RX 5700 XT at FL 11_0; child exit 0; `scarlett_verify` verdict ALL PASS | 2026-11-22 ~22:05 EST (phase 2, milestone A) |
| First `d3d12_xs` frame on screen | **PASS (rendered + pixel-verified)** — 640x360 frame rendered via the shim device, center pixel (51,102,204,255) verified via readback. Swapchain Present is **environment-blocked** in this session: DXGI returns 0x887a0022 and a standalone probe program fails identically, so the present path is best-effort until the session is interactive | 2026-11-22 ~22:05 EST (phase 2, milestone A) |
| Small real Game Core title boots | not attempted | — |
| GTA VI exe import table | blocked — no exe | — |

## Blocked on the exe

Exact import list; GDK/GDKX version; boot call order + first fatal
dependency; shader inventory used at boot; `MEM_GRAPHICS` usage pattern;
storage access pattern (DirectStorage queues, BCPack/GDeflate); threading
model; runtime integrity checks (XCI / package-hash validation).
