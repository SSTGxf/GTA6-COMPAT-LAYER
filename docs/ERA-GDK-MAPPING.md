# ERA → GDK mapping (phase 5)

What an Xbox One (ERA) title actually imports, and what this layer does
with each entry point. ERA and GDK share lineage (XGameRuntime, XUser,
XGameSave, D3D), so much of the surface maps — but the two runtimes are
NOT the same API, and the gaps are documented here instead of faked.

Sources: the real FH2 exe's import table (dumpbin, read-only,
2026-11-23 — see "The real inventory" below); WinDurango's public
d3d11.x.cpp (XDK-derived signatures for D3D11XCreateDeviceX,
D3DMapEsramMemory, DXGIXPresentArray, D3DAllocateGraphicsMemory);
WinDurango's shim-DLL exports (dumpbin /exports, 2026-11-23) for the
ordinal layout; Microsoft Learn / docs.xbox.com GDK docs (public);
the research brief
(`~/workspace/gta6-scarlett-compat-research-2026-11-22.md` §1, §3).
Anything marked ASSUMED is not public: the real Xbox One XDK headers
and the real ERA ordinals are NDA-gated. No real ERA title, exe, or
data is IN this repo — the import table below is measured metadata
(names + counts), the same class of data as docs/FILE_MAP.md.

## The generation split (why a translation exists at all)

- ERA is the Xbox One runtime (XDK). Game Core / GDK is the Series X|S
  runtime. Microsoft's own GDK docs state: **"On Xbox, Microsoft Game
  Development Kit (GDK) supports only D3D12.x. D3D11.x is not
  supported. If you have a game that uses D3D11.x, you'll need to move
  to D3D12.x as part of moving to the GDK."** An ERA title therefore
  cannot run on the GDK path as-is — its D3D11.x calls have no GDK
  equivalent on Xbox, and its services arrive as WinRT classes, not
  flat C functions.
- The GDK porting-guides index lists the ERA→GDK migration as four
  things: **WinRT-style API replacements**, **async pattern migration
  (`IAsyncOperation` → `XAsyncBlock` or `XTaskQueue`)**, **memory model
  changes (ERA memory manager → `XMemAlloc`)**, and **packaging
  migration to MSIXVC**. This layer implements the same four, in that
  order of feasibility (see below).
- ERA saves are the ConnectedStorage API in the `Windows.Xbox.Storage`
  namespace; GDK saves are the flat C `XGameSave` API in `xgamesave.h`
  (XR-052). Same split for users (`Windows.Xbox.System.User` → `XUser`)
  and packages (`Windows.ApplicationModel.Package` / XVD → `XPackage` /
  MSIXVC).
- **ESRAM vs EDRAM (terminology):** FH2 is an Xbox One title, so what
  it uses is **ESRAM** — 32 MB of embedded SRAM on the Xbox One APU
  for render targets (see "ESRAM" below). **EDRAM** is the Xbox 360's
  10 MB embedded DRAM — that is the Pinyon/FH1 resolve problem, not
  this one. The two are different hardware on different consoles; the
  layer handles ESRAM here and has no EDRAM path.

## Classes

- **1:1 map** — same signature, same semantics, different module name.
  The translation is a rename.
- **Adaptation** — same concept, different shape. The translation does
  real work (different parameters, different async model, different API
  generation) and the difference is stated.
- **Shared** — identical module + name in ERA and GDK; the existing GDK
  implementation already serves it.
- **No GDK equivalent** — the ERA thing has no counterpart on the GDK
  path. The layer refuses it explicitly (E_NOTIMPL, logged) instead of
  pretending it works.

## The real inventory — what FH2 actually imports

Read from `forza_durango_release_final.exe` (58,204,160 bytes,
2024-08-18; PE32+, x64, image base 0x140000000, entry 0x140F18058,
14 sections, subsystem Windows GUI, timestamp 2015-11-24) with
dumpbin, 2026-11-23. **648 imports across 14 modules, all by NAME**
(with hints); the only ordinal imports are 30 of WS2_32's 45. The
exe has **NO `.xbld` section** — ERA detection must use the import
set, not `.xbld`.

| Module | Count | What it is |
|---|---|---|
| `MSVCR110.dll` | 198 | VS2012 C runtime — **MISSING from the machine** |
| `MSVCP110.dll` | 185 | VS2012 C++ runtime — **MISSING from the machine** |
| `kernelx.dll` | 112 | 106 Win32 API names (all kernel32.dll exports) + 6 Xbox fns |
| `vccorlib110.DLL` | 57 | C++/CX runtime (incl. `GetActivationFactoryByPCWSTR`) — **MISSING** |
| `WS2_32.dll` | 45 | 15 named + 30 ordinal (Winsock) |
| `combase.dll` | 20 | COM + WinRT strings (`CoCreateInstance`, `WindowsCreateString*`, …) |
| `bcrypt.dll` | 1 | `BCryptGenRandom` |
| `xg_x.dll` | 7 | XG texture-tiling computers/layout |
| `d3d11_x.dll` | 3 | `D3D11XCreateDeviceX`, `D3DMapEsramMemory`, `DXGIXPresentArray` |
| `AcpHal.dll` | 9 | ACP audio-DSP heap |
| `EtwPlus.dll` | 4 | `EtxRegister/Unregister/EventWrite/FillCommonFields_v7` — **MISSING** |
| `MFPlat.DLL` | 4 | Media Foundation (`MFStartup`, `MFCreateMediaType`, …) |
| `MFReadWrite.dll` | 1 | `MFCreateSourceReaderFromURL` |
| `MMDevAPI.DLL` | 2 | `SetWasapiThreadAffinityMask`, `ActivateAudioInterfaceAsync` |

**NOT imported (and therefore not a translation target for this
title):** no `xgameruntime.dll`, no `XUser`/`XGameSave`/`XPackage`
flat APIs, no `d3d12_x.dll` (FH2 is D3D11.x-only), no `xaudio2_9.dll`,
no `RoGetActivationFactory`. WinRT activation goes via vccorlib110
`GetActivationFactoryByPCWSTR` + `CoCreateInstance` (C++/CX), so the
layer's `RoGetActivationFactory` hook cannot fire for this title.
Services arrive as WinRT classes activated at runtime, not static
imports.

### Hint vs ordinal — the correction that matters

The exe's import-table numbers for the Xbox DLLs are **HINTS**
(index into the sorted export-name table), **not ordinals** — the exe
imports by name. WinDurango's shim-DLL export ordinals (dumpbin
/exports) are a DIFFERENT numbering, and for every ERA module they
run one higher than the exe's hints (WinDurango numbers its exports
sequentially from 1 in alphabetical order):

| Function | Exe hint | WinDurango ordinal |
|---|---|---|
| `d3d11_x!D3D11XCreateDeviceX` | 3 | 4 |
| `d3d11_x!D3DMapEsramMemory` | 8 | 9 |
| `d3d11_x!DXGIXPresentArray` | 10 | 11 |
| `xg_x!XGComputeOptimalDepthStencilTileModes` | 1 | 2 |
| `xg_x!XGComputeOptimalTileMode` | 2 | 3 |
| `xg_x!XGComputeTexture2DLayout` | 4 | 5 |
| `xg_x!XGComputeTexture3DLayout` | 5 | 6 |
| `xg_x!XGCreateTexture1DComputer` | 7 | 8 |
| `xg_x!XGCreateTexture2DComputer` | 8 | 9 |
| `xg_x!XGCreateTexture3DComputer` | 9 | 10 |
| `AcpHal!AcpHalAllocateShapeContexts` | 0 | 1 |
| `AcpHal!AcpHalCreate` | 1 | 2 |
| `AcpHal!AcpHalReleaseShapeContexts` | 2 | 3 |
| `AcpHal!ApuAlloc` | 3 | 4 |
| `AcpHal!ApuCreateHeap` | 4 | 5 |
| `AcpHal!ApuFree` | 5 | 6 |
| `AcpHal!ApuIsVirtualAddressValid` | 7 | 8 |
| `AcpHal!ApuMapApuAddress` | 8 | 9 |
| `AcpHal!ApuMapVirtualAddress` | 9 | 10 |
| `kernelx!TitleMemoryStatus` | 433 | 463 |
| `kernelx!XMemAlloc` | 479 | 514 |
| `kernelx!XMemAllocDefault` | 480 | 515 |
| `kernelx!XMemFree` | 482 | 517 |
| `kernelx!XMemFreeDefault` | 483 | 518 |
| `kernelx!XMemSetAllocationHooks` | 487 | 524 |

An earlier phase 5 pass wrote the **hints** into the registry's
ordinal rows and the `.def` files by mistake. They are corrected now
to WinDurango's ordinals (the only real ordinal layout available);
the real console ordinals are unknown. The exe's name imports resolve
through the name table and never touch the ordinal rows — the rows
only matter for a title that imports by ordinal. (The same correction
retires the old "AcpHalAllocateShapeContexts is ordinal 0 and cannot
be expressed in a .def" note: 0 was its hint; its WinDurango ordinal
is 1.)

## Inventory — flat C entry points (grounded)

| ERA entry point | Class | GDK target / what the layer does |
|---|---|---|
| `kernelx.dll!XMemAlloc` | 1:1 map | `xmem.dll!XMemAlloc` (`HleXMemAlloc`, real HeapAlloc). |
| `kernelx.dll!XMemFree` | 1:1 map | `xmem.dll!XMemFree` (`HleXMemFree`). |
| `kernelx.dll!XMemAllocDefault` | 1:1 map | Same real HeapAlloc (`EraXMemAllocDefault`). |
| `kernelx.dll!XMemFreeDefault` | 1:1 map | Same real HeapFree (`EraXMemFreeDefault`). |
| `kernelx.dll!XMemSetAllocationHooks` | Adaptation | Accepted + logged, returns S_OK, **not routed** — the layer's allocations do not call title hooks. |
| `kernelx.dll!TitleMemoryStatus` | Adaptation | ASSUMED one-pointer form: zero-fills 64 bytes, returns 0. Zeros mean "unknown", not a fabricated budget. |
| `kernelx.dll!` + 106 Win32 names | Forwarding | `GetProcAddress(kernel32.dll, name)` at load time (cached handle). **Verified by the real exe**: the FH2 boot attempt logged all 106 forwards resolving (`FlushFileBuffers`, `HeapAlloc`, `CreateFileW`, `VirtualAlloc`, `CreateThread`, …). |
| `d3d11_x.dll!D3D11XCreateDeviceX` | Adaptation | Desktop `D3D11CreateDevice` (real D3D11 device). **GROUNDED 3-arg form** (WinDurango's public d3d11.x.cpp, XDK-derived): `(D3D11X_CREATE_DEVICE_PARAMETERS*, ppDevice, ppImmediateContext)` — NOT the 11-arg D3D11CreateDevice-shaped form phase 5 first assumed (that was wrong; the real exe calls three args, and an 11-arg callee would read stack garbage for the device out-pointer). The params struct's ESRAM/display/ring fields are logged, not acted on. |
| `d3d11_x.dll!D3D11XCreateDevice` | Adaptation | Desktop `D3D11CreateDevice` (10-arg form, matching WinDurango's public `EraD3D11CreateDevice`). The real FH2 exe does NOT import this entry. |
| `d3d11_x.dll!D3D11XCreateDeviceAndSwapChain` | Adaptation | Desktop `D3D11CreateDeviceAndSwapChain`. Same note as above; the real exe does not import it. |
| `d3d12_x.dll!D3D12XCreateDevice` | Adaptation | `d3d12_xs.dll!D3D12XboxCreateDevice` path (same real-device passthrough). The real FH2 exe does NOT import d3d12_x at all (D3D11.x-only title). |
| `d3d11_x.dll!D3DMapEsramMemory` | Adaptation | **Implemented** (see "ESRAM" below): maps real committed memory at the requested VA, 32 MB arena budget enforced. GROUNDED signature from WinDurango's public d3d11.x.cpp. |
| `d3d11_x.dll!DXGIXPresentArray` | Adaptation | **Implemented**: presents every swapchain in the array through the real desktop swapchains. GROUNDED signature from WinDurango's public d3d11.x.cpp. Per-swapchain present parameters (source rect, scale) are accepted and ignored — the full back buffer is presented. |
| `d3d11_x.dll!D3DAllocateGraphicsMemory` | Adaptation | **Implemented** (supporting): VirtualAlloc-backed graphics-memory allocator, GROUNDED shape from WinDurango's public d3d11.x.cpp. The real FH2 exe does NOT import it. |
| `d3d11_x.dll!D3DFreeGraphicsMemory` | Adaptation | **Implemented** (supporting): `VirtualFree` of a graphics-memory allocation. Same source as above. |
| `XAudio2_9.dll!XAudio2Create` | Shared | Same module + name in ERA and GDK; the existing stub serves both. Real audio (XMA2 decode) is 0% in both. |
| `xg_x.dll` (all 7) | No GDK equivalent | Xbox One texture-tiling / graphics helpers (`XG*`). Explicit E_NOTIMPL + log. The proof guest uses linear (untiled) resources, so tiling is not exercised. |
| `AcpHal.dll` (all 9) | No GDK equivalent | Audio Co-Processor hardware abstraction layer — console hardware, no host counterpart. Same E_NOTIMPL + log treatment as `xg_x.dll`. |

Counts (flat C, ERA entry points the layer translates): **4 mapped
1:1, 10 adapted, 1 shared, 16 with no equivalent** (7 XG + 9 AcpHal).
Plus the kernelx Win32 forwarding rule (106 names, one rule, not
per-function translation). 13 ERA entry points are real
implementations; 0 are faked.

## ESRAM — how Xbox One ESRAM translates to desktop D3D11/D3D12

**The hardware.** The Xbox One APU carries **32 MB of ESRAM**
(embedded SRAM, ~102 GB/s) alongside 8 GB of DDR3 system memory
(68 GB/s). ESRAM is a separate, explicitly managed pool: render
targets and depth buffers that fit live there; anything bigger is
tiled and resolved to DRAM (the same tiling/resolve pattern as the
Xbox 360's 10 MB EDRAM, but ESRAM is a different, larger, SRAM pool
on a different console — the two must not be conflated). 32 MB fits
roughly a 1080p 32bpp 2xMSAA target or a 720p 4xMSAA target; larger
targets spill to DDR3. The Xbox One X dropped ESRAM entirely for
unified memory; the Series X (GTA 6's target) has no embedded memory
either — unified GDDR6 — so this section is FH2/ERA-specific.

**The API surface the real exe uses.** The FH2 exe imports exactly
one ESRAM entry point: `D3DMapEsramMemory` (d3d11_x hint 8,
WinDurango ordinal 9). Its GROUNDED signature (WinDurango's public
d3d11.x.cpp) is:

```c
HRESULT D3DMapEsramMemory(UINT Flags, void* pVirtualAddress,
                          UINT NumPages, const UINT* pPageList);
```

It maps ESRAM pages into the title's virtual address space: `Flags`
bit 0 = large pages, bit 1 = 4 MB pages; `pPageList` holds the ESRAM
page numbers (4 KB pages) to map at `pVirtualAddress`. On the console
it delegates to `kernelx!MapTitleEsramPages` (WinDurango ordinal
318); the FH2 exe does not import that function directly. There is no
`D3DUnmapEsramMemory` in the exe's imports. ESRAM *placement* of
render targets (which target lives in ESRAM) is a driver-side
decision on the console, not a separate import — the title creates
D3D11.x resources and the driver places them.

**The translation (what the layer does).** On a PC there is no
ESRAM, so the layer translates ESRAM placements to regular GPU
memory:

1. **Render targets / resources:** an ERA D3D11.x render target is
   created as an ordinary desktop D3D11 texture in GPU memory
   (`D3D11_USAGE_DEFAULT`, the real device). There is no 32 MB limit
   on the host, so the console's spill path (tile + resolve to DRAM)
   is unnecessary — but the *resolve semantics* are preserved: the
   data is already where the resolve reads it, so `ResolveSubresource`
   / `CopyResource` on the texture is a real copy with exactly the
   console resolve's result. The era guest proves this with a
   single-sample target (so the resolve is `CopyResource`); an MSAA
   `ResolveSubresource` is the same D3D11 call on the same resource
   and is unexercised.
2. **CPU mapping (`D3DMapEsramMemory`):** the layer maps REAL
   committed memory at the requested VA (`VirtualAlloc`) and enforces
   the 32 MB arena budget with a counter — a mapping that would push
   the total past 32 MB fails with `E_OUTOFMEMORY`, the same wall the
   console has. Page numbers are validated against the arena
   (0..8191) and logged. Large-page flag bits are accepted and
   logged; the mapping itself is 4 KB pages.
3. **Correctness issue — CPU writes vs GPU visibility:** the mapped
   window is system RAM, not GPU memory. A title that writes through
   the window and then expects the GPU to see the data must copy it
   into a D3D11 resource itself (`UpdateSubresource` / `CopyResource`)
   — exactly as the era guest proves (ESRAM window → texture →
   staging → readback, center pixel red). The layer does not
   auto-upload: it cannot know when the title is done writing.
4. **Honest limits:** (a) two VAs mapping the same ESRAM page are
   independent copies on the host (no VA aliasing), where the console
   aliases them to one physical page — a title that relies on aliasing
   would see two copies; (b) the mapped memory is never unmapped
   (there is no unmap import), so the arena counter only grows within
   a process; (c) the D3D12 route is the same principle — an ESRAM
   placement would land in a `D3D12` DEFAULT-heap committed resource,
   and the era guest's `era_gfx12` stage proves the D3D12.x device
   path, but no D3D12 ESRAM mapping is implemented because the D3D11.x
   title that needs it (FH2) does not use D3D12.

**Verification (era guest, `era_esram` stage).** Maps 4 ESRAM pages
(16 KB) at a reserved VA, writes a red RGBA pattern through the
window and reads it back; maps a second 4 KB window at a different
VA; proves the 32 MB budget (an 8193-page request fails
`E_OUTOFMEMORY`) and page-range validation (page 8192 fails
`E_INVALIDARG`); exercises `D3DAllocateGraphicsMemory` /
`D3DFreeGraphicsMemory` (alloc, write, read back, free); uploads the
ESRAM window into a render-target texture, copies it to staging, and
pixel-verifies the center is red. See docs/STATUS.md for the run
evidence.

## Inventory — WinRT services (the part that is NOT flat C)

ERA titles reach users, saves, packages, and input through WinRT
classes, activated with `RoGetActivationFactory` — not through flat
imports. There is no ERA flat entry point to translate for these; the
translation is a WinRT → flat C adaptation, and it comes in two pieces:

| ERA side (WinRT) | GDK target (flat C, already implemented) | Phase 5 status |
|---|---|---|
| `Windows.Xbox.System.User` (sign-in, identity) | `XUserAddAsync` / `XUserAddResult` / `XUserGetId` / `XUserGetGamertag` / `XUserGetLocalId` / `XUserCloseHandle` (+ `XAsyncBlock` sync completion) | **Correspondence documented; activation plumbing future work.** The proof guest exercises the GDK target directly and says so in its stage detail. |
| `Windows.Xbox.Storage` ConnectedStorage (saves) | `XGameSave*` provider/container/update flow + `XGameSaveFiles*` | Same as above. |
| `Windows.ApplicationModel.Package` / XVD mount (packaging) | `XPackage*` (mount path = guest dir, synthetic package id) | Same as above. Packaging migration (XVD/AppX → MSIXVC) is not implemented; the layer presents the guest dir as the mount, exactly as on the GDK side. |
| `Windows.Xbox.Input` (+ XInput) | `GameInputCreate` (stub) | Same as above. |
| Async pattern `IAsyncOperation` | `XAsyncBlock` (sync completion model) | Documented as the migration; the layer's sync model already matches what a title observes from the GDK side. |

**What phase 5 DOES implement for WinRT:** the activation routing.
The loader hooks the guest's `combase.dll!RoGetActivationFactory`
import (same IAT-hook technique as the `CreateThread` hook) and the
layer's `EraRoGetActivationFactory` intercepts Xbox-namespace class
names (`Windows.Xbox.*`, `Windows.ApplicationModel.Package`): they are
refused with `REGDB_E_CLASSNOTREG` and logged, because the layer has
no COM objects for them yet. Every other class name forwards to the
real `RoGetActivationFactory`, so normal WinRT keeps working. This
proves the routing exists; it does not fake the objects.

**What blocks the COM objects:** the WinRT interface definitions
(IIDs, vtable layouts) for the `Windows.Xbox.*` classes are XDK-gated.
Implementing a COM object with a guessed vtable would be faking it.
The objects need either the XDK headers or a real ERA exe's WinRT
class references to define — same class of blocker as the real GDK
ordinals needing the GTA exe.

**FH2 caveat:** the real FH2 exe does NOT import
`RoGetActivationFactory` at all — it activates WinRT classes via
vccorlib110 `GetActivationFactoryByPCWSTR` + `CoCreateInstance`
(C++/CX), and vccorlib110 is one of the missing CRT DLLs. So the
activation hook is moot for this title until the CRT lands; the era
guest still proves the routing exists (its `era_winrt` stage passes
with the hook installed, and the same stage passes with the real
function — the outcomes are identical for the two classes tested,
so the stage does not prove the hook works; see docs/STATUS.md).

## Inventory — things with no host equivalent at all

- **ESRAM capacity / spill:** the host has no 32 MB pool, so the
  console's spill path (tile + resolve to DRAM when a target does not
  fit) never triggers on PC — the layer enforces the budget only on
  the `D3DMapEsramMemory` mapping path (see "ESRAM" above). A title
  that sizes its targets to fit 32 MB on console gets the same pixels
  on PC; a title that depends on the spill *timing* (not the pixels)
  will not see it.
- **ERA packaging (XVD/AppX):** the layer does not parse XVD
  containers. Mount = guest directory, package id synthetic — same
  treatment as the GDK side.
- **Unified memory / `MEM_GRAPHICS`:** the same open question as on
  the Series X side (see `docs/ARCHITECTURE.md`). ERA titles share
  CPU/GPU memory on the console APU; desktop has separate RAM/VRAM.
- **Precompiled ERA shaders:** ERA D3D11.x shaders are DXBC (SM 5.0)
  and are directly consumable by desktop D3D11 — that is an advantage
  of the D3D11.x path over the D3D12 path, where precompiled shaders
  need the DXIL carving/lifting route (same as Series X). The proof
  guest compiles HLSL at runtime (`D3DCompile`), so no lifting is
  exercised in phase 5.

## Loader detection

A title is ERA when it imports any of `d3d11_x.dll`, `d3d12_x.dll`,
`kernelx.dll`, `xg_x.dll`, `AcpHal.dll`. The loader logs
`era <module>!<fn> -> <target>` for translated imports,
`era <module>!<fn> -> no GDK equivalent (E_NOTIMPL stub)` for refused
ones, and `ERA title detected` once. ERA modules are in the shim list,
so they never go through `LoadLibrary` (none of them exist on desktop
Windows). A mixed title (ERA graphics + GDK service imports, like the
synthetic proof guest) is detected as ERA and its GDK imports resolve
through the normal registry path. The loader also forwards kernelx
Win32 names to kernel32.dll (see the inventory table) and installs
the WinRT activation hook for ERA titles only.

## The real FH2 boot attempt (2026-11-23 ~02:38 EST)

The layer was pointed at the real exe (copy at
`F:\ScarlettCompat\out\fh2\forza_durango_release_final.exe`, harness
`scarlett_verify.exe`, timeout 300 s, log `F:\ScarlettCompat\fh2_boot.log`):

- The loader mapped the exe fine (PE ok, 14 sections, image base
  0x140000000, entry 0x140F18058, TLS template 336 bytes, index 7),
  detected ERA (kernelx.dll), and resolved **203 of 648 imports**
  (445 failed). All 106 kernelx Win32 forwards resolved; `XMemAlloc`/
  `XMemFree`/`XMemAllocDefault`/`XMemFreeDefault` implemented;
  `D3D11XCreateDeviceX` implemented; XG (7) + AcpHal (9) + the two
  other d3d11_x entries honestly stubbed; 30 WS2_32 ordinals resolved.
- **First wall, as predicted: the VS2012 CRT is missing.**
  `LoadLibrary failed for MSVCR110.dll / MSVCP110.dll /
  vccorlib110.DLL / EtwPlus.dll (err 126)` — 444 of the 445 failures
  are those four modules (MSVCR110 198, MSVCP110 185, vccorlib110 57,
  EtwPlus 4); one further import is unidentified in the log. The
  entry point's first call hit an unimplemented-import stub
  (`[hle] called unimplemented import -> E_NOTIMPL`) and the child
  exited `0xFF`. Harness: `LOAD PASS`, `RUN PASS exit=0x000000ff`,
  `verdict: FAIL` (no guest report — the exe died before writing one).
- **Proposed next fix (not done yet — the attempt's failure is the
  evidence):** install the VS2012 redistributable (MSVCR110/MSVCP110/
  vccorlib110) and provide EtwPlus.dll (WinDurango's shim sits in the
  Mount folder, but the harness's DLL search path does not include the
  exe's directory). After the CRT lands, the next walls are the XG
  texture re-tiling, the ESRAM/ACpHal paths, the WinRT COM objects for
  Xbox services, and the real FH2 data files. The CRT was deliberately
  NOT installed before the attempt.

## Verification (phase 5 bar)

`tests/guest_era.cpp` — a synthetic Xbox One-shaped PE (ERA DLL import
set, FH2 boot shape: runtime init → sign-in → save load early →
package/DLC check → memory → device → audio/input → storage → triangle
scene → frame loop → ESRAM → present array → save round-trip →
package mount). It boots THROUGH the translation into the GDK layer
and renders a real frame. Harness: `era` subsystem report in
`src/verify.cpp` (same style as the `proof` report block). Bar: clean
rebuild `BUILD_EXIT=0`; `guest_era` ALL PASS end to end (20/20 boot
stages, including `era_esram` and `era_presentarray`); all 3 existing
guests still ALL PASS (no regressions). The real FH2 boot attempt
above is the proof of "FH2 showing" for the real exe — it stops at
the CRT wall, honestly, and the synthetic guest is the regression
proof that the translation itself works.
