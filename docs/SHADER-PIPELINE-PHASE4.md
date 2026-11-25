# Shader pipeline — Phase 4 handoff (RDNA2 instruction decode)

Status: **Phase 4 is built and verified.** The Phase 3 frontend's code regions can be stepped as
GFX10 instruction streams: each word is classified into its instruction format, its length is
resolved (32/64-bit plus the extra dword a literal/SDWA/DPP form appends), every documented shader
terminator is recognised, and the walk stops on a terminator or on the first word it cannot justify —
with the offset and the reason. Nothing is wired into the boot path, nothing was built into the
layer, no boot was run, nothing was committed or pushed, and `F:\ScarlettCompat` was not read,
written, built or run in.

- Tree: `D:\ScarlettCompat-shader` only.
- Suites: Phase 0 **125/125**, Phase 2 **35/35**, Phase 3 full **804/804**, Phase 3 no-DXC **728/728**,
  Phase 4 **236/236** — all exit 0.

---

## 1. New files (Phase 4)

| File | Purpose |
|---|---|
| `include/shader/sc_isa.h` / `src/shader/sc_isa.cpp` | The decoder: encoding table, length rules, terminator table, stream walker, region walker |
| `tests/shader/sc_isa_main.cpp` | Phase 4 harness (assembler-pinned table, terminator forms, every format bucket, every stop reason, frontend seam) |
| `tests/shader/build_isa_test.bat` | Builds and runs one binary **without** `sc_dxil_emit.cpp` linked |
| `docs/SHADER-PIPELINE-PHASE4.md` | This document |

Existing-file changes in this phase, stated plainly: `src/shader/sc_xs2.cpp` now takes its
terminator table from `sc_isa.h` instead of hard-coding `0xBF810000` (so the three documented forms
are recognised in one place), `tests/shader/sc_frontend_main.cpp` gained cases for the two additional
forms, and `tests/shader/build_frontend_test.bat` links `sc_isa.cpp`. No upstream repository file was
touched.

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
        // w.terminator      -> which documented form ended the stream.
    }
}
```

Notes that matter for wiring:

- **This is triage, not a gate.** Nothing here should decide whether a PSO is created.
- `sc_isa_walk_region` bounds-checks `begin`/`end` against the blob before reading; an out-of-range
  region returns a `truncated` walk with an evidence line instead of touching memory.
- The walker never resynchronises. A stop is reported at the exact offset; resuming by scanning
  forward would invent instruction boundaries.
- A stop is not a total loss: `bytes_covered` and `instructions` describe the verified prefix, which
  is what makes a stop useful for triage.
- Both entry points are pure functions of bytes: no device, no DXC, no layer state.

---

## 3. What the decoder resolves, and how that is grounded

Encoding fields (documented GFX10 layout, applied to the word in front of us):

| Format | Encoding field | Length | Opcode extracted |
|---|---|---|---|
| SOP2 | bits 31:30 = `10`, opcode 29:23 | 1 (+1 literal) | yes |
| SOP1 | bits 31:23 = `0x17D` | 1 (+1 literal) | yes (15:8) |
| SOPC | bits 31:23 = `0x17E` | 1 (+1 literal) | yes |
| SOPP | bits 31:23 = `0x17F` | 1 (§3.4) | yes (22:16) |
| SOPK | bits 31:28 = `1011` | 1, or 2 for `S_SETREG_IMM32_B32` (§3.3) | yes (27:23) |
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
| MIMG | bits 31:26 = `0x3C` | **not determinable** (§7) | no |

### 3.1 Terminators

A shader can end with any of three documented SOPP instructions. Opcode numbers are from AMD's
machine-readable RDNA2 ISA; the SOPP layout puts the opcode at bits 22:16 over the `0xBF800000`
base, which gives the whole words below.

| Instruction | Opcode | Word | Recognised as |
|---|---|---|---|
| `s_endpgm` | 1 | `0xBF810000` | terminator (assembler-verified) |
| `s_endpgm_saved` | 27 | `0xBF9B0000` | terminator |
| `s_endpgm_ordered_ps_done` | 30 | `0xBF9E0000` | terminator |
| `s_code_end` | 31 | `0xBF9F0000` | **not** a terminator — a code marker; a stream ending in it must report `no_terminator` |

`s_code_end` is the one that could quietly corrupt a walk: it looks like a sibling of the endpgm
family and sits at the end of code in some toolchains. Accepting it as a terminator would let a
truncated shader look complete, so the harness asserts the opposite. The table lives in
`sc_isa.h` (`sc_isa_is_terminator_word`) and `sc_xs2.cpp` scans with it, so the prober and the
walker cannot drift.

### 3.2 Pinned to real assembler output

The harness does not only test the table against itself; these words are taken from LLVM's MC tests
for gfx10, quoted with their encoding lines:

```
s_mov_b32 s1, s2                    [0x02,0x03,0x81,0xbe]                    -> BE810302  SOP1 1 word  op=3
s_mov_b32 s1, 100                   [0xff,0x03,0x81,0xbe,0x64,0,0,0]        -> BE8103FF  SOP1 2 words op=3 literal
s_cmov_b32 s1, 200                  [0xff,0x05,0x81,0xbe,0xc8,0,0,0]        -> BE8105FF  SOP1 2 words op=5 literal
v_mov_b32_e32 v0, s105              [0x69,0x02,0x00,0x7e]                    -> 7E000269  VOP1 1 word  op=1
v_mov_b32_sdwa v0, s105 ...         [0xf9,0x02,0x00,0x7e,0x69,0x16,0x86,0]   -> 7E0002F9  VOP1 2 words SDWA
v_cmp_eq_f32_e64 s105, v0, s105     [0x69,0x00,0x02,0xd4,0x00,0xd3,0x00,0]   -> D4020069  VOP3 2 words
v_cmp_eq_f32_sdwa s105, v0, s105    [0xf9,0xd2,0x04,0x7c,0x00,0xe9,0x06,0x86]-> 7C04D2F9  VOPC 2 words op=2 SDWA
flat_load_dword v1, v[3:4]          [0x00,0x00,0x30,0xdc,0x03,0x00,0x7d,0x01]-> DC300000  FLAT 2 words
s_setreg_imm32_b32 hwreg(MODE,0,2), 3 [0x01,0x08,0x80,0xba,0x03,0,0,0]        -> BA800801  SOPK 2 words op=0x15 imm32
s_endpgm                            [0x00,0x00,0x81,0xbf]                    -> BF810000  SOPP 1 word  op=1 TERMINATOR
```

So the literal rule (`SSRC0 = 0xFF` / `SRC0 = 0xFF` selects the following dword) and the SDWA extra
dword (`SRC0 = 0xF9`) are **observed facts about real encodings**. DPP (`SRC0 = 0xFA`) is the
documented sibling marker and is covered only by a synthetic case; the two extra terminator forms
are documented opcode numbers, not assembler-verified words.

### 3.3 `S_SETREG_IMM32_B32` — verified as the one 64-bit SOPK form, and stepped

SOPK is otherwise a pure 32-bit format, so a walker that assumes "SOPK is one dword" desynchronises
on the first `S_SETREG_IMM32_B32` and misreports everything after it. This one can be settled from
the sources rather than merely stopped:

- **Field layout** (LLVM `SOPInstructions.td`, `SOPK_Real64`): `Inst{15-0} = simm16` (the `hwreg`),
  `Inst{22-16}` unused (`has_sdst = 0`), `Inst{27-23} = op`, `Inst{31-28} = 0xb`, and
  `Inst{63-32} = imm`. The pseudo declares `let Size = 8; // Unlike every other SOPK instruction.`
- **Opcode number on GFX10**: `S_SETREG_IMM32_B32 : SOPK_Real64_gfx6_gfx7_gfx10_gfx13<0x015>` — so
  0x15 for gfx10, with `S_GETREG_B32` at 0x12 and `S_SETREG_B32` at 0x13 alongside it. GFX9 uses
  0x14 and GFX11+ 0x13, which is exactly why the opcode must come from the GFX10 table and not from
  memory or a neighbouring ISA.
- **Assembler bytes** (gfx1010 check lines, `-show-mc-encoding`):
  `s_setreg_imm32_b32 hwreg(HW_REG_MODE, 0, 2), 3` → `[0x01,0x08,0x80,0xba,0x03,0x00,0x00,0x00]`,
  i.e. word 0 `0xBA800801` (opcode 0x15 decoded from bits 27:23) and word 1 `0x00000003`, the
  immediate. The `hwreg(HW_REG_MODE, 0, 2)` operand is visible in word 0 as `0x0801` (id 1, width 2).

So the decoder now reports `words = 2`, `extra = imm32`, `opcode = 0x15`, and the walker stores the
second dword in `dwords[1]` and continues from the following word. Nothing is inferred here; the
stop machinery is not involved. Two guards keep it honest: a `S_SETREG_IMM32_B32` as the last word of
a region is a `truncated` stop at its offset (not a one-word step), and the GFX9-numbered word
(`0xBA000801`, opcode 0x14) is *not* treated as the GFX10 form — decoding it as a plain 32-bit SOPK
is correct for a GFX10 stream, and the harness states that explicitly so the numbering difference is
visible rather than discovered later.

### 3.4 SOPP's 64-bit relaxation form — audited, and deliberately walked as one dword

LLVM has a second SOPP encoding for branch relaxation: `SOPP_Real_64`, selected by the
`SOPP_Real_With_Relaxation*` multiclasses that define the `_pad_s_nop` variants
(`isCodeGenOnly = 1`, so no assembler mnemonic exists for them). Its field assignment is decisive:

```
let Inst{15-0}  = simm16;   // same as the 32-bit form
let Inst{22-16} = op;
let Inst{31-23} = 0x17f;
//effectively a nop
let Inst{47-32} = 0x0;
let Inst{54-48} = 0x0;
let Inst{63-55} = 0x17f;
```

Bits 63:55 = 0x17F with 54:32 clear is `0xBF800000`, and that word is a real assembler vector in its
own right: `s_nop 0` → `[0x00,0x00,0x80,0xbf]` (`llvm/test/MC/AMDGPU/sopp.s`). So the 64-bit form
is *the 32-bit instruction followed by the encoding of `s_nop 0`* — which is what LLVM's
"effectively a nop" comment means.

Why no length rule was added:

- **Stepping one dword is exact.** The pad word decodes on its own as SOPP opcode 0 (s_nop, one
dword), so the walk consumes both words and lands on the next instruction at the same offset the
64-bit reading would. Pinned by fixtures: `s_branch 2; pad; s_endpgm` walks to the terminator in
three instructions with offsets 0/4/8 and full coverage, and `s_branch 2; pad` alone ends in
`no_terminator` after consuming both words — not in a desync.
- **"Two dwords when the next word is 0xBF800000" would be an invented distinction.** The bytes of a
relaxed branch and of `s_branch; s_nop 0` are identical, so such a rule would relabel every genuine
nop after a branch as padding. Here the bytes decide the question, and the answer is one dword; the
decision is recorded in the decoder comment and in the walk's `open_questions`.
- **No terminator has a 64-bit form.** On GFX10, `S_ENDPGM`, `S_ENDPGM_SAVED` and
`S_ENDPGM_ORDERED_PS_DONE` are all instantiated from `SOPP_Real_32`; the relaxation groups cover
branch instructions only. If padding ever does follow a terminator, the walk reports
`trailing_after_terminator` instead of swallowing it (fixture-pinned).
- **Cost:** a relaxed branch appears as two instructions with one `s_nop` in the histogram. That is
the same stream the hardware executes; no offset is lost.

What would change this: the padding dword acquiring a nonzero immediate, or a terminator gaining a
64-bit form. Either would make the second dword carry information rather than being a no-op.

---

## 4. Observed vs inferred in this module

Every walk carries `evidence` lines prefixed `observed:` or `inferred:`.

- `observed:` is a documented ISA fact checked against the bytes (`word 0xBF810000 at byte 12 is
  SOPP opcode 1`), or a counted property of the stream (`3 instruction(s), 1 literal dword(s),
  16 of 16 bytes covered`).
- `inferred:` is where a documented rule is not enough to decide, and the walker says so instead of
  guessing. Today that is MIMG (§7).

Measurement caveat, stated in the module header and in every walk's `open_questions`: **no real XS2
payload has been decoded yet.** The table is documented and pinned to assembler output; whether a
console shader stream obeys it is exactly what the first corpus dump will settle.

---

## 5. Stop reasons

| `sc_isa_stop` | Meaning |
|---|---|
| `terminator` + `synced == true` | clean walk; `terminator` names which form ended it |
| `terminator` + `trailing_after_terminator` | terminator found, but bytes remain in the region (padding or a wrong end) |
| `no_terminator` | words ran out without a documented terminator (`s_code_end` does not count) |
| `unsupported_format` | the word's encoding field is not a GFX10 format |
| `mimg_unsupported` | MIMG: length needs the NSA count (§7) |
| `truncated` | an instruction claims a dword past the end of the region |
| `not_word_aligned` | region size is not a multiple of 4 |
| `empty` | zero bytes |

---

## 6. Measured output (Phase 4 harness)

```
s_endpgm                               0xbf810000 -> opcode=1  terminator=s_endpgm
s_endpgm_saved                         0xbf9b0000 -> opcode=27 terminator=s_endpgm_saved
s_endpgm_ordered_ps_done               0xbf9e0000 -> opcode=30 terminator=s_endpgm_ordered_ps_done
s_code_end (marker, not a terminator)  0xbf9f0000 -> opcode=31 terminator=none
s_branch (SOPP opcode 2)               0xbf820000 -> opcode=2  terminator=none
walk ending in s_endpgm                 synced=1 bytes_covered=8
walk ending in s_endpgm_saved           synced=1 bytes_covered=8
walk ending in s_endpgm_ordered_ps_done synced=1 bytes_covered=8
0xba800801 -> sopk opcode=0x15 words=2 extra=imm32
0xb7020006 -> s_addk_i32 opcode=0xe words=1 (unchanged)
walk through it -> synced=1 instructions=3 bytes_covered=16 next_offset=12
missing immediate -> stop=truncated at 0
0xba000801 -> SOPK opcode=0x14 words=1 (GFX9 numbering, not stepped as the GFX10 form)
0xbf800000 == 0x17F<<23 -> sopp opcode=0 (s_nop) words=1 terminator=none
branch + pad + endpgm -> synced=1 offsets=0,4,8
branch + pad alone -> stop=no_terminator, 2 instructions, 8 bytes (both words consumed)
vop1 + s_nop 0 + endpgm -> synced=1 instructions=3 (same reading)
endpgm + pad -> stop=trailing_after_terminator at 4 (padding is reported, never swallowed)
s_code_end last -> stop=no_terminator (correctly not a terminator)
mimg -> stop=mimg_unsupported at 4 (verified prefix 4 bytes, 1 instruction)
   observed: 1 instruction(s), 0 literal dword(s), 4 of 12 bytes covered
   inferred: word 0xf0000000 at byte 4 is MIMG; stopping here rather than assuming an NSA address count
   inferred: MIMG length is 2 dwords plus one per extra non-sequential address component; the count's bit
             position is unverified, and real address dwords (small VGPR numbers) decode as VOP2, so a
             length search would not be conclusive
region [8,24) -> synced=1 first=8 last=20 stop_offset=24
classify (term 0xbf810000) -> kind=rdna2_xs2 regions=1 found=1
walk [16,32) -> synced=1 instructions=3 terminator=s_endpgm
   observed: code region [16, 32) from the Phase 3 frontend
   observed: 3 instruction(s), 1 literal dword(s), 16 of 16 bytes covered
   observed: stream ends exactly on s_endpgm at byte 12 -- every word stepped cleanly
classify (term 0xbf9b0000) -> kind=rdna2_xs2 regions=1 found=1
walk [16,32) -> synced=1 instructions=3 terminator=s_endpgm_saved
classify (term 0xbf9e0000) -> kind=rdna2_xs2 regions=1 found=1
walk [16,32) -> synced=1 instructions=3 terminator=s_endpgm_ordered_ps_done
236 checks, 0 failed
```

The seam block is the whole pipeline in one place: a payload shaped like the Phase 3 fixture is
classified as `rdna2_xs2`, its code region comes from `c.xs2.code_regions[0]`, and the decoder steps
the region to the terminator for each accepted form with no bytes left over.

---

## 7. The MIMG settlement

**Decision: Phase 4 does not step MIMG. It stops on the word, reports the offset and the raw dword,
and keeps the verified prefix.** This is deliberate, not unfinished work, and it is the honest option
until one of two things is verified:

1. **the bit position of the instruction's `nsa` field.** GFX10 has separate non-NSA and NSA MIMG
   encodings; the NSA form appends one dword per extra non-sequential address component, and the
   count (1–3, mapped from the number of address components) is in the instruction. LLVM's
   `MIMGInstructions.td` declares the field (`let nsa = nsah.NSA;`) but the generated `Inst{}` bit
   assignment was not readable from the sources checked here, so its position is unverified; or
2. **the per-opcode address-component count**, so the length can be derived from the image opcode and
   dimension (`VAddrDwords` in LLVM's MIMG tables). That is a table this phase will not guess.

**The alternative that was considered and rejected: pick the length.** A suffix search over the four
ISA-legal lengths (2–5 dwords) sounds attractive — accept the only length under which the rest of the
region still decodes to a terminator — but it is a coin flip on real data: an NSA address dword is a
small VGPR number, which has bit 31 clear and therefore decodes as a perfectly valid VOP2, so several
candidate lengths survive. The walk would either stop anyway or pick wrong with an `inferred:` label,
and a mis-picked length silently resynchronises the rest of the shader. Trading an honest stop for a
labelled guess is exactly what this pipeline refuses to do.

**What this costs:** a shader whose first texture instruction is early will not be walked to its end.
What it gives instead is a precise fact — "these bytes up to offset N are instructions, this word is
MIMG, and here is why I stopped" — which is the input needed to settle the NSA field from real
payloads.

**What would change the decision:** the corpus contains MIMG words, and the region around them is
short enough to test candidate lengths by hand; or a readable `Inst{}` assignment for `nsa` is found
in the ISA XML/toolchain sources. Either makes the step exact rather than statistical.

The module records all of this itself: the stop carries two `inferred:` evidence lines naming the
condition, and `open_questions` repeats that MIMG is not stepped.

---

## 8. Findings from building it

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

### Finding 4 — one terminator is not enough, and one too many is worse

A walker that only ends on `0xBF810000` would read a pixel shader ending in
`s_endpgm_ordered_ps_done` as "no terminator", and the same shader would never have produced a code
region at all, because the Phase 3 prober scanned for that single constant. Both now share the
terminator table in `sc_isa.h`. The failure mode in the other direction is worse: `s_code_end` sits
next to the endpgm family, ends some toolchains' code, and does **not** terminate a wave — accepting
it would make a truncated shader look complete, so the harness pins it as `no_terminator`.

---

### Finding 5 — the same instruction has three opcode numbers across the ISA family

`S_SETREG_IMM32_B32` is SOPK 0x14 on GFX9, 0x15 on GFX10 and 0x13 on GFX11+. Taking the number from
the wrong generation would have produced a decoder that steps the wrong word length *and* labels the
instruction wrongly, on the exact GPU family this pipeline targets. The GFX10 table entry
(`SOPK_Real64_gfx6_gfx7_gfx10_gfx13<0x015>`) and the gfx1010 assembler bytes agree, and the harness
now pins both the GFX10 word and the GFX9-numbered word so the distinction stays visible.

---

### Finding 6 — the "multi-dword" form whose second dword is a legal instruction

The 64-bit SOPP relaxation encoding is multi-dword in the encoding table but not in the byte
stream: its padding half is `s_nop 0`, so it is indistinguishable from a separate instruction and
costs nothing to step separately. That is the inverse of the MIMG and `S_SETREG_IMM32_B32` cases —
there the extra dword was *data*, and a wrong length either desynchronised the walk or mislabelled
the instruction; here the extra dword is a well-formed instruction and the short reading is safe.
The rule recorded in the decoder: a documented 64-bit encoding is only a length rule if its second
dword cannot be a valid instruction on its own.

---

## 9. What is deliberately not done

- **MIMG stepping.** See §7: it needs the `nsa` field position or the opcode address-count table.
- **Distinguishing a relaxed 64-bit SOPP from `instruction; s_nop 0`.** Not possible from bytes and
  not attempted; both readings give the same next offset.
- **VOP3/VOP3P/VINTRP and memory/image opcodes.** Only format and length are decoded for those; their
  opcode field positions were not verified against assembler output in this pass, so `opcode_known`
  is false rather than plausible-looking.
- **No lifting.** Nothing converts instructions to DXIL. The emitter seam from Phase 2 is untouched.
- **No wave64 assumptions encoded.** Operand widths and wave semantics are not decoded at all yet.
- **No real corpus.** The first dump is the test that matters.

---

## 10. The `F:\ScarlettCompat` copies are stale

Unchanged from Phase 3 §6: that tree's container reader and Phase 0 harness predate both the token
correction and the Phase 3 `DXIL::ShaderKind` fix, and the Phase 2, Phase 3 and Phase 4 files are
absent. Re-copy `include/shader/`, `src/shader/` and `tests/shader/` wholesale; this agent does not
touch `F:\ScarlettCompat`.

---

## 11. Verification

```bat
cd /d D:\ScarlettCompat-shader
tests\shader\build_shader_tests.bat    :: Phase 0: 125 checks, 0 failed
tests\shader\build_triangle_test.bat   :: Phase 2:  35 checks, 0 failed (real GPU)
tests\shader\build_frontend_test.bat   :: Phase 3: 804 checks, 0 failed (full)
                                       ::         728 checks, 0 failed (no-DXC build)
tests\shader\build_isa_test.bat        :: Phase 4: 236 checks, 0 failed (no emitter linked)
```
