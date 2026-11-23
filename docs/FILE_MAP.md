# File map — GTA VI Xbox Series X build v1.0.2.1 (`D:\game\game`)

Measured 2026-11-22 by read-only inspection. **No game code or assets are
in this repo** — this is packaging metadata only (names, sizes, manifest /
config XML, PE headers of the shipped DLLs).

## Identity (from `appxmanifest.xml` + `MicrosoftGame.Config`)

| Field | Value |
|---|---|
| Title | Grand Theft Auto VI |
| Identity Name | `RockstarGamesInc.28872443D20A7` |
| Publisher | `CN=3164F9C2-2C28-4832-BBE3-A642A4EF0054` |
| Version | 1.0.2.1 |
| TitleId | `7952987A` |
| MSAAppId | `000000004060F900` |
| StoreId | `9NL3WWNZLZZN` |
| Executable | `game_scarlett_master.exe` (Application Id `GTAO`) |
| EntryPoint | `GamingProxyApp.Application.EntryPoint` |
| TargetDeviceFamily | Scarlett (`GameOs` dep 10.0.18362.0) |
| Protocol | `ms-xbl-7952987a` |
| PersistentLocalStorage | 2048 MB |
| VM | SMT enabled; Lockhart + Anaconda title memory `Advanced` |
| Capabilities | `internetClient`, microphone |
| Languages | 40 (en-US/GB/AU/CA/NZ/IE/HK/SG/IN/SA/ZA/AE/IL/CZ/HU/SK/GR, zh-HK/TW/SG, fr-FR/CA/BE/CH, de-DE/AT/CH, it-IT, ko-KR, pl-PL, pt-PT/BR, ru-RU, es-MX/ES/US/AR/CL/CO) |

## Top level — 41 files, 130,897,922,118 bytes (~131 GB)

| File | Size (bytes) | Role |
|---|---|---|
| `levels.rpf` | 76,155,065,056 | World data (largest archive) |
| `audio.rpf` | 28,543,454,672 | Audio |
| `movies.rpf` | 8,832,481,776 | Cutscenes / Bink video |
| `navmeshes.rpf` | 6,707,512,496 | Navigation meshes |
| `packs.rpf` | 4,798,213,264 | Pack archives |
| `anim.rpf` | 4,407,626,384 | Animation |
| `data.rpf` | 481,544,304 | Data |
| `shaders.rpf` | 411,695,600 | Shaders (DXBC containers, native RDNA2) |
| `textures.rpf` | 192,585,024 | Textures |
| `gameos.xvd` | 348,069,888 | Game OS image (XVD; first bytes are hash-like, opaque) |
| `common.rpf` | 6,642,192 | Common data |
| `script.rpf` | 4,358,960 | Scripts |
| `Catalog000.bin` | 815,104 | Package catalog — MZ/PE-stub wrapper, single unnamed section covering the file (hashes/signatures; loader must skip-or-verify) |
| `amd_fidelityfx_upscaler_xbox.dll` | 786,432 | FSR upscaler (GXDK Oct 2026) |
| `XboxBvhBuilder.dll` | 884,224 | BVH builder (imports `d3d12_xs.dll`) |
| `Bink2Scarlett.dll` | 255,488 | Bink video (imports `XAudio2_9.dll`, `xmem.dll`) |
| `libHttpClient.GDK.dll` | 975,856 | HTTP client (imports `KERNEL32.dll`, `WINHTTP.dll`, `CRYPT32.dll`) |
| `XCurl.dll` | 199,152 | cURL (imports `WINHTTP.dll`, `CRYPT32.dll`) |
| `amd_fidelityfx_loader_xbox.dll` | 13,312 | FSR loader |
| `appxmanifest.xml` | 3,394 | Package manifest |
| `MicrosoftGame.Config` | 2,542 | Game config (XML) |
| `microsoft_gameconfig.bin` | 8,192 | Compiled config — `regf` (registry hive) magic |
| `resources.pri` | 1,152 | PRI resource index (`mrm_pri2`) |
| `appdata0.rpf` | 8,880 | Small RPF, magic `9FPR` (RPF v9) |
| `layout_3202719e-b4b4-421c-9f8f-70d1c4a7d60b_xs.xml` | 13,698 | Chunk layout (25 chunks, 104 file groups) |
| `Update.alignmentchunk` | 1 | Alignment marker (chunk 3009, `Marker="Launch"`) |
| VC++ runtimes | — | `concrt140`, `msvcp140(+_1/_2/_atomic_wait/_codecvt_ids)`, `vccorlib140`, `vcomp140`, `vcruntime140(+_1)` — all PE32+ x64, base `0x180000000` |
| Logos/splash | — | `xbs_logo_*.png`, `xbs_splash.png` |

## Layout chunks (25 chunks, 104 file groups)

| Chunks | Contents |
|---|---|
| 1000 (8 files) | Manifest/config/logos/`resources.pri`/`appdata0.rpf` |
| **1001 (1 file)** | **`game_scarlett_master.exe` — WITHHELD, the missing exe** |
| 1002 (16 files) | All DLLs |
| 1003 (46 files) | `data\ttdb\*.ttpf` |
| 1004–1014 (1 RPF each) | `common`, `movies`, `shaders`, `anim`, `data`, `levels`, `navmeshes`, `packs`, `textures`, `audio`, `script` |
| 3000–3008 | `pack\` chunk RPFs + ttdb files (`story000`, `shared_minigame000`, `core_*`) |
| 3009 (1 file) | `Update.alignmentchunk`, `Marker="Launch"` |

## `data\` — TTDB files (46 in chunk 1003)

`data\ttdb\{graphics, interiors (0_3–3_3), lighting, maps (0_7–7_7),
peds, props, terrain (0_7–7_7 + image_based 0_1–1_1), ui, vehicles, vfx,
weapons}\*.ttpf` — terrain/database files.

## `pack\` — chunk RPFs

`core_bvh4`, `core_licenses`, `core_restofworld(+001)`,
`shared_minigame000(+_bvh4)`, `story000(+_bvh4, +_restofworld)` — each a
directory with an `.rpf` plus loose `data\ttdb\*.ttpf` files.

## Shipped DLLs — PE analysis (measured)

All are standard **PE32+ x86-64** (machine `0x8664`), image base
`0x180000000`, characteristics `0x2022`, subsystem 2 (GUI) or 3 (CUI).
Xbox-built DLLs carry a **`.xbld` section** (512 B, 4096 B for XCurl) with
GXDK/GRDK build strings:

| DLL | GXDK stamp (from `.xbld`) |
|---|---|
| `Bink2Scarlett.dll` | GXDK, June 2022 Update 4, 10.0.22000.4429 |
| `XboxBvhBuilder.dll` | GXDK, April 2025 Update 5, 10.0.26100.4125 |
| `amd_fidelityfx_upscaler_xbox.dll` | GXDK, October 2026, 10.0.26100.9605 |
| `XCurl.dll` | GRDK, April 2026 Update 5, 10.0.26100.7901 |

Key imports (measured from the import tables):

| Import | Imported by | Why it matters |
|---|---|---|
| `d3d12_xs.dll` (1 fn each) | `XboxBvhBuilder.dll`, `amd_fidelityfx_upscaler_xbox.dll` | The Xbox D3D12 variant — the graphics translation surface |
| `xmem.dll` | Bink2Scarlett, XboxBvhBuilder, FSR upscaler | Xbox extended memory API |
| `XAudio2_9.dll` | Bink2Scarlett | Xbox audio |
| `WINHTTP.dll` / `CRYPT32.dll` | XCurl, libHttpClient.GDK | Network (XSAPI transport) |
| `KERNEL32.dll` / `ADVAPI32.dll` | libHttpClient.GDK | Desktop-named kernel APIs (GameOS provides them) |
| `api-ms-win-*` (~30 API sets) | all DLLs | Standard UWP/GDK API-set surface — forwards to real Windows on PC |

No `xgameruntime` / `xgameplatform` imports appear in the shipped DLLs —
those are linked by the **exe**, which is the missing piece. The exe's
import list is open question #1 (see README).

## What the loader must find at boot

1. `game_scarlett_master.exe` — chunk 1001, from `..\..\content\` in the
   layout; named in `MicrosoftGame.Config` `ExecutableList` and in the
   manifest (`Executable="game_scarlett_master.exe"`, Id `GTAO`).
2. The DLLs in chunk 1002 (same directory as the exe).
3. `data\ttdb\*.ttpf` + the RPF archives (chunk 1003–1014, 3000–3008).
4. `MicrosoftGame.Config` / `microsoft_gameconfig.bin` (TitleId, storage
   size, VM settings) and `Catalog000.bin` (package catalog — the loader
   must decide verify-vs-skip; XCI signatures cannot be validated without
   keys).
5. `gameos.xvd` — the Game OS image; opaque to the layer (not parsed).
