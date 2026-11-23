# GTA6-COMPAT-LAYER (ScarlettCompat)

A compatibility layer for running Xbox Series X (Scarlett) Game Core titles on
Windows PC — built from scratch, because nothing existing covers Series X.

This repo contains **no game code, no game assets, and no exe** — only the
layer itself, its tests, and documentation of what was measured from the
packaging metadata.

---

## Why — why does this exist?

### The gap

Every public compatibility layer targets an older Xbox:

- **Azimuth** runs Xbox 360 titles on Xbox One (and is closed source).
- **Mezcal** runs Xbox One titles on PC.
- **WinDurango** shims the Xbox One ERA runtime.
- **Xenia** emulates the Xbox 360.

**None of them cover Xbox Series X.** Series X runs the **GDK / Game Core**
runtime, which replaced the Xbox One XDK/ERA runtime. The APIs a Series X
title calls at boot (`XGameRuntimeInitialize`, Game Core services,
`d3d12_xs`, DirectStorage) are a different platform surface from the Xbox One
one — the old layers' work does not transfer. A Series X title needs a new
layer, and nobody had built one for a AAA open-world title.

### Why now

The leaked GTA VI Xbox Series X build v1.0.2.1 (TitleId `7952987A`) is on
disk with all game data — 131 GB, all 25 layout chunks — but **no exe**
(it was deliberately withheld from the leak). The exe is expected to drop
separately (~Nov 23, 2026). The moment it lands, the only thing between the
game and a PC is a layer that can load it and answer its system calls. This
repo is that layer, built ahead of the exe so the work is ready to verify
the second it arrives.

### Why a compat layer instead of an emulator or a recomp

A Series X title is a **native x86-64 PE32+** running on a Windows-derived
OS. There is no CPU to emulate and no foreign architecture to recompile —
the game's machine code already runs on a PC CPU. The work is entirely in
the *platform*: the title expects Xbox system services, Xbox graphics
(`d3d12_xs`), Xbox storage and input, and Xbox memory behaviour. A compat
layer — load the PE, intercept its imports, and answer the Xbox calls with
host implementations — is the correct shape. That is what this repo does.
(For comparison: a static recomp makes sense for a foreign CPU, like
Switch ARM64 → x86-64. Here the CPU already matches, so a layer is the
shortest path.)

---

## What — what does it do?

The layer has five pieces. Each one is real code in this repo, not a plan.

### 1. Xbox PE loader (`src/pe_loader.{h,cpp}`)

Maps a native x86-64 PE32+ into the host process and prepares it to run:

- Parses and validates the PE (MZ header, x64 machine, PE32+, executable
  flag).
- Validates the **`.xbld` section** — the GXDK build stamp every Series X
  title carries — and parses the edition + full product build out of it.
  The measured game package's DLLs carry GXDK stamps (Bink2 Jun 2022, BVH
  Apr 2025, FSR **Oct 2026** 10.0.26100.9605, XCurl GRDK Apr 2026).
- Maps headers + sections, applies base relocations (`DIR64`) when the
  preferred image base is busy.
- **Resolves imports by interception, never by `LoadLibrary`:** Xbox DLL
  imports (`xgameruntime`, `xmem`, `d3d12_xs`, `XAudio2_9`, `gameinput`,
  `dstorage_xs`) resolve to the HLE registry in this repo; `api-ms-win-*`
  and system DLL imports resolve to real Windows. This matters because
  Windows 11 ships its own `C:\Windows\System32\xgameruntime.dll` — a
  guest launched as a child process fails `0xC0000139` if PATH finds the
  system DLL before the shim. The loader never searches PATH for shim
  modules, so that collision cannot happen.
- Ordinal imports are detected and logged (per-Xbox-DLL ordinal tables are
  still TODO — the loader proved it can resolve one: `d3d12.dll:#101`
  (`D3D12CreateDevice`) via `GetProcAddress(MAKEINTRESOURCE)`).

### 2. HLE registry (`src/hle_registry.{h,cpp}`)

The Xenia-style registry of Xbox exports, each tagged `implemented`,
`stub`, or `not-implemented`. The source of truth is
`src/hle_registry.cpp`; `docs/STATUS.md` is the human-readable mirror.

Implemented / stubbed now:

| Module | Function | State | What it does |
|---|---|---|---|
| `xgameruntime.dll` | `XGameRuntimeInitialize` | stub | First GDK call; returns S_OK |
| `xmem.dll` | `XMemAlloc` / `XMemFree` | implemented | Real process-heap backing |
| `d3d12_xs.dll` | `D3D12XboxCreateDevice` | implemented | Real passthrough to desktop `D3D12CreateDevice` |
| `d3d12_xs.dll` | `SetFrameIntervalX` / `ScheduleFrameEventX` / `WaitFrameEventX` | implemented | Real frame pacing (waitable timer) |
| `d3d12_xs.dll` | First `d3d12_xs` frame | implemented | 640x360 rendered via shim device, pixel verified via readback |
| `XAudio2_9.dll` | `XAudio2Create` | stub | XMA2 decode is 0% everywhere |
| `gameinput.dll` | `GameInputCreate` | stub | — |
| `dstorage_xs.dll` | `DStorageGetFactory` | stub | No queues/files yet |
| — | `XAsync` | implemented | Synchronous completion model |
| — | `XUser` | implemented | Silent default user ("Player") |
| — | `XGameSave` | implemented | Local-only provider |
| — | `XPackage` | implemented | Mount path = guest dir |

Tracked as not-implemented: `XGameSaveCreateUpdate` /
`XGameSaveSubmitBlobWrite` / `XGameSaveFiles` /
`XPackageEnumeratePackages`, plus whole areas not yet in the registry —
XSAPI (live Xbox Live — stub/fail-soft only), XGameUI / TCUI, XDisplay,
XSystem, XStore, Game Chat, AcpHal / XMA, DirectStorage queues + BCPack,
shader lifting, texture de-tiling (`XBOX_TILEMODE_SCARLETT`), unified
memory (`MEM_GRAPHICS`).

### 3. Shim DLLs (`src/scarlett.cpp`, `defs/*.def`)

`ScarlettCompat.dll` is the layer DLL, and `defs/` holds the import-library
definitions for the Xbox DLL names the guest imports
(`xgameruntime.def`, `xmem.def`, `d3d12_xs.def`, `XAudio2_9.def`,
`dstorage_xs.def`, `gameinput.def`). This is the ScarlettX shim-DLL model:
the guest links against Xbox-named DLLs, and the layer answers them.

### 4. Verification harness (`src/verify.cpp`)

`scarlett_verify.exe` runs a guest PE through the layer in a throwaway
child process — a guest crash must not kill the harness — and reports
**pass/fail per subsystem** (`loader`, `runtime`, `memory`, `gfx_xs`,
`present`, `frame_evt`, `frame_wait`, `audio`, `input`, `storage`,
`gfx_real`). Reports land next to the guest: `load_report.txt`,
`guest_report.txt`, `scarlett_verify_report.txt`.

### 5. Synthetic guest title (`tests/guest_minimal.cpp`)

An Xbox-style PE (image base `0x180000000`, `.xbld` section, Xbox imports)
that exercises the boot sequence. It exists because the real exe is not
here yet — the layer is verified against this guest until the exe lands.

### Graphics plan

`d3d12_xs` → desktop D3D12, following the **DXIL shader route**
(Xbox2Windows), not Leonida's SPIR-V/Vulkan route. Precompiled RDNA2 XS2
shaders get lifted to DXIL; Xbox texture tiling
(`XBOX_TILEMODE_SCARLETT`) gets de-tiled; frame pacing is real, not stubbed,
once the device translation lands. Full design:
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

---

## How — how does it work, end to end?

### Boot flow

1. The harness spawns a child process and hands it the guest PE path.
2. The loader maps the PE, validates `.xbld`, applies relocations.
3. For each import, the loader checks the DLL name: Xbox DLL → HLE
   registry entry (implemented function, stub, or logged
   not-implemented); system / `api-ms-win-*` DLL → real Windows
   `LoadLibrary` + `GetProcAddress`. Ordinal imports resolve through
   `GetProcAddress(MAKEINTRESOURCE)`.
4. The guest's entry point runs. Its first GDK call is
   `XGameRuntimeInitialize`, which the registry answers with S_OK.
   Memory calls (`XMemAlloc`) get real heap blocks. Graphics calls hit
   the `d3d12_xs` stubs for now; the real device translation is the next
   step.
5. The harness collects the per-subsystem report and prints the verdict.

### How verification works

```bat
build.bat
out\scarlett_verify.exe out\guest_minimal.exe [timeout_ms]
```

`build.bat` (VS 2022 BuildTools + Windows SDK 10.0.26100.0) builds
`ScarlettCompat.dll`, the shim import libs, the synthetic guest, and the
harness — then runs the harness against the guest and prints the
per-subsystem report.

**Verified result (2026-11-22 ~21:55 EST, clean rebuild, BUILD_EXIT=0,
harness verdict ALL PASS):** the synthetic guest boots through the loader
with **12/12 subsystems passing**, including `gfx_real` — a real desktop
D3D12 device + queue + clear + fence on the RX 5700 XT at FL 11_0 — and
the loader resolved **63 imports with 0 failed** across 14 modules. The
harness runs the guest in a child process, so a guest crash is a failed
subsystem, not a dead harness.

### How to add a new HLE function

1. Add the export to the registry in `src/hle_registry.cpp` with its tag
   (`implemented` / `stub` / `not-implemented`).
2. If it is implemented, write the host implementation (real heap, real
   D3D12 call, real file I/O) — stubs log and return plausible success.
3. Mirror it in `docs/STATUS.md`.
4. Add a guest-side call in `tests/guest_minimal.cpp` so the harness
   covers it, rebuild, and confirm the subsystem still passes.

---

## Status — 2026-11-22 (phase 2 complete: built to work, not just scaffold)

**What works now (verified on the build machine, both harness guests
ALL PASS):**

- Xbox PE32+ loader: maps a native x86-64 PE into the host process,
  validates the `.xbld` section (GXDK build strings), applies base
  relocations, and resolves imports — Xbox DLLs to the HLE registry,
  `api-ms-win-*` / system DLLs to real Windows. **Phase 2:** per-section
  page protections (`.text` XR, `.rdata` R, `.data` RW — guest-verified
  via VirtualQuery), TLS handling (slot + block + template copy +
  `.CRT$XLB` callbacks, `__declspec(thread)` var reads its template
  value), and an ordinal table (synthetic ordinals; `guest_ordinal`
  imports by ordinal and passes).
- HLE registry (Xenia-style tags): `XGameRuntimeInitialize`, `XMemAlloc` /
  `XMemFree` (real heap backing), **`D3D12XboxCreateDevice` — real
  passthrough to desktop `D3D12CreateDevice` (no longer a stub)**,
  **real frame pacing** (`SetFrameIntervalX` / `ScheduleFrameEventX` /
  `WaitFrameEventX`, waitable-timer), **first real `d3d12_xs` frame**
  (640x360 rendered via the shim device, center pixel verified via
  readback), `XAudio2Create`, `GameInputCreate`, `DStorageGetFactory`,
  plus **implemented boot-critical services: `XAsync` (sync completion),
  `XUser` (silent default user), `XGameSave` (local-only provider),
  `XPackage` (mount path = guest dir)**.
- Verification harness (`scarlett_verify.exe`): runs a guest PE through
  the layer in a throwaway child process and reports **pass/fail per
  subsystem**. Final clean build: `guest_minimal` ALL PASS (real device,
  frame, protections, TLS, services), `guest_ordinal` ALL PASS (57
  imports resolved, 0 failed).
- Swapchain Present is **environment-blocked** in the harness session
  (DXGI 0x887a0022; a standalone probe fails identically — no compositor
  in that session), so the frame is proven by render + readback pixel
  verification, with Present best-effort. Re-verify Present when the
  build machine has an interactive session.

**The honest bar for this work (15-hour window):** a loader + shimmed
boot path for a synthetic guest title and a first `d3d12_xs`
frame — **not GTA 6 playable.** No public project boots this game; the
realistic ladder is (1) synthetic guest boots, (2) first `d3d12_xs` frame
on screen, (3) one small real Game Core title boots, (4) only then point
the layer at the GTA VI exe once it lands and read its import table.
Steps 1 and 2 are done and verified (above).

**Blocked on the exe (nothing here can substitute):**

- The exact import list (which Xbox DLLs/functions the GTA VI exe imports).
- The GDK/GDKX version the title was built against.
- Boot call order, first fatal dependency, and whether it hard-aborts when
  Xbox Live / XSAPI is unreachable.
- Shader inventory actually used at boot vs the 26,503 in `shaders.rpf`.
- Memory usage pattern (`MEM_GRAPHICS` aliasing), storage access pattern,
  threading model, and any runtime integrity checks.

**Known hard limits (0% in every public project, including this one):**

- Unified memory (`MEM_GRAPHICS`, 64 KB / 4 MB physical-virtual aliasing)
  — the #1 open design question. No public project has solved it.
- Precompiled RDNA2 XS2 shaders — DXIL route, not implemented yet.
- Retail `.xvc` / `.xvd` decryption — the layer only works with
  already-readable (extracted) files.
- Live Xbox Live / XSAPI services — stubbed / fail-soft only.
- Hardware ray tracing / mesh shaders — desktop D3D12 supports both, but
  the build machine's RX 5700 XT (RDNA1) has neither, so the full GTA VI
  pipeline cannot be verified on it (9,282 mesh shaders + DXR in the game).

**Next steps:** `d3d12_xs` → D3D12 translation beyond device creation
(command lists, resources, shaders/DXIL); confirm the real ordinal
numbers and the true `D3D12XboxCreateDevice` signature from the exe's
import table the moment it lands; re-verify swapchain Present in an
interactive session.

## Layout

```
include/          scarlett.h (registry/logging), xbox_shims.h (guest prototypes)
src/              pe_loader.{h,cpp}   — Xbox PE loader
                  hle_registry.{h,cpp} — HLE export registry + stub implementations
                  scarlett.cpp        — layer DLL entry + exports
                  verify.cpp          — scarlett_verify harness
tests/            guest_minimal.cpp   — synthetic Xbox-style guest title
defs/             *.def               — shim import-lib definitions (Xbox DLL names)
docs/             FILE_MAP.md         — measured inventory of the game package
                  ARCHITECTURE.md     — loader / shim / graphics design
                  STATUS.md           — per-function implementation status
                  SCAFFOLDING-NOTES.md — day-1 working notes
```

## Safety / scope

- Read-only on the game package. This repo never modifies it.
- No exe is fetched, reconstructed, or downloaded by this project. The exe
  arrives on its own; until then the layer is verified against the synthetic
  guest only.
- No game code, assets, shaders, or RPF contents are in this repo — only
  packaging metadata (file names, sizes, manifest/config XML) measured
  during the file-map step.
- **Do not run the harness against any binary claiming to be the missing
  exe from unofficial sources** — the leak hype is a documented malware
  vector. Only the Cyberleek drop with matching SHA-256 counts, and even
  that must be treated as untrusted code in a sandbox.
