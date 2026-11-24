# Shader pipeline — Phase 4 handoff (RDNA2 instruction decode)

Status: **Phase 4 is built and verified.** The Phase 3 frontend's code regions can now be stepped as
GFX10 instruction streams: each word is classified into its instruction format, its length is
resolved (32/64-bit plus the extra dword a literal/SDWA/DPP form appends), and the walk stops on
`s_endpgm` or on the first word it cannot justify — with the offset and the reason. Nothing is wired
into the boot path, nothing was built into the layer, no boot was run, nothing was committed or
pushed, and `F:\ScarlettCompat` was not read, written, built or run in.

- Tree: `D:\ScarlettCompat-shader` only.
- Suites: Phase 0 **125/125**, Phase 2 **35/35**, Phase 3 full **798/798**, Phase 3 no-DXC **722/722**,
  Phase 4 **126/126** — all exit 0.

---

## 1. New files (Phase 4)

| File | Purpose |
|---|---|
| `include/shader/sc_isa.h` / `src/shader/sc_isa.cpp` | The decoder: encoding table, length rules, stream walker, region walker |
| `tests/shader/sc_isa_main.cpp` | Phase 4 harness (assembler-pinned table, every format bucket, every stop reason, frontend seam) |
| `tests/shader/build_isa_test.bat` | Builds and runs one binary **without** `sc_dxil_emit.cpp` linked |
| `docs/SHADER-PIPELINE-PHASE4.md` | This document |

No existing file was edited in this phase: the decoder is a new module and the harness is new.

---

## 2. The interface Freebuff wires up

```cpp
#include "shader/sc_frontend.h"
#include "shader/sc_isa.h"

const sc_shader_classification c = sc_classify_shader(bytes, size);
if (c.kind == sc_payload_kind::rdna2_xs2) {
    for (const sc_xs2_code_region& r : c.xs2.code_regions) {
        const sc_isa_walk w = sc_isa_walk_region(bytes, size, r.begin, r.end);
        // Log w.to_json() once per unique c.sha256.
        // w.synced == true  -> the region bounds and the encoding table agree with the bytes.
        // w.stop            -> why stepping stopped; the offset is absolute in `bytes`.
    }
}
```

Notes that matter for wiring:

- **This is triage, not a gate.** Nothing here should decide whether a PSO is created. Its job is to
  answer, from the first real corpus dump, whether the guest's XS2 payload is what Phase 3 inferred,
  and where.
- `sc_isa_walk_region` bounds-checks `begin`/`end` against the blob before reading; an out-of-range
  region returns a `truncated` walk with an evidence line instead of touching memory.
- The walker never resynchronises. A stop is reported at the exact offset; resuming by scanning
  forward would invent instruction boundaries, which is the one thing this phase must not do.
- Both entry points are pure functions of bytes: no device, no DXC, no layer state.

---

## 3. What the decoder resolves, and how that is grounded

Encoding fields (documented GFX10 layout, applied to the word in front of us):

| Format | Encoding field | Length | Opcode extracted |
|---|---|---|---|
| SOP2 | bits 31:30 = `10`, opcode 29:23 | 1 (+1 literal) | yes |
| SOP1 | bits 31:23 = `0x17D` | 1 (+1 literal) | yes (15:8) |
| SOPC | bits 31:23 = `0x17E` | 1 (+1 literal) | yes |
| SOPP | bits 31:23 = `0x17F` | 1 | yes (22:16) |
| SOPK | bits 31:28 = `1011` | 1 | yes (27:23) |
| VOP2 | bit 31 = 0 | 1 (+1 literal/SDWA/DPP) | yes (30:25) |
| VOP1 | bits 31:25 = `0x3F` | 1 (+1 literal/SDWA/DPP) | yes (16:9) |
| VOPC | bits 31:25 = `0x3E` | 1 (+1 literal/SDWA/DPP) | yes (24:17) |
| VOP3/VOP3b | bits 31:26 = `0x34` / `0x35` | 2 | no |
| VOP3P | bits 31:26 = `0x33` | 2 | no |
| VINTRP | bits 31:26 = `0x32` | 1 | no |
| DS | bits 31:26 = `0x36` | 2 | no |
| FLAT/GLOBAL/SCRATCH | bits 31:26 = `0x37` | 2 | no |
| MUBUF | bits 31:26 = `0x38` | 2 | no |
| MTBUF | bits 31:26 = `0x3A` | 2 | no |
| SMEM | bits 31:26 = `0x3D` | 2 | no |
| EXP | bits 31:26 = `0x3E` | 2 | no |
| MIMG | bits 31:26 = `0x3C` | **not determinable** | no |

Length rules come from the published ISA statement that vector-memory, export and LDS/GDS
instructions are 64-bit, scalar ALU is 32-bit plus an optional literal dword, and 32-bit vector ALU
forms carry at most one extra dword.

**Pinned to real assembler output.** The harness does not only test the table against itself; these
words are taken from LLVM's MC tests for gfx10, quoted with their encoding lines:

```
s_mov_b32 s1, s2                    [0x02,0x03,0x81,0xbe]                    -> BE810302  SOP1 1 word  op=3
s_mov_b32 s1, 100                   [0xff,0x03,0x81,0xbe,0x64,0,0,0]        -> BE8103FF  SOP1 2 words op=3 literal
s_cmov_b32 s1, 200                  [0xff,0x05,0x81,0xbe,0xc8,0,0,0]        -> BE8105FF  SOP1 2 words op=5 literal
v_mov_b32_e32 v0, s105              [0x69,0x02,0x00,0x7e]                    -> 7E000269  VOP1 1 word  op=1
v_mov_b32_sdwa v0, s105 ...         [0xf9,0x02,0x00,0x7e,0x69,0x16,0x86,0]   -> 7E0002F9  VOP1 2 words SDWA
v_cmp_eq_f32_e64 s105, v0, s105     [0x69,0x00,0x02,0xd4,0x00,0xd3,0x00,0]   -> D4020069  VOP3 2 words
v_cmp_eq_f32_sdwa s105, v0, s105    [0xf9,0xd2,0x04,0x7c,0x00,0xe9,0x06,0x86]-> 7C04D2F9  VOPC 2 words op=2 SDWA
flat_load_dword v1, v[3:4]          [0x00,0x00,0x30,0xdc,0x03,0x00,0x7d,0x01]-> DC300000  FLAT 2 words
s_endpgm                            [0x00,0x00,0x81,0xbf]                    -> BF810000  SOPP 1 word  op=1 TERMINATOR
```

So the literal rule (`SSRC0 = 0xFF` / `SRC0 = 0xFF` selects the following dword) and the SDWA extra
dword (`SRC0 = 0xF9`) are **observed facts about real encodings**, not guesses. DPP (`SRC0 = 0xFA`)
is the documented sibling marker and is covered only by a synthetic case.

---

## 4. Observed vs inferred in this module

Every walk carries `evidence` lines prefixed `observed:` or `inferred:`.

- `observed:` is a documented ISA fact checked against the bytes (`word 0xBF810000 at byte 12 is
  SOPP opcode 1`), or a counted property of the stream (`3 instruction(s), 1 literal dword(s),
  16 of 16 bytes covered`).
- `inferred:` is where a documented rule is not enough to decide, and the walker says so instead of
  guessing. Today that is MIMG only: `inferred: word 0xF0000000 at byte 4 is MIMG; its NSA address
  words make the length opcode-dependent`.

Measurement caveat, stated in the module header and in every walk's `open_questions`: **no real XS2
payload has been decoded yet.** The table is documented and pinned to assembler output; whether a
console shader stream obeys it is exactly what the first corpus dump will settle.

---

## 5. Stop reasons

| `sc_isa_stop` | Meaning |
|---|---|
| `s_endpgm` + `synced == true` | clean walk, terminator is the last word of the region |
| `s_endpgm` + `trailing_after_endpgm` | terminator found, but bytes remain in the region (padding or a wrong end) |
| `no_terminator` | words ran out without `s_endpgm` |
| `unsupported_format` | the word's encoding field is not a GFX10 format |
| `mimg_unsupported` | MIMG: length needs the NSA count |
| `truncated` | an instruction claims a dword past the end of the region |
| `not_word_aligned` | region size is not a multiple of 4 |
| `empty` | zero bytes |

---

## 6. Measured output (Phase 4 harness)

```
region [8,24) -> synced=1 first=8 last=20 stop_offset=24
classify -> kind=rdna2_xs2 regions=1 found=1
walk [16,32) -> synced=1 instructions=3 formats={"sop1":1,"sopp":1,"vop1":1}
   observed: code region [16, 32) from the Phase 3 frontend
   observed: 3 instruction(s), 1 literal dword(s), 16 of 16 bytes covered
   observed: stream ends exactly on s_endpgm at byte 12 -- every word stepped cleanly
```

That last block is the whole seam in one place: a payload built like the Phase 3 fixture is
classified as `rdna2_xs2`, its code region is taken from `c.xs2.code_regions[0]`, and the decoder
steps the region to the terminator with no bytes left over. Encoder-level coverage in the same run:

```
literal + vop1 + endpgm  synced=1 instructions=3 literal_words=1 formats={"sop1":1,"sopp":1,"vop1":1}
sdwa extra dword         synced=1 instructions=2 formats={"sopp":1,"vop1":1}
flat 64-bit              synced=1 instructions=2 formats={"sopp":1,"flat":1}
two vop2 then endpgm     synced=1 instructions=3 formats={"sopp":1,"vop2":2}
dpp extra dword          synced=1 instructions=2 formats={"sopp":1,"vop1":1}
no terminator            -> stop=no_terminator
trailing after endpgm    -> stop=trailing_after_endpgm at 4
mimg                     -> stop=mimg_unsupported at 4 (instructions=1)
unknown word             -> stop=unsupported_format
truncated 64-bit         -> stop=truncated
literal without its dword-> stop=truncated
unaligned                -> stop=not_word_aligned ; empty -> stop=empty
126 checks, 0 failed
```

---

## 7. Findings from building it

### Finding 1 — SOPK hides inside SOP2's opcode bits

SOPK's 4-bit encoding is `1011`, which sits inside SOP2's 7-bit opcode space (`10` + opcode). The
first version classified `0xB0000000` as SOP2, because SOP2 was chosen as the fallback for the whole
scalar family. The assembler-pinned table caught it immediately:

```
FAIL inst.format == b.format
   sopk     0xb0000000 -> sop2     words=1
```

The fix is ordering: 9-bit SOP1/SOPC/SOPP patterns first, then the 4-bit `1011` SOPK pattern, then
SOP2. Real SOP2 opcodes stay below `0x60`, so their bits 29:28 are zero and the patterns cannot
overlap. That is now asserted by the bucket test rather than assumed.

### Finding 2 — the VOPC opcode field was confirmed by arithmetic, not by eye

The harness first expected `v_cmp_eq_f32_sdwa` to carry opcode `0x52`; decoding bits 24:17 of
`0x7C04D2F9` gives **2**, which is `v_cmp_eq_f32` in the published VOPC table. The extraction was
right and the expectation was wrong — a useful reminder that the fixtures must be derived from the
ISA table, not from memory. The harness now asserts 2.

### Finding 3 — the literal dword is not an instruction

The first version of the walk test expected four instructions for `s_mov_b32 s1,100; v_mov_b32;
s_endpgm`. The literal is a dword the preceding instruction owns, so the count is three and the
second instruction starts at offset 8, not 4. The regression is now explicit (`instructions.size()
== 3`, `instructions[0].words == 2`, `instructions[0].dwords[1] == 0x64`, `instructions[1].offset ==
8`), because an off-by-one here is exactly how a stream desynchronises in the field.

---

## 8. What is deliberately not done

- **MIMG length.** Documented: 64-bit base plus one dword per extra NSA address component. The count
  needs the image opcode's dimension, which this phase does not decode, so MIMG stops the walk.
  Completing it means a `uimm`-dimension table for the image ops, not a guess.
- **VOP3/VOP3P/VINTRP and memory/image opcodes.** Only format and length are decoded for those; their
  opcode field positions were not verified against assembler output in this pass, so `opcode_known`
  is false rather than plausible-looking.
- **`S_SETREG_IMM32_B32`.** Documented as a 64-bit SOPK form; walked as 32-bit here. A stream that
  contains it will desync, and the walk will say at which offset. Recorded as an open question.
- **No lifting.** Nothing converts instructions to DXIL. The emitter seam from Phase 2 is untouched.
- **No wave64 assumptions encoded.** Operand widths and wave semantics are not decoded at all yet.
- **No real corpus.** The first dump is the test that matters.

---

## 9. The `F:\ScarlettCompat` copies are stale

Unchanged from Phase 3 §6: that tree's `sc_container.h/.cpp` and `sc_test_main.cpp` predate both the
token correction and the Phase 3 `DXIL::ShaderKind` fix, and the Phase 2, Phase 3 and now Phase 4
files are absent. Re-copy `include/shader/`, `src/shader/` and `tests/shader/` wholesale; this agent
does not touch `F:\ScarlettCompat`.

---

## 10. Verification

```bat
cd /d D:\ScarlettCompat-shader
tests\shader\build_shader_tests.bat    :: Phase 0: 125 checks, 0 failed
tests\shader\build_triangle_test.bat   :: Phase 2:  35 checks, 0 failed (real GPU)
tests\shader\build_frontend_test.bat   :: Phase 3: 798 checks, 0 failed (full)
                                       ::          722 checks, 0 failed (no-DXC build)
tests\shader\build_isa_test.bat        :: Phase 4: 126 checks, 0 failed (no emitter linked)
```
