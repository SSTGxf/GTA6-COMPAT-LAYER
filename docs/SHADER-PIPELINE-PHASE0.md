# Shader pipeline — Phase 0 handoff (`shader_capture`)

Status: **Phase 0 (shader capture) is built and verified standalone.** Nothing is wired into the
boot path. No existing file was modified, nothing was built into the layer, no boot was run, and
nothing was committed or pushed.

- Tree used: `D:\ScarlettCompat-shader` (fresh clone of `SSTGxf/ScarlettCompat` at `ae46626`).
  `F:\ScarlettCompat` was never read, edited, built or run.
- Reference projects (XCL, VIctory, Leonida) informed *what* to look at only; no code was copied.
  Everything below is clean-room MIT code.

---

## 1. What exists (new files only)

| File | Purpose | LOC |
|---|---|---|
| `include/shader/sc_sha256.h` / `src/shader/sc_sha256.cpp` | SHA-256 + FNV-1a + hex (clean-room, no deps) | ~230 |
| `include/shader/sc_container.h` / `src/shader/sc_container.cpp` | DXBC/DXIL container reader, stage detection, JSON | ~470 |
| `include/shader/sc_capture.h` / `src/shader/sc_capture.cpp` | PSO capture records, de-dupe store, corpus dumper | ~700 |
| `include/shader/sc_dxil_emit.h` / `src/shader/sc_dxil_emit.cpp` | Replaceable HLSL→DXIL emission seam (dxc.exe backend) | ~300 |
| `tests/shader/sc_test_main.cpp` | Standalone harness (no game, no device, no D3D12) | ~590 |
| `tests/shader/build_shader_tests.bat` | Builds/runs **only** the harness | ~60 |
| `docs/SHADER-PIPELINE-PHASE0.md` | This document | — |

`git status` in the shader tree shows exactly three untracked directories (`include/shader/`,
`src/shader/`, `tests/shader/`) plus this doc. No tracked file changed.

---

## 2. Environment findings (verified on this machine)

**DXC is available.** This is no longer an unverified assumption:

```
dxc.exe         C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe
dxcompiler.dll  1.9(5184-21d28f72)(1.9.2602.17) — 1.9.2602.17
dxil.dll        present in the same folder
```

A real `vs_6_7` + `ps_6_7` pair was compiled through the emitter during the harness run:

```
VS: 2876 bytes, chunks="SFI0,ISG1,OSG1,PSV0,STAT,HASH,DXIL", container stage=vertex (psv0)
PS: 2540 bytes, container stage=pixel (psv0)
```

Practical consequences:

- The DXIL-emission step is **not** a blocker; it is already exercised end to end.
- `dxc.exe` is driven as a **subprocess**, so the module needs no `dxcapi.h`, no COM GUIDs and no
  link-time dependency. An in-process `IDxcCompiler3` backend only has to implement
  `sc_dxil_emitter`; nothing else changes.
- Toolchain used for the harness: MSVC 14.44.35207 (BuildTools) + Windows SDK 10.0.26100.0.
  `build_shader_tests.bat` sets `PATH`/`INCLUDE`/`LIB` directly from `vswhere` output plus directory
  scans instead of calling `vcvars64.bat`/VsDevCmd. That is a deliberate choice for determinism: the
  script has no dependency on VsDevCmd behaviour and prints the MSVC toolset and SDK version it
  picked, which makes a wrong-toolchain build obvious in the log.
  (For the record: an earlier attempt through a shell wrapper produced an empty build log. That was
  argument mangling in the wrapper — `/c` rewritten to a path and `/nologo` treated as a file — not
  a project or toolchain problem. Invoking the `.bat` directly works.)

---

## 3. Interface contract for the boot path (Freebuff's side)

Wire this only when slots 86/16 exist. The capture module needs **no device, no DXGI object and no
allocation** — it copies bytes into a store.

### 3.1 Fill a descriptor at `CreatePipelineState` (slot 86)

```cpp
#include "shader/sc_capture.h"   // NEW header, no d3d12 headers required

static sc::sc_capture_store g_shader_capture;   // process-wide, see 3.3 for locking

// inside the slot-86 handler, once the stream desc has been parsed:
sc::sc_capture_pso_desc desc;
desc.source_slot = 86;
desc.node_mask = stream.NodeMask;
desc.flags = static_cast<uint32_t>(stream.Flags);
desc.sample_count = stream.SampleDesc.Count;
desc.sample_quality = stream.SampleDesc.Quality;
desc.sample_mask = stream.SampleMask;
desc.ib_strip_cut_value = stream.IBStripCutValue;
desc.patch_control_points = stream.PatchControlPoints;
desc.primitive_topology_type = stream.PrimitiveTopologyType;
desc.dsv_format = stream.DSVFormat;
for (UINT i = 0; i < stream.NumRenderTargets; ++i) desc.rtv_formats.push_back(stream.RTVFormats[i]);

// graphics subobjects (only the ones the guest supplied)
desc.rasterizer.fill_mode = stream.RasterizerState.FillMode; /* ... rest verbatim ... */
desc.depth_stencil.depth_enable = stream.DepthStencilState.DepthEnable; /* ... */
for (UINT i = 0; i < stream.NumRenderTargets; ++i) { /* RenderTarget[8] blend state */ }

if (stream.pRootSignature && stream.pRootSignature->blob)
    desc.root_signature.assign(blob->GetBufferPointer(), blob->GetBufferSize());

// shaders: the stage is known here even if the container cannot prove it
sc::sc_capture_stage_blob vs;
vs.stage = sc::sc_stage::vertex;
vs.bytes.assign(stream.VS->GetBufferPointer(), stream.VS->GetBufferSize());
desc.shaders.push_back(vs);
// same for HS/DS/GS/PS; CS lives on its own path (slot 86 compute variant)

const sc::sc_capture_add_result r = g_shader_capture.add(desc);
// r.added == false means "same PSO seen again" — hits++ and no extra corpus files.
```

Notes that matter:

- **Enum values are the D3D12 numeric values**, stored as `uint32_t`, so no cast mapping is needed
  and the module never has to include `d3d12_xs.h`.
- The caller-supplied `stage` is authoritative for the record; the container's own opinion is kept
  separately as `stage_from_container` / `stage_source` so a parser mistake is visible, never silent.
- `desc.debug_name` is optional and is **excluded from the signature** (two PSOs differing only in
  name de-duplicate).
- Compute pipelines: use the same struct, one `compute` blob, `rtv_formats`/`dsv_format` empty.

### 3.2 Dump the corpus

Pick one policy (all work with the same store):

- dump immediately on first N unique PSOs (best for the current blocker: you get the corpus even if
  the process dies later), or
- dump at shutdown, or
- dump on a signal / debug key.

```cpp
sc::sc_capture_dump_report rep = sc::sc_capture_dump(g_shader_capture, "shader_corpus");
// rep.ok, rep.records, rep.files, rep.bytes, rep.errors[]  — logging these is enough
```

Recommendation for the first boot that reaches slot 86: dump **every** PSO to a per-boot directory
(`shader_corpus/boot_<n>/`), because the corpus does not exist yet and the first few hundred PSOs
are the whole discovery result.

### 3.3 Threading

`sc_capture_store` is **not** internally synchronised (deliberately: it stays dependency-free and
allocation-light). Slot 86 can be called concurrently. Guard `add()` with the same mutex the layer
already uses for its slot tables, or give each worker thread its own store and merge dumps. Do not
call `add()` and `dump()` concurrently on one store.

### 3.4 Include/build wiring

- Add `include` as an include directory (already the convention) and compile
  `src/shader/sc_sha256.cpp`, `sc_container.cpp`, `sc_capture.cpp` into the layer.
- `sc_dxil_emit.cpp` is **optional at this stage** — it is only needed once translation starts; the
  capture path does not reference it.
- If `build.bat` enumerates sources, adding the three `.cpp` files is the whole change. I did not
  touch `build.bat`.

---

## 4. Corpus format

```
shader_corpus/
  manifest.json                    // totals + per-record summary, one line of JSON
  index.tsv                        // tab-separated, one row per unique PSO (+ header)
  pso_0000_01812f77/
    vs.bin  ps.bin                 // container bytes EXACTLY as the guest supplied them
    rootsig.bin                    // serialized root signature blob (when present)
    record.json                    // full descriptor + per-stage analysis
```

- Directory name: `pso_<seq:04>_<low32 of signature in hex>`.
- `index.tsv` columns:
  `seq, signature, hits, stages, shaders, shader_bytes, root_sig_bytes, sample_count, rtv_count, dsv_format, source_slot, dir`
- `record.json` (schema `scarlettcompat.shader_capture.pso/1`) carries: signature, hit count, stage
  list, all render/raster/depth/blend/sample/topology fields, the input layout, root signature
  size + SHA-256, and per stage: `stage`, `stage_from_container`, `stage_source`, `format`, `sha256`,
  `size`, `chunk_count`, `chunks`, `has_dxil`, `has_root_signature`, and `parse_error`
  when the container did not parse (the bytes are still written either way — never lose evidence).
- `manifest.json` (schema `scarlettcompat.shader_capture.manifest/1`) carries `adds`,
  `unique_psos`, `duplicates`, `stage_histogram` and the record list.

De-duplication is by content, not by pointer: the signature is FNV-1a 64 over a canonicalised
descriptor (formats, states, input layout, root signature bytes, and every stage blob in stage
order). Repeated submissions of an identical PSO increment `hits` instead of writing new files.

---

## 5. What the container reader knows (and how it was verified)

Handles, with full bounds checking (never reads outside the caller's buffer):

| Input | Detected as |
|---|---|
| `DXBC` envelope + `DXIL` chunk (Xbox Series X\|S SM6) | `dxbc+dxil` |
| `DXBC` envelope + `SHEX`/`SHDR` chunk (SM5) | `dxbc+sm5` |
| Bare `BC\xC0\xDE` LLVM bitcode | `dxil-bitcode` |
| Junk, truncation, absurd chunk counts, out-of-range chunks | rejected with a reason |

Stage detection: DXIL chunk version token (SM6, authoritative) → `SHEX/SHDR` program token (SM5) →
`unknown` (never guessed silently). PSV0 and signature presence are deliberately not used — see the
correction in `SHADER-PIPELINE-PHASE2.md` §3, finding 2.

> **Correction (Phase 2):** this document originally claimed the stage lives in PSV0 dword 1. That
> was wrong for compute/geometry, and the reader has been changed to use the DXIL token instead.
> Everything else in this document still describes the shipped behaviour.

**Stage comes from the DXIL token (corrected in Phase 2).** Measured PSV0 dword 1 is 1 for vertex,
0 for pixel, 0 for **compute** and 3 for geometry — so it is not a stage field, and the earlier
dword-1 reading was a false positive from a two-sample check. The authoritative field is the DXIL
chunk's version token: bits 0–15 = shader model (`0x67` = 6.7), bits 16–23 = stage. Compute also
turns out to ship ISG1/OSG1, so signature-presence heuristics were removed entirely. The reader now
degrades to `unknown` rather than guessing, and the real-DXIL test asserts the parsed stage equals
the true stage for every container it compiles.

Other containers seen in real output: `SFI0, ISG1, OSG1, PSV0, STAT, HASH, DXIL`. All are enumerated
and reported; the parser does not require any particular one, so console-specific chunks the game
may ship will simply appear in `chunks` for us to inspect.

> **Review pass (after Phase 3).** Once stage detection came only from the token, the PSV0 and
> signature *flag* fields (`has_psv0`, `psv0_chunk`, `has_input_sig`, `has_output_sig`,
> `has_patch_sig`, `root_sig_chunk`) and their parse branches were write-only, so they were removed,
> together with the `psv0`, `io_signature` and `caller` values of `sc_stage_source` (none of which
> could be produced). Chunk names are still listed in `chunks`, and no byte of the parser's output
> for stage/format/size changed. Phase 0's harness went 128 → 125 checks because three assertions
> tested the deleted fields; `record.json` no longer carries `has_psv0`.

---

## 6. Verification performed (and what was NOT verified)

Harness: `tests/shader/build_shader_tests.bat` (or `sc_shader_tests.exe <outdir>` directly).
Latest run: **125 checks, 125 passed, 0 failed** (after the review pass described in §5).

Covered:

1. SHA-256 against four published vectors (empty, `abc`, the 56-byte NIST block, 1,000,000 × `a`),
   plus FNV-1a determinism and incremental/one-shot agreement.
2. Synthetic SM5 vertex container: chunks, program token, format, version, stage.
3. Synthetic DXIL containers: stage from the DXIL token for compute/mesh/vertex/pixel/geometry, the
   library kind staying `unknown`, `has_dxil`.
4. Robustness: all 96 truncations of a valid container rejected with a message; zero/absurd chunk
   counts, out-of-range chunk payloads and lying total sizes rejected; no crash.
5. Container located inside a decorated blob (offset 64) and parsed from that offset.
6. Capture: signature stability across separately built identical descriptors, difference on a real
   field change, de-dupe (`adds=4, unique=2, duplicates=2`, hits counter), stage CSV, byte totals.
7. Dump round-trip: file/byte counts, `manifest.json` numbers, `index.tsv` row count, and the dumped
   `vs.bin`/`ps.bin`/`rootsig.bin` compared byte-for-byte against the inputs.
8. JSON escaping for quotes/backslashes in debug names and semantics.
9. **Real DXC**: `vs_6_7`/`ps_6_7` compile, container parse, correct stage, then the real DXIL flows
   through the capture store and corpus dump (SHA-256s recorded).

Not verified, by design or by absence:

- **No real Xbox shader has been seen yet.** There is no corpus and the game has not reached PSO
  creation, so we do not yet know whether the guest ships DXBC SM5, DXIL SM6, or a console-specific
  container. The reader is built to report what it sees rather than assume.
- No device, no PSO creation, no root signature creation, no rendering — Freebuff owns those.
- The dxc **subprocess** path was verified; an in-process `dxcompiler.dll` backend is not written.
- Nothing here proves a translated shader will *render*; that requires the boot path and the first
  frame.

---

## 7. Suggested next phases (not built)

1. **Corpus triage** — after the first boot with slot 86 wired, read `manifest.json`/`index.tsv`:
   stage histogram, container formats, which PSOs carry `RTS0`, which containers fail to parse.
2. **Root signature replication** (slot 16) — the blobs are already captured verbatim in
   `rootsig.bin`; a `CreateRootSignature` implementation can replay them and use the capture
   signature as its cache key.
3. **Translation plan** — for each captured stage decide *reuse as-is* (already DXIL SM6.6/6.7) vs
   *translate*; the emitter seam is ready, the HLSL/SPIR-V plan is not written.
4. **PSO cache** — keyed on the same signature, so a repeated PSO costs one hash, not a creation.
5. **Mesh work** stays pure compute-to-draw on RDNA1 (no hardware mesh tier). Nothing in Phase 0
   assumes mesh shaders exist; the `mesh`/`amplification` stages are recorded, not used.

Wave64 note: RDNA1 has native wave64, so any HLSL that relies on wave semantics should be emitted
with wave64 (`-enable-16bit-types` is unrelated; per-shader flags go through
`sc_dxil_request::extra_args`). No emulation is planned anywhere in this subsystem.

---

## 8. How to reproduce the verification

```bat
cd /d D:\ScarlettCompat-shader
tests\shader\build_shader_tests.bat
```

Expected tail: `117 checks, 117 passed, 0 failed` and `harness exit=0`.
Corpus written to `out\shader_tests\corpus\{corpus_basic,corpus_escape,corpus_dxc}`.

## 9. How to move these files into the main tree

Only new files, no merges needed:

```
include\shader\sc_sha256.h      sc_container.h      sc_capture.h      sc_dxil_emit.h
src\shader\sc_sha256.cpp        sc_container.cpp    sc_capture.cpp    sc_dxil_emit.cpp
tests\shader\sc_test_main.cpp   build_shader_tests.bat
docs\SHADER-PIPELINE-PHASE0.md
```
