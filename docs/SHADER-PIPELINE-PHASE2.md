# Shader pipeline — Phase 2 handoff (DXIL backend → synthetic triangle)

Status: **Phase 2 is built and verified on the real GPU.** The DXIL backend compiles, validates and
caches shaders, and the emitted DXIL creates an actual D3D12 pipeline state that rasterizes a
triangle whose pixels were read back and checked. Nothing is wired into the boot path, nothing was
built into the layer, no boot was run, and nothing was committed or pushed.

- Tree: `D:\ScarlettCompat-shader` only. `F:\ScarlettCompat` was not read or touched.
- Phase 0 (capture) is unchanged in behaviour and still green: **124 checks, 0 failures**.

---

## 1. New files (Phase 2)

| File | Purpose |
|---|---|
| `include/shader/sc_dxil_backend.h` / `src/shader/sc_dxil_backend.cpp` | Device-aware DXIL backend: profile from the real shader model, compile + validate + content cache, smoke-shader sources |
| `include/shader/sc_gpu_render_test.h` / `src/shader/sc_gpu_render_test.cpp` | Caps probe and a real offscreen D3D12 triangle with pixel readback (D3D12/DXGI, no layer, no present) |
| `tests/shader/sc_triangle_main.cpp` | Phase 2 runner: caps → backend → PSO → render → pixel checks → capture dump |
| `tests/shader/build_triangle_test.bat` | Builds/runs the runner only (same no-vcvars environment setup as Phase 0) |
| `docs/SHADER-PIPELINE-PHASE2.md` | This document |

`src/shader/sc_container.cpp` and `tests/shader/sc_test_main.cpp` also changed — see **finding 2**,
which corrects a Phase 0 mistake.

---

## 2. Measured evidence (this machine, this run)

```
adapter="AMD Radeon RX 5700 XT" vendor=0x1002 device=0x731f FL=0xc100 SM=6.7 wave=32..64 mesh_tier=0 rt_tier=0
emitter: dxc-cli  (Windows SDK 10.0.26100.0, dxcompiler 1.9.2602.17)
model  : SM 6.7 -> profiles vs_6_7 / ps_6_7
VS: 3224 bytes  chunks="SFI0,ISG1,OSG1,PSV0,STAT,HASH,DXIL"  sha256=1d9ba3a8...
PS: 2528 bytes  chunks="SFI0,ISG1,OSG1,PSV0,STAT,HASH,DXIL"  sha256=7d1c9df2...
cache: 2 entries, 1 hit (same source compiled twice -> one dxc invocation)
pso_created=1 drew=1
center=(0,255,0,255)  corner=(0,0,0,255)   triangle visible=1, corner untouched=1
compute [WaveSize(64)]: accepted
vertex  [WaveSize(64)]: rejected  (dxc: "attribute WaveSize only allowed on compute or node shaders")
pixel shader observed wave lanes = 64
```

`35 checks, 0 failed.` Phase 0 recheck: `124 checks, 0 failed.`

---

## 3. Findings that change decisions

### Finding 1 — the target-hardware assumptions are confirmed *and* sharpened

Confirmed on hardware: FL 12_1, **SM 6.7** available (so `*_6_7` DXIL is the right target), mesh
shader tier **not supported**, raytracing tier **not supported**.

Sharpened: the device reports wave lanes **32..64**, not "64 only". RDNA1 can run wave32, so an
unannotated shader is not guaranteed 64-wide by the API. Measurement of what the driver actually
chooses (below) matters more than the API range.

### Finding 2 — stage detection was wrong in Phase 0; corrected (supersedes Phase 0 §5)

Phase 0 claimed the stage lives in PSV0 dword 1. Measured against real dxc 1.9 output:

| stage | PSV0 dword 1 | DXIL token byte 2 |
|---|---|---|
| vertex | 1 | 1 |
| pixel | 0 | 0 |
| compute | **0** | **5** |
| geometry | **3** | **2** |

PSV0 dword 1 is not a stage field — it merely coincided for vertex and pixel, which is exactly the
kind of false confirmation a two-sample check produces (and it made a compute shader parse as
`pixel`). Two more facts fell out: **every stage, compute included, ships ISG1/OSG1**, so signature
presence proves nothing either.

The authoritative source is the **DXIL chunk version token** (first dword of the DXIL chunk
payload): bits 0–15 = shader model (`0x67` = 6.7), bits 16–23 = stage, bits 24–31 reserved.

> **Correction (Phase 3):** the stage byte is `DXIL::ShaderKind`, not `D3D12_SHADER_VERSION_TYPE`.
> They agree for stages 0–5 only; a real mesh container reports **13** and amplification **14**
> (6–12 are shader-library/ray-tracing kinds). The reader was fixed in Phase 3 — see
> `SHADER-PIPELINE-PHASE3.md` §4, finding 1. The reader now uses that, keeps the SM5 `SHEX/SHDR` token path, and uses
neither PSV0 nor signature heuristics. The `psv0` / `io_signature` values of `sc_stage_source` were
removed in the later review pass — `sc_stage_source` is now `none` / `shader_token`, since nothing
could ever produce the others, and the PSV0/signature flags went with them.

**Action for Freebuff:** the copy of the shader subsystem in `F:\ScarlettCompat` predates this fix.
`include/shader/sc_container.h`, `src/shader/sc_container.cpp` and `tests/shader/sc_test_main.cpp`
there are stale, and the four Phase 2 source files + runner + build script are missing. Re-copy the
whole `include/shader/`, `src/shader/` and `tests/shader/` set; the change is self-contained and no
other file is affected. Until then, any container stage that came from the F: copy should be treated
as unverified for compute/geometry/hull/domain.

### Finding 3 — wave size: pinnable for compute, driver-chosen for graphics (measured = 64)

- `[WaveSize(64)]` **is accepted** at SM 6.7 for compute (and node) shaders. Console compute shaders
  that assume wave64 can be pinned deterministically.
- `[WaveSize(64)]` is **rejected by dxc for vertex/pixel** (and the other graphics stages):
  *"attribute WaveSize only allowed on compute or node shaders."* There is no way to pin the wave
  size of a graphics shader through HLSL or DXIL.
- What the driver actually picks for graphics here was measured, not assumed: a pixel shader that
  encodes `WaveGetLaneCount()` into the red channel rendered `64` on the RX 5700 XT.

Consequence for the RDNA2 frontend: lifted **graphics** shaders cannot be forced to wave64, but on
this driver they *run* at 64. Two things are therefore required rather than optional:
1. the frontend must not rely on the attribute for VS/PS/GS/HS/DS — emit it only for compute/node;
2. wave64-assuming graphics code should be validated per boot (this measurement is the harness for
   that), because the API does not guarantee the lane count. No emulation is involved either way —
   wave64 is native; it is simply not always the compiler's choice.

### Finding 4 — the backend refuses mismatched shaders instead of passing them on

`compile()` re-parses the emitted container and fails if the container's stage disagrees with the
requested stage, or if the emitted container has no DXIL chunk. This is what caught the compute
misdetection above; it stays as a guard against a wrong-profile compile slipping into a PSO.

### Finding 5 — a minimal D3D12 path works on this machine

The harness created a device, a root signature, a PSO, a 64×64 target, a readback buffer, executed a
draw and mapped the result with no device removal. That does not explain boot #89's
`DXGI_ERROR_DEVICE_REMOVED` on `CreatePlacedResourceX`, but it does show the driver is not failing
everything — consistent with the blocker being specific to the placed-resource/queue path rather
than "D3D12 is broken here". Offered as a datapoint only; the boot path is Freebuff's.

---

## 4. Interface for the boot path (when slots 86/16 land)

```cpp
#include "shader/sc_dxil_backend.h"
#include "shader/sc_gpu_render_test.h"   // only if you want the caps probe

// once, at device creation
const sc_gpu_caps caps = sc_query_gpu_caps();          // adapter, FL, SM, wave, tiers
const sc_shader_model model{caps.shader_model_major, caps.shader_model_minor};
auto backend = sc_dxil_backend::create(model);          // dxc if present, noop otherwise

// per shader (cache keyed on profile+entry+source, so repeats are free)
sc_backend_shader vs;
if (!backend->compile(sc_stage::vertex, hlsl, "main", vs)) {
    // vs.log explains it: unsupported stage, dxc failure, or container mismatch
}
// vs.dxil is what CreateGraphicsPipelineState expects: a DXBC envelope holding
// a DXIL chunk, verified on hardware in this phase.
```

Notes:
- `sc_stage_profile()` returns an empty string when a stage is unreachable at the device's model
  (mesh before 6.5). Treat empty as "skip/fallback", never as "try anyway".
- The backend needs no device, so it can be constructed before `D3D12CreateDevice` as long as caps
  are known; it holds no GPU objects.
- Per-shader compiler flags go in `sc_dxil_request::extra_args` (the backend passes them through).
  `[WaveSize(n)]` belongs in the HLSL, and only for compute/node.

---

## 5. Verification performed (and what was not)

```
tests\shader\build_triangle_test.bat     -> 35 checks, 0 failed, exit 0
tests\shader\build_shader_tests.bat      -> 124 checks, 0 failed, exit 0   (Phase 0 regression)
```

Covered by the Phase 2 runner: device caps; profile selection tied to the device's SM; VS+PS compile
with container/stage validation; cache hit on identical source; PSO creation; offscreen draw; pixel
readback proving the triangle covers the center with exactly `(0,255,0,255)` while the corner stays
at the clear color; compute `[WaveSize(64)]` accepted; graphics `[WaveSize(64)]` rejected (recorded);
measured graphics wave size 64; and the same PSO flowing through the Phase 0 capture path into a
corpus dump (`triangle_corpus`, 5 files, 8285 bytes, signature `4a15d68441c45449`).

Not verified:
- **No console shader has been seen yet.** There is still no corpus, so whether the guest ships DXBC
  SM5, DXIL SM6, or RDNA2 XS2 microcode remains unknown. Everything above is our own HLSL on our own
  hardware.
- Wave size was measured for a pixel shader only (vertex/geometry/hull/domain not separately probed;
  they share the driver's graphics path).
- No mesh shaders, no raytracing, no compute dispatch through a PSO (compute compilation only).
- Nothing here proves a *lifted* shader renders; only that our emitted DXIL does.

---

## 6. Next: the RDNA2 frontend (not started)

Per `docs/ARCHITECTURE.md`, the route is **carve DXBC → unpack native RDNA2 (XS2) → LLVM
disassembly → DXIL**, on the DXIL route (not SPIR-V). The frontend therefore has to answer, in
order, with corpus in hand:

1. **What the guest actually ships.** Phase 0's `chunks` field per stage will show whether containers
   carry a DXIL chunk (SM6, ready to reuse) or something else (XS2 payload). This is the first thing
   to look at once the hook is in — no point writing an XS2 unpacker for shaders that never appear.
2. **Carving.** Locate the container inside RPF-served streams and Xbox resource headers
   (`sc_locate_container` already finds a `DXBC` envelope at an offset, bounded and checked).
3. **Unpacking the XS2 payload** into a decodable instruction stream, with wave64 register/`scc`
   semantics preserved (no emulation — measure, per finding 3).
4. **Lifting** to DXIL through the existing emitter seam, with `sc_backend_shader` validation as the
   gate: a shader that fails container/stage validation never reaches a PSO.

Open question for Freebuff: the first corpus dump will settle whether step 3 is needed at all. If
the guest hands us DXIL SM 6.x containers, the frontend is mostly a classifier and the backend can
reuse them as-is (hash → PSO cache), and mesh work stays compute-to-draw as the architecture says.
