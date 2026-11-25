# Shader pipeline — Phase 5 handoff (operand widths + wave semantics)

Status: **Phase 5 is built, compiles cleanly as a unit, and is verified by its own harness.** It decodes
the register file of each VOP operand, produces a per-stream register census, and reports wave size as
**unknown from bytes** with the reason — no wave64 assumption is encoded anywhere. Nothing is wired
into the boot path, nothing was built into the layer, no boot was run, nothing was committed or pushed.

- Tree: `D:\ScarlettCompat-shader` only.
- Build gate (new, applies to every phase): `tests\shader\build_all_no_dxc.bat` compiles **every**
  shader source at `/W4 /WX` and archives the no-DXC set. Result: **0 warnings, 0 errors**; the
  decode-path library links without `sc_dxil_emit.cpp`. The emitter is compiled **without**
  `-DSC_FRONTEND_NO_DXC` (see §5), so the gate can never quietly accept a stub in its place.
- Suites: Phase 0 **125/125**, Phase 2 **35/35**, Phase 3 full **804/804** + no-DXC **728/728**,
  Phase 4 **236/236**, Phase 5 **151/151** — all exit 0.

---

## 1. New files (Phase 5)

| File | Purpose |
|---|---|
| `include/shader/sc_wave.h` / `src/shader/sc_wave.cpp` | Operand decoding (SGPR/VGPR/literal), register census, wave facts |
| `tests/shader/sc_wave_main.cpp` | Phase 5 harness: operand fixtures, census, wave assertions, frontend seam |
| `tests/shader/build_wave_test.bat` | Builds/runs the Phase 5 binary without the emitter |
| `tests/shader/build_all_no_dxc.bat` | **Build gate**: all sources at `/W4 /WX`, no-DXC archive |
| `docs/SHADER-PIPELINE-PHASE5.md` | This document |

No existing source file changed in this phase.

---

## 2. The encoding fact that matters (and that memory gets wrong)

On GFX10 the 9-bit VOP source field encodes **SGPRs at 0..255 and VGPRs at 256+n** (256..511), with
`0xFF` selecting the following literal dword. That is the reverse of the older GCN-style convention,
so it is pinned against real output instead of assumed:

```
v_mov_b32_e32 v0, s105      -> 0x7E000269   src0 field = 105    -> s105      (gfx10-sgpr-max test)
v_mov_b32_e32 v1, v2        -> 0x7E020302   src0 field = 258    -> v2        (vop1.s)
v_cvt_f32_i32_e32 v1, v2    -> 0x7E020B02   src0 field = 258    -> v2        (vop1.s)
```

8-bit vector fields (`vdst`, `vsrc1`) name VGPRs only. In SDWA/DPP forms the low byte is a modifier
selector (`0xF9`/`0xFA`), so `src0` is **not** reported as a register for those instructions — the
census counts only their `vdst`, which is the one register they actually name.

Fields between the SGPR file and the VGPR range (106..254) hold the documented inline-constant table.
That table is not transcribed in this module, so those operands are reported as `unclassified`
(counted separately) rather than guessed at. A stream full of inline constants therefore cannot look
like a stream full of registers.

---

## 2.1 Operand widths: only the forms the ISA pins, everything else `unknown`

Width is **opcode-selected**, not format-selected: `v_cvt_i32_f64 v1, v[2:3]` names a register *pair*
as its source, while `v_mov_b32 v1, v2` names one register with the same operand encoding. A decoder
that assumed 32 bits would silently read `v[222:223]` as a single register, so widths are assigned
from a short table and `sc_operand_width::unknown` is the default for everything else.

| Instruction | Rule | Provenance |
|---|---|---|
| `s_mov_b64` (SOP1 opcode `0x04`) | dst pair, src0 pair | `assembler_verified` — real GFX10 bytes below |
| `v_cvt_i32_f64` (VOP1 `3`) | dst b32, src0 pair | `isa_documented` — `vdst_89680f` / `src_d5cd94` |
| `v_cvt_f64_i32` (VOP1 `4`) | dst pair, src0 b32 | `isa_documented` — `vdst_bdb32f` / `src_823582` |
| `v_cvt_f32_f64` (VOP1 `15`) | dst b32, src0 pair | `isa_documented` — as above |
| `v_cvt_f64_f32` (VOP1 `16`) | dst pair, src0 b32 | `isa_documented` — as above |

Every width carries its provenance (`none` / `isa_documented` / `assembler_verified`) so a documented
rule is never presented as an assembler-verified one. The GFX10 bytes that pin `s_mov_b64`:

```
s_mov_b64 s[0:1], s[102:103]  -> [0x66,0x04,0x80,0xbe]   sdst=0 (s[0:1]), opcode=0x04, ssrc0=102
s_mov_b64 exec,    s[2:3]     -> [0x02,0x04,0xfe,0xbe]   sdst=126 (exec), opcode=0x04, ssrc0=2
s_mov_b32 s105,    s1         -> [0x01,0x03,0xe9,0xbe]   sdst=105, opcode=0x03 (not tabulated)
```

SOP1 is now decoded at all (`sdst` 22:16 as 7 bits, `ssrc0` 7:0 as 8 bits). Scalar fields classify as
SGPRs only in `0..105` and as literals at `0xFF`; `exec` (126), `null` (125), the `vcc` halves and the
inline-constant range stay `unclassified`, because this module names registers and does not invent
names for the special files. A width is therefore never claimed for a field it did not classify: in
`s_mov_b64 exec, s[2:3]` the destination is `unclassified/unknown` while the source is `s2/b64`. The
same applies to an inline constant in a 64-bit slot — `v_cvt_f64_i32 v[222:223], 1.0` gives
`dst=v222/b64`, `src0=field:0xf2/unknown`.

What stays `unknown` on purpose: every other VOP1/VOP2/VOPC/SOP1 opcode, including `v_mov_b32`, the SDWA
and DPP forms, and the `_e64` VOP3 encodings (VOP3 is not decoded at all yet, and an unverified VOP3
opcode table would be the same guess in a different place). The census counts the three cases apart
(`b32_refs`, `b64_refs`, `width_unknown_refs`) and the harness asserts that an untabulated opcode
reports `unknown` instead of defaulting to 32 bits.

---

## 3. Wave semantics: what the bytes can and cannot say

`sc_wave_facts` keeps three buckets apart so they cannot be confused:

- **`device_wave_lane_min/max`** — hardware capability, supplied by the caller from the Phase 2
  adapter probe. Recorded as an `observed (device)` evidence line; never converted into a shader fact.
- **`wave_size_known` / `wave_size`** — what the instruction stream determines. **Nothing**, and that
  is the finding: no operand field carries a wave width. A shader's wave size is a per-shader or
  per-pipeline property (chosen at compile time and carried in the shader record), so
  `wave_size_known` stays `false` and `wave_size` stays `0`.
- **`open_questions`** — states that the value must come from the shader record (container metadata
  or a corpus capture), and that nothing here assumes wave64.

The harness asserts the negative: it fails if the module ever starts claiming a wave size without a
source. When the corpus lands, the value belongs in Phase 5's inputs (a parsed shader record), and
this module will then report it as observed with the record named as provenance — not inferred from
instruction patterns.

---

## 4. Interface Freebuff wires up

```cpp
const sc_isa_walk w = sc_isa_walk_region(blob, size, r.begin, r.end);
const sc_wave_decode d = sc_decode_wave(w, caps.wave_lane_min, caps.wave_lane_max);
// d.census: sgpr_refs / vgpr_refs / literal_refs / unclassified_refs / max_sgpr / max_vgpr
//            b32_refs / b64_refs / width_unknown_refs   (width is per-operand, see §2.1)
// d.instructions[i].vdst.width / .provenance        sc_operand_width / sc_width_provenance
// d.wave.wave_size_known == false  -> do not branch on a wave width
// d.to_json()                      -> log with the walk's JSON
```

`sc_decode_wave` only consumes instructions the walker synced through, so a stop still bounds the
census; when the walk did not end on a terminator, an open question says the census covers the
verified prefix only.

---

## 5. Build gate (the requirement added for this phase)

```bat
cd /d D:\ScarlettCompat-shader
tests\shader\build_all_no_dxc.bat
```

Result: `all shader sources compiled at /W4 /WX with zero warnings (C++17 and C++14); no-DXC library built`.
The C++14 pass exists because the layer's build may not pass `/std:c++17`; the sources use no
C++17-only feature (no `if constexpr`, structured bindings, `optional` or `string_view`), and that is
now proven rather than assumed. The layer build itself was not run (not this agent's job).

It compiles, in order: the nine no-DXC sources (`sc_sha256`, `sc_container`, `sc_xs2`, `sc_frontend`,
`sc_isa`, `sc_wave`, `sc_capture`, `sc_dxil_backend`, `sc_gpu_render_test`), then `sc_dxil_emit.cpp`
on its own (compile-only, so no `dxcompiler` import is needed), then archives the no-DXC objects into
`out\shader_tests\sc_shader_nodxc.lib`. The emitter object is deleted before archiving, which is what
proves the decode path has no DXC link dependency.

**The emitter is no longer compiled under `-DSC_FRONTEND_NO_DXC`.** That define belongs to the no-DXC
configuration of `sc_frontend.cpp`; passing it to `sc_dxil_emit.cpp` was wrong on two counts. First, it
is not that file's configuration: the emitter is a subprocess driver for `dxc.exe` and includes no DXC
headers, so `SC_FRONTEND_NO_DXC` changes nothing in it today. Second — the reason it was still a bug —
the moment anyone gave the emitter a `#ifdef SC_FRONTEND_NO_DXC` stub, the gate would have compiled the
stub and reported the file green while the real translation unit was never built. The gate now builds
the real file at `/W4 /WX`, which is what makes the check meaningful. (Checked, not assumed: a grep for
`SC_FRONTEND_NO_DXC` finds it only in the batch files and `sc_frontend_main.cpp`; `sc_dxil_emit.cpp`
currently has no such branch, so nothing was being stubbed out — the gate was simply unable to notice
one.)

---

## 6. Measured output (Phase 5 harness)

```
field 105 -> sgpr s105 ; field 258 -> vgpr v2 ; field 511 -> vgpr v255 ; field 255 -> literal
field 200 -> unclassified field:0xc8
v_mov_b32 v0, s105          -> vdst=v0  src0=s105  vsrc1=-
v_mov_b32_sdwa v0, s105     -> vdst=v0  src0=-     vsrc1=-     (modifier form)
vop1 with src0=0xFF         -> literal, words=2, literal_refs=1
instructions=3 sgpr=1 vgpr=4 literal=0 unclassified=0 max_sgpr=105 max_vgpr=2
observed (device): wave lane count 64..64 from the Phase 2 adapter probe
inferred: ... no instruction field carries it; wave_size_known stays false ...
region [16,32) -> 2 instruction(s), sgpr=1 vgpr=3, wave_size_known=0

s_mov_b64 s[0:1], s[102:103]         dst=s0    /b64      src0=s102  /b64
v_cvt_i32_f64 v1, v[2:3]             dst=v1    /b32      src0=v2    /b64
v_cvt_f64_i32 v[1:2], v2             dst=v1    /b64      src0=v2    /b32
v_cvt_f32_f64 v1, v[2:3]             dst=v1    /b32      src0=v2    /b64
v_cvt_f64_f32 v[1:2], v2             dst=v1    /b64      src0=v2    /b32
v_mov_b32 v0, s105 (not tabulated)   dst=v0    /unknown  src0=s105  /unknown
s_mov_b64 exec, s[2:3]               dst=unclassified/unknown   src0=s2/b64
v_cvt_f64_i32 v[222:223], 1.0        dst=v222/b64               src0=field:0xf2/unknown
151 checks, 0 failed
```

---

## 7. What the corpus (or its absence) proves

No real shader has been decoded, so this phase proves the encoding rules and the module's behaviour on
assembler-derived words and synthetic payloads — nothing about GTA 6's shaders. Two specific things
stay open until the corpus:

- whether real streams use operand fields this module classifies correctly at the same rate (a stop
  would bound the census, and that is reported, not hidden);
- the actual wave size each shader was compiled for.

---

## 8. Phase 6–8 status, stated plainly

- **Phase 6 (resources and pipeline).** Not started: it decodes *captured PSO records*, and capture
  has no records because the layer's slots 86/16 do not create PSOs yet. Building it now would mean
  inventing record layouts. Gate: corpus.
- **Phase 7 (BCPack).** Not started. The B1 spec exists (`docs/FREEBUFF-BCPACK-TEXTURE-SPEC.md`) and
  the layer currently returns `E_NOTIMPL` for every BCPACK mode; there is no decoder in this tree and
  no real BCPack bitstream to verify one against, and the rANS bitstream is not documented publicly.
  Implementing it blind would be guesswork; the honest prerequisite is a captured BCPack chunk.
- **Phase 8 (mesh-as-compute).** Not started: it is a compute-to-draw path, which needs the layer's
  device/PSO plumbing (Freebuff's side) plus the CPU prototype that is not in this tree.

The next shader work that is buildable without the layer is a Phase 6 *input* decoder for whatever
record format capture will write — but that format is not fixed yet, so it would be invented too.
