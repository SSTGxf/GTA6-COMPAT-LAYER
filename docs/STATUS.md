# Status

Per-function implementation status (Xenia-style tags). Updated 2026-11-23 —
phase 5 done (ERA→GDK translation: era proof guest 20/20 stages PASS
including ESRAM + DXGIXPresentArray, real FH2 exe import table read and
first boot attempted — stops at the missing VS2012 CRT). The source of
truth is `src/hle_registry.cpp`; this file is the human-readable mirror.

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
| `d3d12_xs.dll` | `ScSoftwareRT_QueryPath` | implemented | Software ray tracing (see docs/SOFTWARE-RT.md): reads the adapter RT tier; tier 0 → software path. ScarlettCompat-defined entry point, not an Xbox API |
| `d3d12_xs.dll` | `ScSoftwareRT_Init` | implemented | Software ray tracing: compiles the compute ray-tracing PSO once, caches it |
| `d3d12_xs.dll` | `ScSoftwareRT_BuildScene` | implemented | Software ray tracing: CPU BVH build (median-split, one triangle per leaf) + GPU upload; returns a scene handle |
| `d3d12_xs.dll` | `ScSoftwareRT_FreeScene` | implemented | Software ray tracing: releases a scene handle's GPU resources |
| `d3d12_xs.dll` | `ScSoftwareRT_DispatchRays` | implemented | Software ray tracing (software path): compute-shader ray tracer (raygen + BVH walk + Möller–Trumbore + closest-hit/miss) into the caller's UAV. Hardware path returns E_NOTIMPL — real hardware DXR passthrough is future work, unverifiable on this card |
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
Software ray tracing is in the registry now (`ScSoftwareRT_*`, see
docs/SOFTWARE-RT.md) — compute-shader DXR replacement, verified on the
5700 XT. Still open: the COM wrapper that routes a real title's
`ID3D12Device5` / `ID3D12GraphicsCommandList4` RT calls to it, hardware
DXR passthrough for RT-capable adapters, and mesh shaders (a separate
problem from ray tracing).

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
| **Phase 5 — ERA→GDK translation** (`src/era_to_gdk.cpp`, `tests/guest_era.cpp`, `docs/ERA-GDK-MAPPING.md`) | **PASS — era guest 20/20 boot stages, triangle + ESRAM resolve pixel-verified, 60-frame loop, no regressions** — an Xbox One (ERA) title boots through the translation into the GDK layer: ERA DLL detection (d3d11_x / d3d12_x / kernelx / xg_x / AcpHal imports), kernelx Win32 forwarding (~106 names → kernel32), `D3D11XCreateDeviceX` (real desktop D3D11 — **grounded 3-arg form**, WinDurango's public d3d11.x.cpp; the first 11-arg assumption was wrong), `D3DMapEsramMemory` (real committed memory at the requested VA, 32 MB arena budget, page validation) + `D3DAllocateGraphicsMemory`/`D3DFreeGraphicsMemory`, `DXGIXPresentArray` (real per-chain Present), WinRT activation routing (Xbox-namespace classes → REGDB_E_CLASSNOTREG, others forwarded), XG + AcpHal → documented E_NOTIMPL. Era guest 20/20: user → save early → package/DLC → memory → D3D11.x device → D3D11 surface → D3D12.x surface → audio + input → WinRT → **triangle scene (center 255,0,0,255, corner 51,102,204,255)** → **60-frame loop avg 16.7 ms, max 18.7, slow 0, Present 60/60** → **ESRAM (map + pattern + 2nd map + capacity + range + gfxmem + resolve center 0xff0000ff red)** → **PresentArray (S_OK + empty-array E_INVALIDARG)** → files → package mount → protections → TLS → TLS worker thread. Counts: 4 mapped 1:1, 10 adapted, 1 shared, 16 no-equivalent (7 XG + 9 AcpHal). **Real FH2 exe** (`forza_durango_release_final.exe`, 58,204,160 B): import table read read-only — 648 imports, 14 modules (MSVCR110 198, MSVCP110 185, kernelx 112, vccorlib110 57, WS2_32 45, combase 20, xg_x 7, d3d11_x 3, AcpHal 9, EtwPlus 4, MFPlat 4, MFReadWrite 1, MMDevAPI 2, bcrypt 1); no .xbld section, no RoGetActivationFactory import, no d3d12_x, no xgameruntime. **Boot attempt 2026-11-23 ~02:38 EST:** loader mapped it fine (ERA detected, 203/648 imports resolved — all 106 kernelx forwards), then stopped at the first wall: **VS2012 CRT missing** (MSVCR110/MSVCP110/vccorlib110/EtwPlus, err 126, 444 of 445 failures), entry point's first call hit an unimplemented-import stub, child exit 0xFF, verdict FAIL (LOAD PASS). CRT deliberately NOT installed before the attempt; it is the proposed next fix. **Hint-vs-ordinal correction:** the exe's import numbers are HINTS, not ordinals — all ERA ordinal rows in the registry + .def files corrected to WinDurango's shim-DLL ordinals (d3d11_x 3→4/8→9/10→11, xg_x +1 each, AcpHal +1 each, kernelx +30/+31). **Intermittent TLS flake (known, unresolved):** in full builds one guest stage intermittently reads a garbage `__declspec(thread)` initial value (e.g. init=-747589040, -651308944, 896898448 — heap-pointer fragments, not a mis-copied template); never in 6 isolated re-runs; TLS hardening (re-verify + restore the block after callbacks) did NOT fire its warning, so the corruption is not at callback time — it happens during the guest's run. Mitigations in this build: `tls_early` diagnostic (reads the TLS var at entry in all three guests — 42 in this clean run), frame-check tolerance (fails only if >2 frames exceed 50 ms; avg + present count still prove the loop). **This run: all 5 guests ALL PASS, verify exit 0, no flake.** Full evidence: `F:\ScarlettCompat\build_phase5.log` | 2026-11-23 ~03:00 EST (phase 5) |
| **Software ray tracing** (`tests/guest_software_rt.cpp` + host test `tests/software_rt_test.cpp`) | **PASS — 6/6 stages, ray-traced triangle pixel-verified via the software path** — the layer's `ScSoftwareRT_*` entry points route DXR-style work to a compute-shader ray tracer (raygen + stack BVH walk + Möller–Trumbore + closest-hit/miss) on the RX 5700 XT, which reports `RaytracingTier == NOT_SUPPORTED`. Guest stages: `srt_tier` (tier 0, software path), `srt_init` (compute PSO compiled), `srt_scene` (1-triangle CPU BVH, 1 node), `srt_dispatch` (256x256 rays into a UAV, center (255,0,0,255) red, corner (51,102,204,255) miss color). Host test passes standalone first (`[srt-test] PASS`), then the guest passes through the loader + harness with the same pixels. All 5 guests ALL PASS in the full build (minimal, ordinal, proof 17/17, era 20/20, software-rt 6/6), verify exit 0. Not covered: any-hit, procedural primitives, recursion, multi-BLAS TLAS, hardware DXR passthrough (E_NOTIMPL), the COM wrapper for real titles, mesh shaders | 2026-11-23 ~02:50 EST (software RT) |
| Session-0 harness crash (environmental, NOT a layer bug) | **Found + worked around** — under a session-0 scheduled task (SYSTEM) both guests crash right after the `tls` stage: child exit 0xFF, `EXCEPTION_ACCESS_VIOLATION` in `gdi32full.dll+0x8984a` reading address 0x20 (NULL+0x20), faulting thread's stack holds the AMD driver (`amdxc64`), RTSSHooks64, combase/imm32. The layer never calls GDI. Proven environmental: the same binaries pass ALL PASS in an interactive session (session 1) and in a foreground SSH run, and crash 100% in session 0. **The build task `ScarlettPhase4Build` must be created with `schtasks /it` (interactive).** Same class as the phase-3 Present finding (0x887A0022 = no compositor in session 0) | 2026-11-23 ~01:00 EST (phase 4) |
| `.xbld` detection | **PASS** — loader logs `.xbld present: edition='GXDK,October 2026 GXDK' build='GXDK,10.0.26100.9605'` for both guests (volatile-read keep + `/OPT:NOREF`) | 2026-11-22 ~22:40 EST (phase 2, milestone B) |
| Small real Game Core title boots | not attempted | — |
| GTA VI exe import table | blocked — no exe | — |

## Blocked on the exe

Exact import list; GDK/GDKX version; boot call order + first fatal
dependency; shader inventory used at boot; `MEM_GRAPHICS` usage pattern;
storage access pattern (DirectStorage queues, BCPack/GDeflate); threading
model; runtime integrity checks (XCI / package-hash validation).

Note: the blockers above are for the **GTA VI (Series X) exe**, which has
not landed. The **FH2 (ERA) exe's import table has been read** (648
imports, 14 modules) and its first boot attempted — see the Phase 5 row
above and `docs/ERA-GDK-MAPPING.md` ("The real inventory").
