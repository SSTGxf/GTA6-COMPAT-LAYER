# Shader pipeline - Phase 6: resources and pipeline

Status: **built and green on synthetic records. Not validated against real PSOs.**
Nothing here has seen a game PSO: the layer still returns `E_NOTIMPL` for the
pipeline-state slot, so no capture record exists yet. Every PSO record in the harness
is labelled SYNTHETIC. The root signature *bytes* are real - they are
`D3D12SerializeRootSignature` (1.0) and `D3D12SerializeVersionedRootSignature` (1.1)
output - but they were produced for descs written in the harness, not read from the
game.

## What Phase 6 is

Phase 6 turns one captured PSO into the binding the layer would need to service it:

* **root signature** - parsed from the record's own `root_signature` bytes, in both
  serialized versions (1.0 and 1.1): parameter kinds, visibilities, registers, constants
  counts, descriptor flags, descriptor ranges with their flags, static samplers;
* **root layout** - where each parameter's arguments sit in the root-argument space and
  how each table's ranges pack into one contiguous run of 32-byte descriptors;
* **pipeline-state binding** - which captured shader binds to which stage, plus the
  formats and fixed-function state the record already carries.

`sc_pipeline_bind(desc)` takes the Phase 0 record and **nothing else**. The parameter
and table structure is parsed out of `desc.root_signature`; no caller-supplied
description is needed. `sc_root_signature_matches` exists only as an optional
cross-check for a caller that happens to hold its own `D3D12_ROOT_SIGNATURE_DESC`.

## What was built

| file | what it is |
| --- | --- |
| `include/shader/sc_pipeline_binding.h` | contract: parsed root-signature description, blob identity, root/table binding, pipeline binding, validator, cross-check |
| `src/shader/sc_pipeline_binding.cpp` | implementation: RTS0 parser, layout builder, binding, validator, cross-check |
| `tests/shader/sc_pipeline_binding_fixtures.h` | eleven real serialized root signatures - six from the 1.0 serializer, five from the 1.1 serializer - each with provenance |
| `tests/shader/sc_pipeline_binding_main.cpp` | harness over synthetic records |
| `tests/shader/build_pipeline_binding_test.bat` | builds and runs the harness, `/W4 /WX`, no D3D header, no `sc_dxil_emit.cpp` |
| `docs/SHADER-PIPELINE-PHASE6.md` | this handoff |

`sc_pipeline_binding.cpp` is the eleventh no-DXC source in
`tests/shader/build_all_no_dxc.bat` and `tests/shader/build_link_smoke.bat`.

## The RTS0 encoding, pinned empirically

The encoding was measured, not guessed. A one-off generator (in `out/shader_tests/rsgen`,
not shipped) called `D3D12SerializeRootSignature` for **eight** synthetic descs that vary
the parameter count (0-4), every parameter type, one and two tables, range counts 1-3,
sampler counts 0-2 and flags 0/1/5, then dumped every DWORD. Diffing those dumps gave one
layout that reproduces all eight blobs exactly to the last byte:

```
container: "DXBC", 16-byte digest, container version, total size, chunk count (1),
           chunk offset (36), "RTS0", chunk size

RTS0 chunk, offsets relative to the chunk start:
  +0   version (1)
  +4   parameter count
  +8   parameter-section offset (24)
  +12  static-sampler count
  +16  static-sampler-section offset  (== chunk size when there are none)
  +20  flags

parameter entries, at the parameter-section offset, 12 bytes each:
  +0   parameter type   (0 table, 1 32-bit constants, 2 CBV, 3 SRV, 4 UAV)
  +4   visibility
  +8   payload offset, chunk-relative

payloads, in parameter order, each at its own offset:
  32-bit constants   register, register space, Num32BitValues
  CBV/SRV/UAV        register, register space
  table              range count, range-section offset (chunk-relative)
ranges, 20 bytes each at the range-section offset:
  range type (0 SRV, 1 UAV, 2 CBV, 3 sampler), descriptor count,
  base register, register space, offset in table

static samplers, 52 bytes each at the sampler-section offset:
  filter, address U/V/W, mip LOD bias, max anisotropy, comparison function,
  border colour, min LOD, max LOD, shader register, register space, visibility
```

Two properties made this checkable rather than cosmetic: every sample's parsed sections
end exactly at the blob size (`consumed == size`), and the earlier confusion about the
two-parameter blob was resolved rather than papered over - the parameter entries are
**3 DWORDs each with the payloads stored after all entries**, which is why a naive
sequential read lands one DWORD off in that shape. `kBlobProbeTwo` is that exact desc,
regenerated, and it parses cleanly.

### Version 1.1: the same container, version 2 inside

The 1.1 serializer (`D3D12SerializeVersionedRootSignature` with `D3D_ROOT_SIGNATURE_VERSION_1_1`)
emits the same DXBC container, the same `RTS0` chunk id and the same 24-byte chunk header,
with **chunk version 2** instead of 1. Measured against five 1.1 blobs, the deltas are
exactly two:

* every CBV/SRV/UAV root-descriptor payload grows from 2 to **3 DWORDs** (register,
  register space, `D3D12_ROOT_DESCRIPTOR_FLAGS`);
* every descriptor range grows from 20 to **24 bytes**, with the `D3D12_DESCRIPTOR_RANGE_FLAGS`
  DWORD between the register space and the offset in table.

Parameter entries (3 DWORDs), 32-bit-constant payloads, table payloads (range count +
range-section offset), range field order apart, and the 52-byte static samplers are
identical - the same blob sizes come out for descs with no flags (for example 92, 172
bytes), and 8 bytes more per range or root descriptor when flags are present.

That is why the parser is keyed on the chunk version: it reads `root_version` from the
header and switches the range stride and the descriptor-payload size, nothing else. Each
1.1 sample also ends exactly on the blob's last byte, and the cross-version test below
shows a desc serialized by both APIs parsing to the same neutral description.

The parser rejects a blob whose header, counts, types or offsets do not stay inside the
chunk: not a DXBC container, wrong declared size, wrong chunk id/count/size, unknown
parameter type, payload or range sections outside the chunk, zero ranges, zero-descriptor
ranges, zero 32-bit constants, unknown range type, samplers outside the chunk.

## The binding contract

Root arguments, in parameter order:

* CBV / SRV / UAV root descriptor: 2 DWORDs;
* descriptor table: 1 DWORD (a heap pointer);
* 32-bit constants: `num_32bit` DWORDs.

Offsets are cumulative from 0, so the space is exactly packed, and the total stays
inside 64 DWORDs.

Descriptor tables, one per table parameter, in parameter order:

* ranges are sorted by their offset in the table before packing, so two descriptions
  that list the same ranges in a different order produce the same table;
* each range starts exactly where the previous one ended (no gaps, no overlaps), and a
  range with zero descriptors is rejected;
* every descriptor is 32 bytes, so a table of *n* descriptors is `n * 32` bytes;
* a table's base is the running descriptor count, so tables tile one contiguous run.

Pipeline state: stages are reported in **canonical order** (vertex, hull, domain,
geometry, pixel, compute, mesh, amplification) whatever order `desc.shaders` uses.
Unknown-stage shaders, empty stage blobs, a missing root signature, more than eight
render targets, or more render-target states than formats are rejected.

## What the harness proves (all synthetic)

`261 checks, 0 failed` from `out\shader_tests\sc_pipeline_binding_test.exe`, built by
`tests\shader\build_pipeline_binding_test.bat` at `/W4 /WX` with zero warnings:

1. **parser, against real bytes** - six `D3D12SerializeRootSignature` blobs parse to the
   exact desc they were generated from: one root-constants parameter (b0xa1/s0xb2, six
   values, vertex visibility); two tables (SRV 8 / UAV 3, different spaces and
   visibilities); constants + a two-range table + a root SRV + one static sampler;
   four parameters with two tables of different sizes; two static samplers with no
   parameters; and the two-parameter probe with flags `0x5` and `MaxLOD 100`.
2. **parser refusals** - null, truncated, unknown parameter type, payload offset outside
   the chunk, zero 32-bit constants, zero ranges, and a sampler section outside the chunk
   are each refused with a reason.
3. **binding from the record alone** - the three-parameter blob packs root arguments at
   DWORD 0 (four constants), 4 (one table pointer), 5 (two for the root SRV) for seven
   total; its table holds 11 descriptors at base 0 (352 bytes) with one static sampler.
   The four-parameter blob tiles two tables at base 0 (7 descriptors) and base 7
   (1 descriptor) into one run of 8 (256 bytes) with five root DWORDs.
4. **pipeline state** - vertex and pixel bind with the right byte counts and digests,
   both render-target formats, depth format, sample mask and two input slots come
   through.
5. **validator** - a binding parsed from the record passes with two stages, two formats,
   one table, two ranges, three packed argument offsets, and no overlaps.
6. **cross-check** - a hand-filled description equal to the parsed one matches; the same
   description with its ranges listed in the other order still matches; a changed
   register, changed flags or a changed static sampler each fail with a reason.
7. **order independence** - reversing a table's ranges leaves the same packing
   (11 descriptors, 352 bytes); an overlapping table parsed from a corrupted blob is
   rejected, and the validator reports the same overlap count with its ranges flipped.
8. **refusals** - a corrupted render-target format, a dropped stage, an extra
   render-target state, a changed sample count, a changed format, no stages, an
   unidentified stage, no root signature, more than eight render targets and a corrupt
   root signature each fail.

9. **version 1.1** - five 1.1 blobs parse to their descs: constants (version 2, flags 0),
   two tables with 24-byte ranges (SRV 8 / UAV 3, offsets 0 and 5), range and
   root-descriptor flags (`DATA_VOLATILE` = 2 on the range, `DATA_STATIC_WHILE_SET_AT_EXECUTE`
   = 4 on the CBV), and two static samplers with the unchanged 52-byte encoding. The
   three-parameter 1.1 blob binds to the same layout as its 1.0 twin (7 root DWORDs, one
   table of 11 descriptors = 352 bytes, one sampler) and passes the validator; the
   flag-carrying blob binds to 3 root DWORDs and 4 descriptors (128 bytes).
10. **cross-version equality** - the same desc serialized by the 1.0 and the 1.1 API
    parses to two descriptions that `sc_root_signature_matches` reports as equal, both for
    the three-parameter full case and for the two-table case.

The oracle is a contract test, not a shader comparison: there is no real shader in this
phase to compare against. What it establishes is that the parser reads the real encoding
correctly on every sample, that the layout rules are exact and insensitive to every order
the hardware and the capture do not fix, and that corruption is caught - so a green run
over a real corpus would mean something.

## Build and checks

```
tests\shader\build_pipeline_binding_test.bat      exit 0, 261 checks, 0 failed, no warnings
tests\shader\build_all_no_dxc.bat                 exit 0, eleven no-DXC sources, C++17 + C++14, /W4 /WX
tests\shader\build_link_smoke.bat                 exit 0, negative LNK2019 without sc_dxil_emit.cpp, clean link + run with it
```

No layer build, no boot, no commit, no push, and nothing wired into the boot path.

## Still open

* **Real PSO records.** This is the gate: the layer's pipeline-state slots (86/16) still
  return `E_NOTIMPL`, so there is no captured record to validate against. Until a real
  record runs through `sc_pipeline_bind` and `sc_pipeline_verify`, Phase 6 is proven on
  synthetic descs and real serializer output only, and must be described that way.
* **Root signature versions beyond 1.1.** The measured set is chunk versions 1 (1.0) and
  2 (1.1). A chunk version of 3 or more, or a version-2 blob whose stride does not match,
  is refused rather than guessed.
* **Extra chunks.** The parser requires exactly one `RTS0` chunk at offset 36. A blob
  that also carries `RDAT`/`STAT` data would be refused; that is deliberate until a real
  sample shows the shape.
* **Root signature flags.** The flags are parsed and carried, but their meaning
  (deny-access bits, IA layout, stream output) is not interpreted by the binding yet.
* **Per-stage capture report.** `sc_capture_shader_report` already carries a sha256 and
  container format per stage; the binding recomputes the digest from the blob instead of
  reusing it, so the two can be compared once records exist.
* **Descriptor heap types.** The layout assumes one contiguous 32-byte-descriptor run per
  table, which holds for CBV/SRV/UAV heaps; a sampler table in a separate heap is
  modelled but not distinguished in the binding.
