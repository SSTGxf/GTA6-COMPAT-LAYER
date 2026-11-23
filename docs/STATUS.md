# Status

Per-function implementation status (Xenia-style tags). Updated 2026-11-23 —
phase 3 done (milestones F, G, H, I). The source of truth is
`src/hle_registry.cpp`; this file is the human-readable mirror.

## Loader

| Item | Status | Notes |
|---|---|---|
| PE32+ parse + validation | implemented | MZ, machine x64, PE32+, executable flag |
| `.xbld` validation + GXDK strings | implemented | Edition + full product build parsed |
| Section mapping | implemented | Headers + sections; per-section protections applied after imports (phase 2) |
| Base relocations (`DIR64`) | implemented | Only when preferred base is busy |
| Import resolution — Xbox DLLs | implemented | Via HLE registry; never `LoadLibrary`'d |
| Import resolution — api-ms-win-*/system | implemented | Real `LoadLibrary` + `GetProcAddress` |
| Ordinal imports | implemented | Synthetic ordinal tables per Xbox DLL (phase 2); real ordinals need the exe |
| TLS directory | implemented | Slot + block + template + callbacks (phase 2); worker threads via CreateThread IAT hook (phase 3) |
| `xgameruntime.dll` PATH collision | handled | Loader never searches PATH for shim modules |
| Package catalog (`Catalog000.bin`) | not implemented | Verify-vs-skip decision needs the exe |

## HLE exports

| Module | Function | Status | Notes |
|---|---|---|---|
| `xgameruntime.dll` | `XGameRuntimeInitialize` | stub | First GDK call; returns S_OK |
| `xmem.dll` | `XMemAlloc` | implemented | Real: process heap |
| `xmem.dll` | `XMemFree` | implemented | Real: process heap |
| `d3d12_xs.dll` | `D3D12XboxCreateDevice` | implemented | Real passthrough to desktop `D3D12CreateDevice` (phase 2). Signature is D3D12CreateDevice-shaped (assumed); real Xbox form takes `D3D12XBOX_CREATE_DEVICE_PARAMETERS` — confirm vs exe |
| `d3d12_xs.dll` | Device-method surface (queues, allocators, lists, resources, heaps, root signatures, fences, feature queries) | implemented | Passthrough via the real desktop device behind the shim (phase 3, milestone G): guest `gfx_xs_surface` proves DIRECT/COMPUTE/COPY queues, committed + placed resources, CBV heap, root signature, and a GPU copy round-trip through the shim device |
| `xgameruntime.dll` | `XAsyncGetStatus` / `XAsyncGetResultSize` / `XAsyncCancel` / `XAsyncRun` | implemented | Synchronous completion model (phase 2, milestone E): the provider completes the block inline and invokes the callback before returning |
| `xgameruntime.dll` | `XUserAddAsync` / `XUserAddResult` / `XUserGetId` / `XUserGetGamertag` / `XUserGetLocalId` / `XUserCloseHandle` | implemented | Simplified user model (phase 2, milestone E): single synthetic default user ("Player", Xuid 0x9000000000001); interactive sign-in rejected with E_INVALIDARG |
| `xgameruntime.dll` | `XGameSaveInitializeProvider` / `XGameSaveCloseProvider` / `XGameSaveCreateContainer` | implemented | Local-only provider (phase 2, milestone E): root `%LOCALAPPDATA%\ScarlettCompat\XGameSave\<SCID>\`; containers are subfolders; no cloud sync |
| `xgameruntime.dll` | `XGameSaveCreateUpdate` / `XGameSaveSubmitBlobWrite` / `XGameSaveSubmitBlobDelete` / `XGameSaveSubmitUpdate` / `XGameSaveSubmitUpdateAsync` / `XGameSaveSubmitUpdateResult` / `XGameSaveCloseUpdate` / `XGameSaveCloseContainer` / `XGameSaveReadBlobData` | implemented | Staged update flow (phase 3, milestone F): writes/deletes staged in memory, applied to the container folder on submit (per-blob atomic via temp+rename); 16 MB cap per blob/update; async completes inline |
| `xgameruntime.dll` | `XGameSaveFilesGetFolderWithUiAsync` / `XGameSaveFilesGetFolderWithUiResult` | implemented | File-style saves (phase 3, milestone F): returns the local provider folder `%LOCALAPPDATA%\ScarlettCompat\XGameSaveFiles\<SCID>\`; no UI headless; titles use Win32 FileIO under it. Deviation: the real GDK errors if a title mixes XGameSave and XGameSaveFiles — this layer keeps their roots separate, so both work locally |
| `xgameruntime.dll` | `XPackageGetMountPath` / `XPackageGetMountPathSize` / `XPackageIsPackagedProcess` | implemented | Mount path = guest directory (phase 2, milestone E, set by the loader); presents the guest as a packaged Game Core title |
| `xgameruntime.dll` | `XPackageGetCurrentProcessPackageIdentifier` | implemented | Returns the synthetic identifier `ScarlettCompat-Guest-7952987A` (phase 3, milestone F). Signature corrected to the real (bufferSize, char*) form — phase 2 had it wrong as (void*) |
| `xgameruntime.dll` | `XPackageEnumeratePackages` | implemented | Enumerates the single synthetic package (phase 3, milestone F); Content kind returns 0 packages |
| `xgameruntime.dll` | `XPackageMountWithUiAsync` / `XPackageMountWithUiResult` / `XPackageCloseMountHandle` | implemented | Mounts the current package; no UI headless; unknown identifiers rejected with E_INVALIDARG (phase 3, milestone F) |
| `d3d12_xs.dll` | `SetFrameIntervalX` | implemented | Real pacing (phase 2): stores the interval |
| `d3d12_xs.dll` | `ScheduleFrameEventX` | implemented | Real pacing (phase 2): stamps the frame |
| `d3d12_xs.dll` | `WaitFrameEventX` | implemented | Real pacing (phase 2, tightened phase 3): waitable timer + QPC spin tail; `timeBeginPeriod(1)` — measured 16.7 ms vs 16.7 ms target (was 27.7 ms) |
| `xgameruntime.dll` | `XGameRuntimeInitialize` | stub | Ordinal 1 (synthetic) — real Xbox ordinal must be confirmed from the exe |
| `xmem.dll` | `XMemAlloc` / `XMemFree` | implemented | Ordinal 1 / 2 (synthetic) — real Xbox ordinals must be confirmed from the exe |
| `d3d12_xs.dll` | `D3D12XboxCreateDevice` | implemented | Ordinal 1 (synthetic) — real Xbox ordinal must be confirmed from the exe |
| `XAudio2_9.dll` | `XAudio2Create` | stub | Ordinal 1 (synthetic) |
| `gameinput.dll` | `GameInputCreate` | stub | Ordinal 1 (synthetic) |
| `dstorage_xs.dll` | `DStorageGetFactory` | stub | Ordinal 1 (synthetic) |
| `XAudio2_9.dll` | `XAudio2Create` | stub | XMA2 decode 0% everywhere |
| `gameinput.dll` | `GameInputCreate` | stub | — |
| `dstorage_xs.dll` | `DStorageGetFactory` | stub | No queues/files yet |

## Areas tracked but not yet in the registry

XSAPI (profile/achievements/sessions — stub/fail-soft only, live is 0%),
XGameUI / TCUI, XDisplay, XSystem, XStore, Game Chat, AcpHal / XMA,
DirectStorage queues + BCPack, shader lifting (DXIL route), texture
de-tiling (`XBOX_TILEMODE_SCARLETT`), unified memory (`MEM_GRAPHICS`).
XGameSave / XGameSaveFiles / XPackage are in the registry now (phases 2–3);
`XGameSaveCreateUpdate`-family, `XGameSaveFilesGetFolderWithUi*`, and
`XPackageEnumeratePackages` / `XPackageMountWithUi*` are implemented.

## Verification results

| Check | Result | When |
|---|---|---|
| Synthetic guest boots through loader | **PASS** — 12/12 subsystems (loader + 11 guest incl. new `frame_xs`); real desktop D3D12 device + queue + clear + fence OK on RX 5700 XT at FL 11_0; child exit 0; `scarlett_verify` verdict ALL PASS | 2026-11-22 ~22:05 EST (phase 2, milestone A) |
| Ordinal imports resolve | **PASS** — `guest_ordinal` (imports `XGameRuntimeInitialize` #1, `XMemAlloc` #1, `XMemFree` #2 by ordinal): loader logs `ordinal xgameruntime.dll:#1 -> XGameRuntimeInitialize` etc., 57 imports resolved 0 failed, child exit 0, verdict ALL PASS. Ordinals are **synthetic** (layer-assigned, .def order); real Xbox ordinals must be confirmed from the exe. Harness fix: `load_report.txt` is now written next to the guest, not next to the harness exe | 2026-11-22 ~22:40 EST (phase 2, milestone B) |
| Per-section page protections | **PASS** — loader logs `protect .text -> 0x20`, `.rdata -> 0x02`, `.data -> 0x04`, `.pdata/.xbld/.reloc -> 0x02`; guest VirtualQuery confirms `text=0x20 rdata=0x2 data=0x4`; both guests still ALL PASS (imports/relocations write before protections are applied) | 2026-11-22 ~22:50 EST (phase 2, milestone C) |
| TLS handling | **PASS** — loader allocates a TLS slot + block, copies the template, runs the `.CRT$XLB` callback (`DLL_PROCESS_ATTACH`); guest `__declspec(thread)` var reads template value 42 and writes back 123, `tls PASS callback=1 init=42 set=123`. Gotcha found + fixed: `TlsSetValue` alone did NOT update the TEB TLS array slot the compiler's TLS access reads (`gs:[58h]`) — the loader must also write the block directly into `TEB->TLS[index]` | 2026-11-22 ~23:15 EST (phase 2, milestone D) |
| XUser / XAsync / XGameSave / XPackage (boot-critical) | **PASS** — guest `services` subsystem: `user xuid=0x9000000000001 tag=Player local=1 mount=22 chars packaged=1`. XUserAddAsync (silent) → callback → XAsyncGetStatus → XUserAddResult → GetId/GetGamertag/GetLocalId/CloseHandle; XAsyncRun + GetResultSize (42); XGameSaveInitializeProvider + CreateContainer + CloseProvider (local folder); XPackageGetMountPath(Size) + IsPackagedProcess | 2026-11-22 ~23:45 EST (phase 2, milestone E) |
| First `d3d12_xs` frame on screen | **PASS (rendered + pixel-verified)** — 640x360 frame rendered via the shim device, center pixel (51,102,204,255) verified via readback. Swapchain Present is **environment-blocked** in this session: DXGI returns 0x887a0022 and a standalone probe program fails identically, so the present path is best-effort until the session is interactive | 2026-11-22 ~22:05 EST (phase 2, milestone A) |
| Swapchain Present (interactive session) | **PASS** — one-shot interactive scheduled task (session 1, explorer running) ran the standalone probe: `swapchain created`, `Present hr=0x00000000`, `PRESENT OK`. The same task then ran the full harness: guest `frame_xs` reports `swapchain present ok`, verdict ALL PASS. The 0x887a0022 block is purely the SSH session having no compositor — not a layer bug | 2026-11-23 ~00:10 EST (phase 3, milestone I) |
| XGameSave update flow + XGameSaveFiles + XPackage details | **PASS** — guest `services2` subsystem: gamesave round-trip ok (8-byte sync write + 4-byte async write, read-back verified), files folder `C:\Users\seths\AppData\Local\ScarlettCompat\XGameSaveFiles\00000000-0000-0000-0000-000000000003` with Win32 file write/read round-trip, pkg `ScarlettCompat-Guest-7952987A` enum=1 mount ok | 2026-11-23 ~00:00 EST (phase 3, milestone F) |
| `d3d12_xs` device-method surface | **PASS** — guest `gfx_xs_surface`: `queues=3 buffers=3 copy=1 rootSig=1 heap+placed=1 bindingTier=3` through the shim device (DIRECT/COMPUTE/COPY queues, committed + placed resources, CBV heap, empty root signature, GPU copy round-trip verified byte-for-byte) | 2026-11-23 ~00:00 EST (phase 3, milestone G) |
| Frame pacing tightened | **PASS** — `frame_wait` measured **16.7 ms** vs 16.7 ms target (was 27.7 ms). Cause: at the default 15.6 ms clock tick a waitable timer fires up to a full tick late; fix is `timeBeginPeriod(1)` + a QPC spin tail for the last ~1.5 ms | 2026-11-23 ~00:02 EST (phase 3, milestone H) |
| TLS on worker threads | **PASS** — guest `tls_thread`: `worker saw=77 set=88 main=77->77`. The loader hooks the guest's own `CreateThread`/`CreateRemoteThread` imports; the trampoline sets up a per-thread TLS block (template copy + `DLL_THREAD_ATTACH` callbacks) before the start routine runs. Threads created any other way (CRT internals etc.) still do not get guest TLS | 2026-11-23 ~00:02 EST (phase 3, milestone H) |
| `.xbld` detection | **PASS** — loader logs `.xbld present: edition='GXDK,October 2026 GXDK' build='GXDK,10.0.26100.9605'` for both guests (volatile-read keep + `/OPT:NOREF`) | 2026-11-22 ~22:40 EST (phase 2, milestone B) |
| Small real Game Core title boots | not attempted | — |
| GTA VI exe import table | blocked — no exe | — |

## Blocked on the exe

Exact import list; GDK/GDKX version; boot call order + first fatal
dependency; shader inventory used at boot; `MEM_GRAPHICS` usage pattern;
storage access pattern (DirectStorage queues, BCPack/GDeflate); threading
model; runtime integrity checks (XCI / package-hash validation).
