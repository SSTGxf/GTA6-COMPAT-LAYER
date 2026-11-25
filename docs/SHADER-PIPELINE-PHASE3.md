# Shader pipeline — Phase 3 handoff (RDNA2 frontend: classification + XS2 discovery)

Status: **Phase 3 is built and verified.** A shader blob is now classified definitively as
reusable-DXIL, RDNA2 XS2 microcode, or unknown — with stage identity taken only from the shader
token — and the XS2 discovery/parse path exists to triage the first real corpus dump. Nothing is
wired into the boot path, nothing was built into the layer, no boot was run, nothing was committed
or pushed, and `F:\ScarlettCompat` was not read, written, built or run in.

- Tree: `D:\ScarlettCompat-shader` only.
- Suites: Phase 0 **125/125**, Phase 2 **35/35**, Phase 3 full **804/804**, Phase 3 no-DXC **728/728**, all exit 0.
  (Counts are after the review pass in §8, which deleted redundant fields and their assertions.)

---

## 1. New files (Phase 3)

| File | Purpose | LOC |
|---|---|---|
| `include/shader/sc_frontend.h` / `src/shader/sc_frontend.cpp` | The classifier: one entry point, `sc_classify_shader(data, size)` | 287 |
| `include/shader/sc_xs2.h` / `src/shader/sc_xs2.cpp` | XS2 microcode discovery: terminator scan, header-field probing, code-region derivation, observed/inferred evidence | 333 |
| `tests/shader/sc_frontend_main.cpp` | Phase 3 harness (real dxc fixtures, synthetic stage matrix, XS2, unknown, fuzz, JSON) | 595 |
| `tests/shader/build_frontend_test.bat` | Builds **two** binaries (with and without the emitter linked) and runs both | 89 |
| `docs/SHADER-PIPELINE-PHASE3.md` | This document | — |

**What Phase 3 touched, precisely.** No file that came from the upstream repository was edited:
`git diff` against `ae46626` is empty, and every path in the table above is new. Two items written by
earlier phases of this agent *were* changed, and an earlier draft of this section claimed otherwise;
they are listed explicitly instead of glossed over:

| Changed (already in this tree) | Why |
|---|---|
| the container reader (`sc_container.h` / `sc_container.cpp`) | the stage token table was wrong for mesh/amplification (finding 1) |
| `tests/shader/sc_test_main.cpp` | its Phase 0 fixtures asserted the old stage table |

The "new files only" rule held for the repository, not for this subsystem's own modules: correcting
the token table inside files this agent wrote is what the "correct for every stage" criterion
required. The earlier handoffs record it too: `PHASE0.md` §5 and `PHASE2.md` §3 carry correction notes.

---

## 2. The interface Freebuff wires up

```cpp
#include "shader/sc_frontend.h"

const sc_shader_classification c = sc_classify_shader(bytes, size);
switch (c.kind) {
case sc_payload_kind::dxil_container:   // c.reusable_as_is == true
    // hand the blob bytes (exactly what the guest gave us) straight to
    // CreateGraphicsPipelineState / CreateComputePipelineState for c.stage.
    // c.container is metadata (chunks/digest), not a copy of the bytes.
    break;
case sc_payload_kind::rdna2_xs2:        // needs unpacking
    // do NOT create a PSO. Log c.xs2 (JSON via c.to_json()) and route to the
    // Phase 4 unpacker. For XS2 the stage is unknown *by nature*: take it from
    // the PSO slot that is being created (that is ground truth, not a heuristic).
    break;
case sc_payload_kind::dxbc_sm5_legacy:  // needs translation
    // SM5 bytecode: not a PSO bytecode for DXIL. Needs the translation route.
    break;
case sc_payload_kind::dxil_bitcode:     // bare bitcode, no envelope
    break;
case sc_payload_kind::unknown:
    // c.reason says why; log it once per unique sha256 so boot logs stay short.
    break;
}
```

Notes that matter for wiring:

- **No stage parameter exists on purpose.** A caller cannot inject a stage guess; `c.stage` can only
  come from the shader token (DXIL for SM6, SHEX/SHDR for SM5). That is a structural guarantee, not
  a convention.
- `c.reusable_as_is` is false unless there is a DXIL chunk *and* the token identified a stage. A
  container that parses but whose stage is unknown is reported as `dxil_container` with
  `reusable_as_is == false`, so it is visible in the corpus instead of silently producing a PSO with
  the wrong stage.
- `c.carve_offset` is non-zero when the container had to be found behind a prefix (Xbox resource
  headers, RPF chunk decoration). Carving is bounded by a fixed 4096-byte search and fully
  bounds-checked.
- Classification allocates nothing on the GPU and touches no device; it is safe to call before
  device creation, and it is the natural place to dedupe by `c.sha256`.
- Cost: one SHA-256 plus a linear scan; the XS2 path is only entered when no DXIL/SHEX chunk exists.

---

## 3. Measured evidence

### 3.1 Real containers, every stage the classifier must be right about

Produced by dxc 1.9 (Windows SDK 10.0.26100.0) at SM 6.7, then classified:

```
vs: 2876 bytes -> kind=dxil_container stage=vertex        source=shader-token sm=6.7 reusable=1
ps: 2528 bytes -> kind=dxil_container stage=pixel         source=shader-token sm=6.7 reusable=1
gs: 3072 bytes -> kind=dxil_container stage=geometry      source=shader-token sm=6.7 reusable=1
hs: 3628 bytes -> kind=dxil_container stage=hull          source=shader-token sm=6.7 reusable=1
ds: 3272 bytes -> kind=dxil_container stage=domain        source=shader-token sm=6.7 reusable=1
cs: 3128 bytes -> kind=dxil_container stage=compute       source=shader-token sm=6.7 reusable=1
ms: 2872 bytes -> kind=dxil_container stage=mesh          source=shader-token sm=6.7 reusable=1
carved vertex container at offset 64 -> stage=vertex reusable=1
```

Amplification shaders could not be compiled with dxc 1.9 (`DispatchMesh` needs a mesh payload type);
that stage is covered by the synthetic matrix (token 14) and is noted as unverified against real bytes.

### 3.2 Synthetic stage matrix (token → stage), plus refusals

```
ps(0) vs(1) gs(2) hs(3) ds(4) cs(5) ms(13) as(14)  -> all reusable, stage correct, sm=6.7
token kind 6   -> stage=unknown reusable=0 (refused)   // shader-library kind, not a stage
token kind 200 -> stage=unknown reusable=0 (refused)
SM5 SHEX (type 1) -> kind=dxbc_sm5_legacy stage=vertex (not reusable)
```

### 3.3 XS2 path

```
raw payload 112 bytes -> kind=rdna2_xs2 code_start_corroborated=1
header fields: size@0 code@4  regions=1  evidence=5  open=2
no code_offset field -> corroborated=0 region.begin=0
DXBC-wrapped microcode -> kind=rdna2_xs2 envelope=dxbc-chunk:XS20
two s_endpgm words -> 2 regions
no terminator -> kind=unknown ; 4-word blob with terminator -> kind=unknown (both refused)
unaligned payload -> kind=unknown
terminator 0xbf9b0000 -> kind=rdna2_xs2 regions=1
terminator 0xbf9e0000 -> kind=rdna2_xs2 regions=1
```

The region scan accepts every documented SOPP terminator — `s_endpgm` (0xBF810000),
`s_endpgm_saved` (0xBF9B0000) and `s_endpgm_ordered_ps_done` (0xBF9E0000) — because a pixel shader
may end with the PS-done form, and a single-constant scan would have classified such a payload as
unknown. `s_code_end` (0xBF9F0000) is a code marker and is **not** accepted. The table lives in
`sc_isa.h` (Phase 4) so the prober and the instruction walker share one copy. Which forms this
title's payloads actually use is for the corpus to show; the set is documented ISA, the choice to
accept all three is inferred from "a shader can end any of these ways".

### 3.4 Unknown handling and fuzz

```
1 KiB pseudo-random      -> unknown  "no DXBC container, no DXIL bitcode, no XS2 microcode evidence"
truncated container      -> unknown
RDEF-only container      -> unknown  chunks="RDEF"   (parsed, but nothing usable)
bare bitcode             -> dxil_bitcode "needs an envelope before it can be a PSO"
fuzz: 300 pseudo-random blobs (0..600 bytes) -> 300 unknown, 0 false reusable, no crash
```

### 3.5 The frontend has no DXC dependency (proven, not asserted)

`build_frontend_test.bat` builds `sc_frontend_nodxc.exe` from `sc_sha256.cpp`, `sc_container.cpp`,
`sc_xs2.cpp`, `sc_frontend.cpp` and the harness **without** `sc_dxil_emit.cpp` in the link line. It
runs every non-dxc case: **728 checks, 0 failed**. If the frontend ever grows an emitter dependency,
that build stops linking. HLSL→DXIL emission remains in `sc_dxil_backend` / `sc_dxil_emit`, optional
and replaceable.

---

## 4. Findings

### Finding 1 — the token's stage byte is `DXIL::ShaderKind`, not `D3D12_SHADER_VERSION_TYPE`

Phase 2 read the token's stage byte with the D3D12 stage enum. Those agree for 0–5 and disagree from
6 upward. A real `ms_6_7` container reports **13**, not 6:

```
mesh shader DXIL chunk: 67 00 0d 00 2e 01 00 00 'D''X''I''L' 07 01 00 00
                        ^SM 6.7  ^kind 0x0d = 13
```

So the numbering is DXIL's: 0 pixel, 1 vertex, 2 geometry, 3 hull, 4 domain, 5 compute, 6 library,
7 raygen, 8 intersection, 9 anyhit, 10 closesthit, 11 miss, 12 callable, 13 mesh, 14 amplification.
The reader now maps 13/14 to mesh/amplification and deliberately leaves 6–12 and 15 as `unknown`
(they are not pipeline stages). Until this phase, a mesh shader would have been misread as mesh only
by coincidence at 6 — it was in fact read as "library" and now, correctly, as mesh.

**Consequence for Freebuff:** the `sc_container.cpp` copy in `F:\ScarlettCompat` has the old table.
Re-copy the shader subsystem (see §6).

### Finding 2 — classification refuses rather than guesses

Three refusals are deliberate and are asserted in the harness: a DXIL container whose token does not
identify a stage, a payload with a terminator too early to be a shader body (< 8 words), and an
unaligned microcode payload. In each case the reason is recorded and nothing is marked reusable. The
alternative — guessing a stage or a region — would put wrong data into the corpus, which is worse
than a hole in it.

### Finding 3 — XS2 evidence is split into observed and inferred, and says so per line

Every discovery line is prefixed. Examples:

- `observed: 1 terminator word(s), first is s_endpgm at 0x6c` — the word at that offset really is `0xBF810000`.
- `observed: payload size is not a multiple of 4` — measured, then the payload is rejected.
- `inferred: header field @0 = 112 looks like payload_size` — a guess about layout.
- `inferred: code region starts at the code_offset field value 16` — derived, not documented.

Header fields are never taken from fixed offsets, and only corroborated candidates are reported. The
prober scans the first 64 bytes for dwords that *satisfy* a rule (equals the payload size; is 4-aligned
and lands inside the code region) and reports their offsets with the evidence; speculative
"might be a count or version" guesses were removed in review because no rule could test them. When
the first real XS2 blob arrives,
`c.xs2.header.fields` will show which offsets actually hold sizes and code offsets instead of us
having baked in a guess.

## 5. Phase 4 prerequisites (what is deliberately missing)

- **No instruction decoding.** The code region is located but not walked. Doing that correctly needs
  the RDNA2 encoding tables (32/64-bit instruction formats, SOPP/VOP/SMEM/FLAT classes) and a
  validator; faking a length discipline would produce plausible-looking nonsense. Every discovery
  object therefore carries the open question "instruction stream is not decoded".
- **No lifting.** Nothing converts XS2 to DXIL. The emitter seam is ready (Phase 2) but the frontend
  never calls it.
- **Container layout unconfirmed.** `sc_xs2_discover` accepts a payload from a DXBC chunk (largest
  chunk, inferred rule) or a raw blob. Which fourcc the console uses, and whether there is an outer
  header, is unknown until the corpus exists.
- **Amplification shaders**: synthetic only (dxc 1.9 cannot compile a trivial AS).
- **Mesh**: classification is proven, but mesh *hardware* still does not exist on RDNA1 — mesh work
  stays compute-to-draw per `docs/ARCHITECTURE.md`.

## 6. The `F:\ScarlettCompat` copies are stale

Muse copied the Phase 0 files there before the stage-detection fixes. As of this phase:

- `include/shader/sc_container.h`, `src/shader/sc_container.cpp`, `tests/shader/sc_test_main.cpp`
  predate **both** the PSV0→DXIL-token correction (Phase 2) and the `DXIL::ShaderKind` mesh/
  amplification fix (Phase 3). Stage identity from that copy is wrong for mesh/amplification and was
  unverified for compute/geometry/hull/domain.
- The Phase 2 files (`sc_dxil_backend.*`, `sc_gpu_render_test.*`, `sc_triangle_main.cpp`,
  `build_triangle_test.bat`) and all Phase 3 files are not there at all.

Re-copy `include/shader/`, `src/shader/` and `tests/shader/` wholesale. This agent does not touch
`F:\ScarlettCompat`; that is Muse's step.

## 7. Verification

```bat
cd /d D:\ScarlettCompat-shader
tests\shader\build_shader_tests.bat    :: Phase 0: 128 checks, 0 failed
tests\shader\build_triangle_test.bat   :: Phase 2:  35 checks, 0 failed (real GPU)
tests\shader\build_frontend_test.bat   :: Phase 3: 804 checks, 0 failed (full)
                                       ::          728 checks, 0 failed (no-DXC build)
```

Phase 3 coverage: real SM 6.7 containers for vs/ps/gs/hs/ds/cs/ms; carving behind a 64-byte prefix;
the synthetic token matrix for all eight stage kinds; two refusals for non-stage kinds; SM5 legacy;
XS2 strong/weak/wrapped/multi-terminator/negative cases; bare bitcode; empty, truncated, RDEF-only
and absurd containers; 300-blob fuzz; JSON shape and determinism.

## 8. Review pass: what was cut, and why the check counts moved

Phase 3 was re-read after it went green and simplified. Classification behaviour did not change —
the same decisions come from the same evidence — but redundant API was removed so each fact has one
source of truth:

| Removed | Why it was redundant |
|---|---|
| `sc_classify_options` (4 knobs) | no caller ever set them; the one constant that mattered (the 4096-byte carve bound) is now fixed and documented |
| `sc_shader_classification::needs_unpacking`, `::needs_translation` | exactly `kind == rdna2_xs2` / `kind == dxbc_sm5_legacy`; two booleans that could disagree with `kind` |
| `sc_shader_classification::container_parsed` | duplicates `container.ok` |
| `sc_shader_classification::has_program_version` | duplicates `shader_model_major != 0` (the container reader keeps its own flag, where it distinguishes token families) |
| `container_format`, `chunks` | duplicate `container.format` and `container.chunk_list()` |
| `sc_xs2_confidence` enum | a two-level enum for one bit; now `code_start_corroborated` |
| `sc_xs2_field_candidate::inferred`, `::consistent` | both were constant-true: a candidate is only reported when corroborated, and every header field is inferred by definition |
| `sc_xs2_code_region::word_count` | derived from `end - begin` |
| `sc_xs2_is_endpgm()` | unused helper; the scan now asks `sc_isa_is_terminator_word` (Phase 4) |
| "count or version" header guesses | speculation with no testable rule |

The counts moved because the deleted fields had assertions attached to them: Phase 3 full
**837 -> 798**, no-DXC **747 -> 722** (later terminator work took them to **804** / **728**). No case was dropped — real dxc containers for vs/ps/gs/hs/ds/cs/ms,
the synthetic eight-stage matrix, SM5, XS2, unknown/fuzz and JSON are all still covered.
