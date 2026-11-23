# GTA 6 Series X compat layer — scaffolding notes (2026-11-22)

**Repo:** `SSTGxf/GTA6-COMPAT-LAYER` (public, main). Work folder on the PC:
`F:\ScarlettCompat`. **Do NOT push from the PC** (no git there) — the parent
agent pulls the tree and pushes. No game code in the repo; no exe fetching
or reconstruction; read-only on `D:\game\game`.

**Deadline:** 15 hours to verify a good stable layer (~12:46 EST Nov 23).
Realistic bar (research brief): loader + shimmed boot path for a synthetic
guest title and a first `d3d12_xs` frame — NOT GTA 6 playable.

## 1. File map — `D:\game\game` (measured, read-only)

GTA VI Xbox Series X build v1.0.2.1, TitleId `7952987A`, ~131 GB
(130,897,922,118 bytes, 41 files + `data\` + `pack\`). **No exe.**

- Identity: `RockstarGamesInc.28872443D20A7`, Publisher
  `CN=3164F9C2-2C28-4832-BBE3-A642A4EF0054`, MSAAppId `000000004060F900`,
  StoreId `9NL3WWNZLZZN`, protocol `ms-xbl-7952987a`.
- Exe name (from manifest + `MicrosoftGame.Config` + layout chunk 1001):
  `game_scarlett_master.exe`, AppId `GTAO`, EntryPoint
  `GamingProxyApp.Application.EntryPoint`, TargetDeviceFamily Scarlett,
  GameOS dep 10.0.18362.0, 2048 MB persistent local storage, SMT on,
  Lockhart/Anaconda memory `Advanced`.
- Biggest RPFs: `levels.rpf` 76.2 GB, `audio.rpf` 28.5 GB, `movies.rpf`
  8.8 GB, `navmeshes.rpf` 6.7 GB, `packs.rpf` 4.8 GB, `anim.rpf` 4.4 GB,
  `data.rpf` 481 MB, `shaders.rpf` 411 MB, `textures.rpf` 192 MB,
  `common.rpf` 6.6 MB, `script.rpf` 4.4 MB, `appdata0.rpf` 8,880 B
  (magic `9FPR` = RPF v9).
- `gameos.xvd` 348 MB — Game OS image, first bytes hash-like, opaque.
- `Catalog000.bin` 815 KB — MZ/PE-stub wrapper, single unnamed section
  covering the file (package catalog; loader must decide verify-vs-skip).
- `microsoft_gameconfig.bin` 8 KB — `regf` (registry hive) magic.
- `resources.pri` — `mrm_pri2`. Layout XML: 25 chunks (1000–1014,
  3000–3009, 3009 `Marker="Launch"`), 104 file groups. Chunk 1001 = the
  exe (withheld). Chunks 1004–1014 = one RPF each; 3000–3008 = `pack\`
  chunk RPFs + ttdb files.
- `data\ttdb\` — 46 `.ttpf` files (graphics, interiors, lighting, maps,
  peds, props, terrain, ui, vehicles, vfx, weapons). `pack\` —
  `core_bvh4`, `core_licenses`, `core_restofworld(+001)`,
  `shared_minigame000(+_bvh4)`, `story000(+_bvh4, +_restofworld)`.

## 2. Shipped DLLs — PE analysis (measured)

All PE32+ x86-64, base `0x180000000`, chars `0x2022`. Xbox-built DLLs
carry a `.xbld` section with GXDK/GRDK strings:

- `Bink2Scarlett.dll` — GXDK June 2022 Update 4, 10.0.22000.4429.
- `XboxBvhBuilder.dll` — GXDK April 2025 Update 5, 10.0.26100.4125.
- `amd_fidelityfx_upscaler_xbox.dll` — **GXDK October 2026**,
  10.0.26100.9605 (dates the build to late 2026).
- `XCurl.dll` — GRDK April 2026 Update 5, 10.0.26100.7901.
- Key imports: `d3d12_xs.dll` (XboxBvhBuilder, FSR upscaler),
  `xmem.dll` (Bink2, BVH, FSR), `XAudio2_9.dll` (Bink2),
  `WINHTTP.dll`/`CRYPT32.dll` (XCurl, libHttpClient.GDK),
  `KERNEL32.dll`/`ADVAPI32.dll` (libHttpClient.GDK), ~30 `api-ms-win-*`
  API sets (all DLLs — forward to real Windows on PC).
- No `xgameruntime`/`xgameplatform` imports in the shipped DLLs — those
  are linked by the exe. Its import list is open question #1.

## 3. Loader design (`src/pe_loader.cpp`)

Native x86-64 PE32+ → map straight into the host process (no CPU
emulation). Validate MZ/PE32+/x64, parse `.xbld` (GXDK edition + build),
`VirtualAlloc` at preferred base `0x180000000` (reloc `DIR64` on
fallback), copy headers/sections, resolve imports: Xbox DLLs → HLE
registry (never `LoadLibrary` — avoids the Win11 26100 System32
`xgameruntime.dll` collision, `0xC0000139`); api-ms-win-*/system →
real `LoadLibrary` + `GetProcAddress`. Missing imports → E_NOTIMPL stub
+ counted. TLS detected, not handled. Entry runs in a throwaway child
process (harness) so a guest crash can't kill the harness.

## 4. Service surface (`src/hle_registry.cpp`, Xenia-style tags)

Implemented/stubbed now: `XGameRuntimeInitialize` (stub, S_OK),
`XMemAlloc`/`XMemFree` (real heap), `D3D12XboxCreateDevice` (stub;
signature assumed D3D12CreateDevice-shaped — confirm vs exe),
`SetFrameIntervalX` / `ScheduleFrameEventX` / `WaitFrameEventX` (stubs,
no real pacing), `XAudio2Create` (stub), `GameInputCreate` (stub),
`DStorageGetFactory` (stub). Not implemented: `XUserAddAsync`,
`XUserGetId`. Tracked in docs/STATUS.md, not yet in the registry:
XGameSave, XPackage, XAsync/XTaskQueue, XSAPI (fail-soft only), XGameUI,
DirectStorage queues, AcpHal/XMA.

## 5. Graphics plan (DXIL route, not SPIR-V)

`d3d12_xs` → desktop D3D12. Device creation via `D3D12XboxCreateDevice`
→ `D3D12CreateDevice`. Core D3D12 surface (lists, fences, heaps,
barriers, PSOs, views, clears, copies, queries) is structurally
identical — thin passthrough. Tiling via DirectXTex Xbox tables
(`XBOX_TILEMODE_SCARLETT`). Shaders: DXBC carve → RDNA2 → LLVM → DXIL
(per Xbox2Windows; Leonida's SPIR-V path does not transfer to D3D12).
Mesh shaders (9,282) + DXR: desktop D3D12 supports both, but the RX 5700
XT (RDNA1) has neither — full pipeline unverifiable on the build
machine. **Unified memory (`MEM_GRAPHICS` aliasing) is the #1 open
design question — 0% in every public project.**

## 6. Verification harness (`src/verify.cpp` + `tests/guest_minimal.cpp`)

`scarlett_verify.exe <guest> [timeout_ms]` spawns itself as a child
(`--run`), loads the guest through the layer, runs it, reads
`load_report.txt` + `guest_report.txt`, prints PASS/FAIL per subsystem
(loader, runtime, memory, gfx_xs, present, frame_evt, frame_wait, audio,
input, storage, gfx_real). The synthetic guest is an Xbox-style PE (base
`0x180000000`, `.xbld` with Oct 2026 GXDK strings, Xbox imports) that
runs the boot sequence plus a real desktop D3D12 device + clear + fence
as the translation-target proof. Build + run: `build.bat` (VS 2022
BuildTools, MSVC 14.44, Win SDK 10.0.26100.0).

**Verified 2026-11-22 ~21:55 EST — verdict ALL PASS (exit 0):** loader
PASS (14 modules, 63 imports resolved, 0 failed — including ordinal
import `d3d12.dll:#101` = `D3D12CreateDevice`, fixed in the loader during
this session); guest subsystems runtime/memory/gfx_xs/present/frame_evt/
frame_wait/audio/input/storage all PASS; `gfx_real` PASS (desktop D3D12
device + queue + clear + fence OK on the RX 5700 XT at FL 11_0). The
`d3d12_xs` device call itself is still a stub — real translation is the
next step after this snapshot.

## 7. Blocked on the exe

Exact import list; GDK/GDKX version; boot call order + first fatal
dependency; shader inventory used at boot; `MEM_GRAPHICS` usage pattern;
storage pattern (DirectStorage queues, BCPack/GDeflate); threading model;
runtime integrity checks (XCI / package-hash validation). **Do not run
any binary claiming to be the exe from unofficial sources** — leak hype
is a documented malware vector; only the Cyberleek drop with matching
SHA-256 counts, sandboxed.

## 8. Research base

`~/workspace/gta6-scarlett-compat-research-2026-11-22.md` — GDK/Game Core
vs XDK/ERA split; ScarlettX as closest template (64.4%, boots one indie
Unity game); Leonida shader pipeline; Xbox2Windows DXIL route; RAGE as a
cross-platform abstraction layer (platform surface concentrated in RAGE's
platform layer — exactly what a compat layer replaces).
