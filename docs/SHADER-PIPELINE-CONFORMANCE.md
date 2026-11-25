# Shader pipeline — GFX10 decode conformance matrix

Status: **built and green, all 37,905 harvested fixtures pass.** 37,905 assembler-pinned GFX10
encodings decode and match the mnemonic `llvm-mc` printed for the same bytes: **0 failures**, with
**91,179** operand tokens classified against the printed text. Decoded and asserted: VOP3 operands
and modifiers (14,576 fixtures), SOP2/SOPC (1,998), SOPK (206) and SOPP (113), the VOP1/VOP2/VOPC
forms including their SDWA and DPP modifier forms (with the four SDWA selectors, `clamp` and
`omod` compared against the printed attributes: **22,472 attribute checks, all matching**; and the
DPP control word - quad/row control, both masks, `fi`, `bound_ctrl`, DPP16 `neg`/`abs`: **13,955
control checks, all matching**), the named scalar special registers, the scalar inline constants,
the literal dwords and the VINTERP attribute/param selectors. The memory families (DS, FLAT, MUBUF,
SMEM, MIMG, EXP: 3,542 fixtures) now have their format, their length (MIMG's included: `2 + dim`
at bits 2:1) and their **opcode field** asserted, plus the nine measured memory operand fields. VOP3P now has 7 fixtures (`v_dot2/dot4/dot8`
at `0x33`), all printed with the same `v0, v1, v2, v3` operands, so neither its opcode field nor its
operand layout is claimed.

The matrix is also a bug finder: it caught four real decoder defects and five harness mistakes
while being built (listed below). Nothing is wired into the boot path, nothing was built into
the layer, no boot was run, nothing was committed.

- Tree: `D:\ScarlettCompat-shader` only.
- New files:
  - `tests/shader/fixtures/harvest_gfx10_mc.py` — harvester (generates the fixture TSV).
  - `tests/shader/fixtures/gfx10_mc_fixtures.tsv` — generated fixtures (37,905 rows + provenance).
  - `tests/shader/fixtures/README.md` — provenance, licence, regeneration recipe.
  - `tests/shader/sc_conformance_main.cpp`, `tests/shader/build_conformance_test.bat` — the harness.

---

## 1. What the fixtures are (and are not)

The input is LLVM's MC asm tests for RDNA1: `llvm/test/MC/AMDGPU/gfx10_asm_*.s`, licence
**Apache-2.0 WITH LLVM-exception**. Each row holds an instruction pair produced by a real assembler:

```text
v_mov_b32_e32 v5, s103
// GFX10: v_mov_b32_e32 v5, s103     ; encoding: [0x67,0x02,0x0a,0x7e]
```

or, in the `vopc`/`dpp` files, the encoding on the line after the instruction. Both forms are
harvested. This is **synthetic ground truth**: open-source assembler output for RDNA1. No byte here
was captured from a game, and nothing here says anything about GTA 6's shaders.

All 38 `gfx10*.s` files are harvested, including the memory families and the chip-specific
files. Two rules keep the set honest:

- **Check prefixes are filtered.** Several files carry assertions for more than one GPU
  (`gfx10-constant-bus.s` has `GFX10:` and `GFX11:` lines for the same source, and gfx11 numbers
  some opcodes differently). Only `GCN`, `W*` and `GFX10*` lines are kept; `GFX11`/`GFX12`/`GFX9`
  lines are dropped rather than mixed into an RDNA1/2 set.
- **`;`-comment assertions are read too.** The image tests write `; GFX10: ... ; encoding: [...]`
  instead of `//`. Without that, all 233 MIMG rows were invisible.

The error tests (`*_ERR` labels) still contribute only where `llvm-mc` emitted an `encoding:` line.

| family | fixtures | | family | fixtures |
|---|---|---|---|---|
| vop1 | 5,125 | | ds | 1,865 |
| vop2 | 4,077 | | mubuf | 771 |
| vopc | 2,892 | | smem | 459 |
| vopcx | 5,323 | | mimg | 233 |
| vopc_sdwa | 3,452 | | flat | 134 |
| vopc_e64 | 5,421 | | exp | 24 |
| vop3 | 4,249 | | 30_new (gfx1030) | 68 |
| sop | 3,352 | | 11_dlops (gfx1011) | 17 |
| dpp8 | 189 | | 13 (gfx1013) | 16 |
| dpp16 | 228 | | constant-bus | 8 |
| | | | vop2be-literal | 2 |
| **total** | **37,905** (977 duplicate encodings dropped) | | | |

## 2. What is asserted, and against what

Five independent checks per fixture, all compared to the mnemonic text:

1. **Format.** The source file's family is the independent signal: a word from
   `gfx10_asm_vop2.s` must decode as `vop2`. Two refinements are read from the fixture itself
   rather than assumed: an `_e64` mnemonic inside an e32 file is a VOP3 encoding
   (`v_mov_b32_e64` lives in `gfx10_asm_vop1.s`), and mixed files (`sop.s`, `dpp8/16.s`) state an
   allowed family set.
2. **Length.** The dwords the fixture encodes must equal `sc_isa_length()`, which already
   includes the literal/SDWA/DPP/imm32 dword — including the VOP3 literal dword, whose selector
   sits in the *second* dword and so cannot be decided from dword 0 alone.
3. **Opcode.** Every (format, mnemonic) pair must resolve to exactly one opcode value across all its
   fixtures, and three opcodes pinned by hand earlier are asserted outright
   (`v_mov_b32` = 1, `v_cvt_f32_i32` = 5, `s_mov_b64` = 4). **1,257 pairs resolved, 48 assertions applied.**
4. **Operands.** For every format the module decodes (`VOP1`, `VOP2`, `VOPC`, `VOP3`, `SOP1`,
   `SOP2`, `SOPC`, `SOPK`, `SOPP`), each token the mnemonic prints must classify to the same thing:
   register file and index, named special register, inline-constant or literal value, VINTERP
   attribute/param, or the `hwreg(id, offset, size)` triple: **91,088 operand comparisons, all
   matching**. Nothing is left in a "counted, not decoded" bucket.
5. **VOP3 modifiers.** For VOP3 forms that carry modifier bits, the printed `-vN` / `|vN|` must match
   the decoded `neg`/`abs` for that source slot, `clamp` must match word0 bit 15, `mul:2`/`mul:4`/
   `div:2` must match `omod`, and a printed `op_sel:[…]` must match the decoded `op_sel` bits.
   `v_interp_*` forms are checked against their attribute operand instead. The SDWA forms carry the
   same kind of check from their own modifier dword: printed `-vN` / `|vN|` / `sext(vN)` per source,
   `clamp`, `omod`, and the four data-format selectors (`dst_sel`, `dst_unused`, `src0_sel`,
   `src1_sel`) mapped from their printed `BYTE_`/`WORD_`/`DWORD`/`UNUSED_` names:
   **22,472 attribute checks, all matching**. The DPP forms get the same treatment from their
   control word (`quad_perm` lanes, `row_*` op/amount, `row_mask`, `bank_mask`, `fi`,
   `bound_ctrl`, `-vN`/`|vN|`): **13,919 control checks, all matching**.

## 3. Measured pass rate per format (this run)

| format | fixtures | fmt ok | len ok | passed | op checks | op ok | rate |
|---|---|---|---|---|---|---|---|
| ds | 1865 | 1865 | 1865 | 1865 | 1630 | 1630 | 100.0% |
| exp | 24 | 24 | 24 | 24 | 0 | 0 | 100.0% |
| flat | 153 | 153 | 153 | 153 | 0 | 0 | 100.0% |
| mimg | 260 | 260 | 260 | 260 | 674 | 674 | 100.0% |
| mubuf | 781 | 781 | 781 | 781 | 1558 | 1558 | 100.0% |
| smem | 459 | 459 | 459 | 459 | 417 | 417 | 100.0% |
| sop1 | 1057 | 1057 | 1057 | 1057 | 2104 | 2104 | 100.0% |
| sop2 | 1656 | 1656 | 1656 | 1656 | 4968 | 4968 | 100.0% |
| sopc | 342 | 342 | 342 | 342 | 684 | 684 | 100.0% |
| sopk | 206 | 206 | 206 | 206 | 407 | 407 | 100.0% |
| sopp | 113 | 113 | 113 | 113 | 72 | 72 | 100.0% |
| vop1 | 4118 | 4118 | 4118 | 4118 | 8230 | 8230 | 100.0% |
| vop2 | 2986 | 2986 | 2986 | 2986 | 9018 | 9018 | 100.0% |
| vop3 | 14576 | 14576 | 14576 | 14576 | 43640 | 43640 | 100.0% |
| vop3p | 7 | 7 | 7 | 7 | 0 | 0 | 100.0% |
| vopc | 9302 | 9302 | 9302 | 9302 | 22056 | 22056 | 100.0% |
| vop3p | 7 | 7 | 7 | 0 | 0 | 7 | 100.0% |
| **total** | **37,905** | **37,905** | **37,645** | **91,179** | **91,179** | **37,905** | **100.0%** |
In this per-format table `op checks` counts register roles compared against a printed token
(so the two word-only MIMG rows are not included); MIMG dmask and the NSA address list are
accounted separately in the per-field table above.


The full report (including the work queue) is written to `out/shader_tests/gfx10_conformance.md`
on every run.

## 4. Verified field layouts

Every row below was pinned by fixtures that vary that field and nothing else; none of it is from
memory. `encoding` is word0 bits 31:26 = `0x35` for every one of the 14,564 fixtures (`0x34` and
`0x33` appear zero times in the harvested files).

| dword | bits | field |
|---|---|---|
| 0 | 7:0 | `vdst` (VGPR, or SGPR on the forms listed below) |
| 0 | 10:8 | `abs[0..2]` |
| 0 | 14:11 | `op_sel[0..3]` |
| 0 | 15 | `clamp` |
| 0 | 25:16 | opcode (10 bits) |
| 1 | 8:0 | `src0` |
| 1 | 17:9 | `src1` |
| 1 | 26:18 | `src2` |
| 1 | 28:27 | `omod` |
| 1 | 31:29 | `neg[0..2]` |

Evidence, one line per field:

- `vdst`: `v_add_f32_e64 v5, v1, v2` = `0xD5030005` → 5 in bits 7:0.
- `abs`: `|v1|` sets bit 8, `|v1|,|v2|` sets bits 8 and 9 (`0xD5030105` / `0xD5030305`).
- `clamp`: `v_add_f32_e64 v5, v1, v2 clamp` = `0xD5038005` → bit 15.
- `omod`: `mul:2` = `0x0a020501`, `mul:4` = `0x12020501`, `div:2` = `0x1a020501` → bits 28:27 = 1, 2, 3.
- `neg`: `-v1` flips bit 29, `-v2` flips bit 30, both flip both (word1 `0x20000000` / `0x40000000`).
- `op_sel`: 4-bit forms (align\*) move bits 11,12,13,14 one at a time (`op_sel:[1,0,0,0]` … `[1,1,1,1]`
  on `v_alignbit_b32`); 16-bit forms print three values, and the third is the destination half at
  bit 14 (`v_add_nc_u16 v5, v1, v2 op_sel:[0,0,1]` = `0xd7034005`). `v_permlane16_b32` prints two.
  Shapes measured over all 105 `op_sel` fixtures: `{3 sources, 4 values}: 71`, `{2 sources, 3}: 28`,
  `{3 sources, 2}: 6`.
- `src0/1/2`: the same 9-bit convention as the 32-bit forms (SGPR `0..105`, VGPR `256+n`, `0xFF`
  literal), which is why the same classifier is reused.
- **Literal dword.** A VOP3 instruction owns one extra dword when *any* of its three 9-bit source
  fields is `0xFF`. Two fixtures mark two slots at once and still append a single dword
  (`v_permlane16_b32 v5, v1, 0x12345678, 0x12345678` = `0xd7770005 0x03fdff01 0x12345678`).
- **Opcode space.** `< 0x100` is the compare space (VOPC encodings), `0x100..0x17f` the VOP2-derived
  forms, `>= 0x180` the VOP1-derived forms; 426 mnemonics, no mnemonic maps to two values.
- **Scalar destination.** The compare space puts the scalar destination in bits 7:0 with no vector
  destination (`v_cmp_eq_f32_e64 s105, v0, s105` = `0xD4020069`; `v_cmpx` prints no destination at
  all). Nine opcodes put a scalar destination in bits **15:8** while keeping a vector destination in
  7:0: `0x128/0x129/0x12a/0x30f/0x310/0x319` (`v_add/sub/subrev_co_ci_u32`, `v_add/sub/subrev_co_u32`)
  plus `0x16d` (`v_div_scale_f32`), `0x176` (`v_mad_u64_u32`), `0x177` (`v_mad_i64_i32`) — the index
  in bits 15:8 matched the printed register in all 234 such fixtures. `0x360` (`v_readlane_b32`) has
  a scalar destination in bits 7:0 and no vector destination.
- **VINTERP-encoded VOP3.** Six opcodes (`0x200/0x201/0x202` `v_interp_p1/p2/mov_f32_e64`,
  `0x342/0x343/0x35a` `v_interp_p1ll/p1lv/p2_f16`) put an attribute selector in the **src0** slot and
  the VGPRs in src1/src2, although the disassembler prints the attribute last. Measured over all 67
  fixtures: attribute index = field bits 5:0 (67/67), channel = bits 7:6 with `x/y/z/w` = 0/1/2/3
  (67/67). Bit 8 does **not** correspond to the printed `high` flag (two fixtures set it with no
  `high`), so it is left unclaimed. `v_interp_mov_f32_e64` additionally takes its param selector
  from the src1 slot (0 = `p10`, 1 = `p20`, 2 = `p0`, 14 fixtures); the other five keep a VGPR there.
- `v_nop_e64` (`0x180`) has no operands; its fields decode as inline constants and are not reported.

### SOP2 and SOPC

| format | bits | field |
|---|---|---|
| SOP2 | 31:30 | `0b10` (scalar prefix shared with SOP1/SOPC/SOPP/SOPK) |
| SOP2 | 29:23 | opcode (7 bits) |
| SOP2 | 22:16 | `sdst` (7 bits) |
| SOP2 | 15:8 | `ssrc1` |
| SOP2 | 7:0 | `ssrc0` |
| SOPC | 31:23 | `0x17E` |
| SOPC | 22:16 | opcode (7 bits) |
| SOPC | 15:8 | `ssrc1` |
| SOPC | 7:0 | `ssrc0` |

- Measured over all 1,656 SOP2 fixtures: every register token in the printed `dst, src0, src1`
  list is exactly `sdst`, `ssrc0`, `ssrc1` — 3,720 tokens, zero mismatches, including the 846
  `s[a:b]` pair forms (`s_cselect_b64`, `s_and_b64`, …), where the printed pair base equals the
  field. The order is the printed one, which is *not* the field order (ssrc0 is the low byte).
- Measured over all 342 SOPC fixtures: `s_cmp_*` prints `src0, src1` with no destination (the
  result goes to SCC), matching `ssrc0`/`ssrc1` in all 414 register tokens.
- A literal dword follows when `ssrc0` — or `ssrc1`, where the form allows it — is `0xFF`; that is
  the length rule the matrix already pinned (`s_absdiff_i32 s0, s1, 0x3f717273`).
- `sdst` is 7 bits wide, so `s0..s105` are reachable and the special numbers (`vcc_lo`, `exec_lo`,
  `m0`, …) are decoded only for the values listed in the special-register table below.

### Named scalar special registers and inline constants

Seven field values are the ones the fixtures actually print by name, and only those are named:
`106 vcc_lo`, `107 vcc_hi`, `118 ttmp10`, `119 ttmp11`, `124 m0`, `126 exec_lo`, `127 exec_hi`.
The 64-bit pair forms print `vcc` / `exec` for the same fields 106 / 126; since the printed name
carries the pair width but no field decodes that width, the harness accepts both spellings and the
operand's width stays `unknown`. `flat_scratch`, `xnack_mask`, `ttmp0..9` and `null` appear in no
fixture, so they stay unclassified rather than being filled in from the ISA text.

Inline constants (fields 128..192 = `0..64`, 193..208 = `-1..-16`, 240..247 =
`{0.5,-0.5,1.0,-1.0,2.0,-2.0,4.0,-4.0}`). The fixtures pin both ends of each run: `0 -> 0x80`,
`-1 -> 0xC1`, `0.5 -> 0xF0`, `-4.0 -> 0xF7`; the values between are the documented table those
ends bracket. A literal dword is carried as `literal_value` on the instruction and compared as a
number, including the `|0.5|` spelling (abs applied to an inline constant).

### SOPK and SOPP

| format | bits | field |
|---|---|---|
| SOPK | 31:28 | `0xB` |
| SOPK | 27:23 | opcode (5 bits) |
| SOPK | 22:16 | register (`sdst`, or the source on `s_setreg_b32`) |
| SOPK | 15:0 | `simm16` |

- `s_getreg_b32` / `s_setreg_b32` carry a **packed hwreg** in 15:0, decoded as
  `id = bits 5:0`, `offset = bits 10:6`, `size = bits 15:11 + 1`. Two words pin it:
  `hwreg(52, 8, 3)` = `0x1234` and `hwreg(HW_REG_TBA_HI, 7, 25)` = `0xC1D1` (so the printed size is
  one-based and `HW_REG_TBA_HI` encodes 17). `s_setreg_imm32_b32` keeps the hwreg in 15:0 and puts
  its immediate in the next dword; `s_version` prints only its immediate; on `s_setreg_b32` the
  22:16 field is the source, not a destination.

| SOPP | bits | field |
|---|---|---|
| SOPP | 22:16 | opcode (7 bits) |
| SOPP | 15:0 | `simm16`, printed unsigned |

Only the SOPP opcodes whose fixtures print one plain number get an immediate operand (24 opcodes:
`0x00`, `0x02`, `0x04..0x09`, `0x0b`, `0x0d..0x0f`, `0x12`, `0x14`, `0x15`, `0x17..0x1a`, `0x20`,
`0x21`, `0x24`, `0x25`, `0x28`; 72 tokens). The packed forms (`s_waitcnt`, `s_sendmsg`,
`s_waitcnt_depctr`) and the operand-less forms are not given a number that is not one.

### SDWA and DPP modifier forms

7,772 `_sdwa` rows and 2,308 `_dpp` rows. In both, the low byte of the 9-bit source field is a
marker (`0xF9` / `0xFA`) and the real source register lives in the second dword:

| field | bits | measurement |
|---|---|---|
| SDWA/DPP `src0` register | dword1 7:0 | 6,262 SDWA tokens, 0 mismatches; DPP rows agree |
| SDWA `src0` scalar flag | dword1 23 | `v_mov_b32_sdwa v5, s1` = `0x00860601` |
| SDWA `src1` register | dword0 16:9 | 6,252 tokens (the 162 `v_add_co_ci_u32` forms insert `sdst`, which the harness skips) |
| SDWA `src1` scalar flag | dword1 31 | scalar `src1` fixtures all set it |
| `dst_sel` | 10:8 | `BYTE_0..3`/`WORD_0..1`/`DWORD` = 0..6 (vector-destination forms only) |
| `dst_unused` | 12:11 | `UNUSED_PAD/SEXT/PRESERVE` = 0/1/2 (same forms) |
| `clamp` | 13 | `..._sdwa v5, v1, v2 clamp` sets `0x2000` (same forms) |
| `omod` | 15:14 | `mul:2` / `mul:4` set bit 14 / 15 (same forms) |
| `src0_sel` / `src1_sel` | 18:16 / 26:24 | same 0..6 encoding, on every SDWA form |
| `sext` `src0`/`src1` | 19 / 27 | `sext(vN)` flips exactly that bit |
| `neg` `src0`/`src1` | 20 / 28 | `-v1` → `0x160601` (bit 20) |
| `abs` `src0`/`src1` | 21 / 29 | `\|v1\|` / `\|v2\|` flip 21 / 29 |

**Compare forms reuse the destination bits.** `v_cmp*_sdwa` has no vector destination, so bits 15:8
hold the **scalar destination** instead and the disassembler prints no `dst_sel`/`dst_unused`:
`{bit 15 = SGPR flag, bits 14:8 = field}`, measured on the 3,452 compare rows that print one —
`s6` / `s[6:7]` → `0x86`, `s8` → `0x88`, `s100` → `0xE4`, and `vcc` / `vcc_lo` → `0x00` (field 0
with the flag clear is the vcc pair, special 106). No other flag-clear encoding appears, so those
stay unclassified rather than being guessed.

Every one of these is asserted, not just decoded: the harness maps the printed
`dst_sel:`/`dst_unused:`/`src0_sel:`/`src1_sel:` names to their values and compares them with the
decoded bits, checks `clamp`/`omod`, and fails if a compare form prints a destination selector at
all — **22,472 attribute checks, all matching**. The compare form's printed scalar destination is
compared too (3,452 more operand tokens). Deliberately breaking a bit position is caught: reading
`dst_sel` from 11:9 instead of 10:8 fails 2,724 fixtures, and reading the compare `sdst` flag from
the wrong bit fails 3,206.

### DPP control word

2,308 `_dpp` rows in two forms, told apart by the source-field marker in dword0: `0xFA` is DPP16,
`0xE9` / `0xEA` are DPP8 (the `0xEA` marker *is* DPP8's `fi` flag). DPP is not SDWA: no scalar
default flag, so a DPP source is a plain `vN`, and the bits that hold SDWA selectors hold the DPP
control word instead.

**DPP16** (marker `0xFA`), second dword:

| field | bits | measurement |
|---|---|---|
| `src0` register | 7:0 | every DPP row; no scalar form exists |
| control, quad mode | 16:8 (bit 16 = 0) | `quad_perm:[a,b,c,d]`, lane i is bits (8+2i+1 : 8+2i): `[3,2,1,0]` = `0x01B`, `[0,1,2,3]` = `0x0E4` |
| control, row mode | 16:8 (bit 16 = 1) | op in 15:12, amount in 11:8: shl `0x10n`, shr `0x11n`, ror `0x12n`, mirror `0x140`, half_mirror `0x141`, share `0x150\|n`, xmask `0x160\|n` |
| `fi` | 18 | `fi` / `fi:1` sets it, `fi:0` and "not printed" clear it (97 fixtures) |
| `bound_ctrl` | 19 | set whenever a `bound_ctrl` operand is printed — **both** `bound_ctrl:0` and `bound_ctrl:1` set it, so only its presence is encoded |
| `neg` / `abs` | 20 / 21 src0, 22 / 23 src1 | `-v1` → `0x00101B01`, `\|v1\|` → `0x00201B01`, `-v2` → `0x00401B01`, `\|v2\|` → `0x00801B01` |
| `bank_mask` | 27:24 | `bank_mask:0x1/0x3/0xf` → `0x01000000` / `0x03000000` / `0x0F000000` |
| `row_mask` | 31:28 | `row_mask:0x1/0x3/0xf` → `0x10000000` / `0x30000000` / `0xF0000000` |
| bit 17 | — | never set in any DPP16 row, so it stays unnamed |

An **omitted** `row_mask` / `bank_mask` encodes `0xF`, not 0: the two fixtures that print only
one of them carry 15 in the other field (`v_mov_b32_dpp v5, v1 quad_perm:[3,2,1,0] bank_mask:0x0`
→ row_mask 15). That is asserted as the default rather than treated as absent.

**DPP8** (markers `0xE9` / `0xEA`): `dpp8:[a..h]` prints eight 3-bit lane selectors, lane i in bits
(8+3i+2 : 8+3i) of the second dword — `[0,1,2,3,4,5,6,7]` = `0xFAC68800` over bits 31:8, and
`[7,6,5,4,3,2,1,0]` = `0x05397700`. Both marker spellings carry the same lanes; only the marker
differs, and it is exactly the `fi` flag (92 rows `0xE9` without `fi`, 92 rows `0xEA` with `fi:1`,
2 rows `0xE9` with an explicit `fi:0`). DPP8 prints no masks and no `bound_ctrl`, and no fixture
gives it a source modifier, so those stay undecoded for that form.

All of it is asserted: **13,919 DPP control checks, all matching** — quad lanes, row op/amount, both
masks (with the 0xF default), `fi`, `bound_ctrl` presence, and the DPP16 `neg`/`abs` bits, which
were previously not decoded at all. Deliberately reading the control from 15:7 instead of 16:8 and
`row_mask` from 30:27 instead of 31:28 fails 2,122 fixtures.

### VINTERP param and operand-less forms

- `v_interp_mov_f32_e64` (the only interp form that takes one) puts a **param selector** in its
  src1 slot: 0 = `p10`, 1 = `p20`, 2 = `p0` (14 fixtures). The other interp forms keep a plain VGPR
  there (`v_interp_p1ll_f16 v5, v2, attr31.x`).
- `v_nop` (VOP1 `0x00`), `v_pipeflush` (`0x1b`), `v_clrexcp` (`0x41`) and `v_nop_e64` (VOP3
  `0x180`) print no operands at all; their vdst/src0 fields are unused and are not reported.

### Memory formats: the opcode field, measured

The six memory families carry 3,542 fixtures. Each row below is the *narrowest* bit range for
which every printed mnemonic has exactly one value across all of its rows; each mnemonic appears
with many different operands, so that stability is evidence about the field rather than one
encoding's coincidence. Where two ranges fit, the ambiguity is stated, not hidden.

| format | encoding | opcode bits | mnemonics -> distinct values | length |
|---|---|---|---|---|
| DS | `0x36` | 25:18 | 154 -> 154 | 2 dwords (all 1,865) |
| FLAT | `0x37` | 24:18 | 88 -> 46: `flat_`/`global_`/`scratch_` spellings share an opcode | 2 (all 153) |
| MUBUF | `0x38` | 25:18 | 60 -> 60 | 2 (all 781) |
| SMEM | `0x3D` | 25:18 | 68 -> 68 | 2 (all 459) |
| MIMG | `0x3C` | 24:18 | 88 -> 79: `image_*` and `image_msaa_*` share an opcode | **measured**: 2 + `dim` (word0 bits 2:1), decoded histogram {2: 159, 3: 70, 4: 31} over 260 rows, 0 disagreements |
| EXP | `0x3E` | not extracted | one mnemonic | 2 (all 24) |

The ambiguity: for DS/MUBUF/SMEM, `26:18` fits equally well because bit 26 is constant inside
each family's `0x36`/`0x38`/`0x3D` encoding, so the narrower `25:18` is reported; for FLAT/MIMG
the same holds for bit 25. Nothing about the *meaning* of an opcode value is claimed beyond "one
value per mnemonic".

Not claimed: the operand fields still listed as open below (FLAT registers, EXP's uniform
operands, MIMG's remaining operands). Fixtures that do not print any of the nine measured memory fields are
asserted on format, length and opcode only. MIMG's length is claimed in the measured direction:
`words = 2 + dim` from bits 2:1, 260 of 260 rows agreeing.

### Memory operand fields: measured ranges, and what stayed open

`tests/shader/fixtures/measure_memory_operands.py` groups each family by (mnemonic,
operand position), keeps only groups whose printed value varies, and then keeps a bit
range only if the field equals the printed value in *every* such group. Ranges with zero
violations over the rows that print the role:

Every number below is printed by the harness itself (`== asserted operand tokens per field ==` in
`out/shader_tests/gfx10_conformance.md`), with each exclusion reason printed under its field. `asserted` counts token-compared
assertions; `word-only` is the part of `asserted` decoded from the instruction words when the mnemonic
printed no operand token to compare, printed as its own column so the total is not inflated. The harness
**fails** (a printed MISMATCH row) if asserted + excluded != the fixture row count for that field.

The table is in the order the harness prints, with the same columns and no others;
`tests/shader/fixtures/check_doc_table.py` fails if this block and a fresh run disagree.

The table is in the order the harness prints, with the same columns and no others: a cell with no
printed reason line is empty (mimg mvdst), and the `exclusions` cell is the printed reason line
verbatim. `tests/shader/fixtures/check_doc_table.py` diffs this block against a fresh run.

| field | asserted | word-only | excluded | rows | exclusions |
|---|---|---|---|---|---|
| ds moffset | 1,630 | 0 | 235 | 1,865 | 235 excluded: offset0:/offset1: or offset:swizzle() word |
| ds roles | 1,855 | 0 | 10 | 1,865 | 10 excluded: prints no register operand: operand word 0x00000000 carries four 8-bit vgpr fields but the row names no v[] position to compare |
| flat msaddr | 70 | 0 | 83 | 153 | 83 excluded: printed shape does not name this slot (off where the omitted register is unidentifiable, a lone v with saddr, or values that coincide) |
| flat mvaddr | 118 | 0 | 35 | 153 | 35 excluded: printed shape does not name this slot (off where the omitted register is unidentifiable, a lone v with saddr, or values that coincide) |
| flat mvdata | 110 | 0 | 43 | 153 | 43 excluded: printed shape does not name this slot (off where the omitted register is unidentifiable, a lone v with saddr, or values that coincide) |
| flat mvdst | 12 | 0 | 141 | 153 | 141 excluded: printed shape does not name this slot (off where the omitted register is unidentifiable, a lone v with saddr, or values that coincide) |
| mimg mdmask | 247 | 0 | 13 | 260 | 12 excluded: mnemonic prints no dmask operand (image_bvh*_intersect_ray); the 11:8 slot is not a dmask there<br>1 excluded: no printed text: the row has words only, so the 11:8 slot cannot be checked against a dmask:0x token |
| mimg mnsa | 100 | 0 | 160 | 260 | 159 excluded: contiguous address: dim 2:1 is 0, so no NSA list is encoded and the one v[dword] is the whole address<br>1 excluded: no printed text: dim 2:1 gives the extra-dword count but the component count exists only in the printed list |
| mimg msrsrc | 257 | 1 | 3 | 260 | 3 excluded: ttmp[] resource form: the 52:48 field names no s[] base |
| mimg mssamp | 103 | 0 | 157 | 260 | 157 excluded: mnemonic prints one s[] resource and no sampler operand |
| mimg mvaddr | 159 | 0 | 101 | 260 | 101 excluded: NSA address list: components live in extra dwords |
| mimg mvdst | 260 | 1 | 0 | 260 |  |
| mubuf moffset | 779 | 0 | 2 | 781 | 2 excluded: no operands on this mnemonic; operand word 0x00000000 is reserved, so no role position exists |
| mubuf msoffset | 590 | 0 | 191 | 781 | 49 excluded: constant soffset `-1` is encoded as 0xc1 in 63:56, an operand value table entry rather than an s[] index<br>22 excluded: constant soffset `-4.0` is encoded as 0xf7 in 63:56, an operand value table entry rather than an s[] index<br>22 excluded: constant soffset `0.5` is encoded as 0xf0 in 63:56, an operand value table entry rather than an s[] index<br>48 excluded: constant soffset `0` is encoded as 0x80 in 63:56, an operand value table entry rather than an s[] index<br>48 excluded: constant soffset `m0` is encoded as 0x7c in 63:56, an operand value table entry rather than an s[] index<br>2 excluded: no operands on this mnemonic; operand word 0x00000000 is reserved, so no role position exists |
| mubuf msrsrc | 779 | 0 | 2 | 781 | 2 excluded: no operands on this mnemonic; operand word is 0x00000000 (reserved), so the field names no register |
| mubuf mvaddr | 779 | 0 | 2 | 781 | 2 excluded: no operands on this mnemonic; operand word 0x00000000 is reserved, so no role position exists |
| mubuf mvdst | 779 | 0 | 2 | 781 | 2 excluded: no operands on this mnemonic; operand word is 0x00000000 (reserved), so the field names no register |
| smem moffset | 70 | 0 | 389 | 459 | 389 excluded: row prints no immediate offset (a register or operand value occupies dword1 31:25) |
| smem msbase | 432 | 0 | 27 | 459 | 13 excluded: mnemonic prints no sbase operand in this row<br>8 excluded: second operand `vcc` is a named pair; (word0 5:0) * 2 = 106 is that pair's index rather than an s[] token<br>6 excluded: two-operand form prints sdata + soffset/offset and no sbase |
| smem msdata | 417 | 0 | 42 | 459 | 42 excluded: two-operand s_dcache_discard: no data slot |
| smem msoffset | 312 | 0 | 147 | 459 | 13 excluded: third operand `(none)`: dword1 31:25 = 0x00 is the offset form marker, not an s[] index<br>18 excluded: third operand `0x0`: dword1 31:25 = 0x7d is the offset form marker, not an s[] index<br>16 excluded: third operand `0x1234`: dword1 31:25 = 0x7d is the offset form marker, not an s[] index<br>1 excluded: third operand `0x123`: dword1 31:25 = 0x7d is the offset form marker, not an s[] index<br>1 excluded: third operand `0x1`: dword1 31:25 = 0x7d is the offset form marker, not an s[] index<br>34 excluded: third operand `0x64`: dword1 31:25 = 0x7d is the offset form marker, not an s[] index<br>16 excluded: third operand `m0` is an operand value encoded 0x7c in dword1 31:25, not an s[] index<br>16 excluded: third operand `null` is an operand value encoded 0x7d in dword1 31:25, not an s[] index<br>16 excluded: third operand `vcc_hi` is an operand value encoded 0x6b in dword1 31:25, not an s[] index<br>16 excluded: third operand `vcc_lo` is an operand value encoded 0x6a in dword1 31:25, not an s[] index |
| **all memory fields** | **9,858** | **2** | **1,785** | | |
Per-shape sums equal the printed per-format totals: DS 1,630 + 1,855 = 3,485; MUBUF 779 x 5 =
3,895; MIMG 260 + 100 + 257 + 159 + 247 + 103 = 1,126; SMEM 417 + 312 + 432 + 70 = 1,231;
FLAT 12 + 118 + 110 + 70 = 310; total 9,858. EXP contributes 0: its target is an operand at 7:4
(one mnemonic spans every target), so it cannot enter the one-opcode-per-mnemonic check.

**MIMG length is measured, not tabled.** `tests/shader/fixtures/mimg_length.py` selects every fixture row
whose word0 decodes as MIMG (bits 31:26 == 0x3c) and prints **260 rows**, dword histogram
**{2: 159, 3: 70, 4: 31}** summing to 260, and **0 rows** disagreeing with `words = 2 + ((word0 >> 1) & 3)`
- the dim field at word0 bits **2:1** is exactly the extra dword count. 233 of those rows are filed under the
`mimg` family column and 27 under `13`/`30_new`, which is why the harness sees 260 MIMG rows. The decoder
sets `words = 2 + dim`, the stream walker steps MIMG (260/260 length-ok), and `sc_decode_instruction`
copies every dword the caller supplies: dword 1 for every multi-dword form (previously only `words == 2`,
which had zeroed the 3- and 4-dword operands) and dwords 2.. for the NSA components.
The isa suite adds 18 MIMG checks (3 fixture rows x vdst/vaddr/srsrc/dmask/NSA/format) through
`sc_decode_instruction(words, count)`, which takes the words and count from the caller and never consults the walker.

**Totals:** 37,905 fixtures read from all 38 `gfx10*.s` files (21 contribute encodings, 17 are error-only),
37,905 passed every applicable check, 0 failures; isa 281 checks; `/W4 /WX` exit 0, zero warnings.
**DS registers are measured, per row.** Every printed DS register equals one of the four bytes of
dword 1 (7:0, 15:8, 23:16, 31:24); the same byte ranges of word0 match **0** rows in every printed
position, and word0 31:16 is the printed offset. The printed position -> byte permutation is a
per-mnemonic property (the order does flip between `_rtn` and `cmpst` shapes), so it is derived per
(mnemonic, token count) key from a row whose vgpr values pin the positions and then asserted on
every row: **150 keys, 1,855 asserted, 0 disagreements**, 10 rows with no printed register excluded
(the operand word is present, the row names no position). Asserted + excluded = 1,865.

Open, with the fixture count that cannot close it:

**MUBUF roles are measured, per row.** vaddr is dword1 7:0: 100/100 rows that print a register
match it, and all 679 `off` rows encode 0 there. offset is word0 **11:0** - 717/717 printed
`offset:N` match, while bits 15:0 disagree on 193 rows because 12..15 are flag bits - and the
64 rows that omit `offset:0` all encode 0. soffset is dword1 31:24: 590/590 `sN` rows are the
raw index, while constants are an 8-bit operand table (0x80 = 0, 0xC1 = -1, 0xF0 = 0.5,
0xF7 = -4.0, 0x7C = m0) and are excluded with that evidence. Counts: vaddr 779 + 2, offset
779 + 2, soffset 590 + 191, each closing on 781.
  `offset:N` sits inside the soffset token; both stay unknown.
**SMEM roles are measured, per row.** sbase is (word0 5:0) * 2 - 440/440 rows that print an
`s[]` base match, while the 8 rows that print `vcc` and the 19 that print no base are excluded
with that evidence. soffset is dword1 **31:25**: 308/308 `sN` rows match; m0/null/vcc_lo/vcc_hi
and the 0x7d offset-form marker are operand values, not indices, and are excluded. offset is
dword1 **20:0** - 106/106 printed immediates match, while 7:0, 12:0 and 15:0 disagree on 51, 34
and 34 rows because the width is 21 bits. Counts: sbase 432 + 27, soffset 312 + 147, offset
70 + 389, each closing on 459; sdata stays at 417 + 42.
- **FLAT registers (134).** Only four (mnemonic, position) groups vary at all, and the `off`
  ambiguity applies too, so no FLAT register role is pinned.
- **EXP operands (24).** All four source fields print `v0` in every harvested row.
- **MIMG's remaining operands (260).** dmask (word0 11:8) and the NSA address list are measured and
  asserted per row (247/247 dmask tokens, 100/100 lists); ssamp and any further MIMG operand are not.
- **MIMG length.** Measured, not open: `words = 2 + dim` (word0 bits 2:1), 260/260 rows, and the
  dword histogram {2:159, 3:70, 4:31} in `mimg_length.py`.
## 5. Work queue (what the matrix says is still missing)

The queue is no longer empty, and every entry is a stated gap rather than a failure:

```
- operand fields still open (FLAT registers, EXP operands, MIMG ssamp)          see section 4
  MIMG ssamp)
- opcode field not extracted for format exp                                  24
- opcode field not extracted for format vop3p                                 7
- operand token not a plain register (hwreg / vcc / literal)                  4
- EXP target is an operand, not an opcode, so it is not exposed through sc_isa   24
```

The 4 token entries are the normal residue (`s_setreg_imm32_b32` hwreg, implicit `vcc`): cases
where the printed token is not a plain register, so no field is asserted for it.
## 6. Decoder bugs the matrix found (fixed)

| # | Finding | Evidence | Fix |
|---|---|---|---|
| 1 | DPP8 was not recognised as a modifier: 189 fixtures consumed 1 dword instead of 2 and read the selector as a register field | `gfx10_asm_dpp8.s` (`0x7e0a_02e9` + dword) | `sc_isa.cpp`: treat src0 `0xE9` and `0xEA` (fi) as `extra = dpp` |
| 2 | A literal in **ssrc1** was not counted for SOP2/SOPC, so a 2-dword scalar instruction looked 1 dword long (286 fixtures) | `s_absdiff_i32 s0, s1, 0x3f717273` → ssrc1 = `0xFF` | `sop_extra(..., ssrc1_can_be_literal)`; SOP1 passes `false` because bits 15:8 are opcode there |
| 3 | `v_madmk/v_madak/v_fmamk/v_fmaak` own a literal dword that is not signalled in src0 (55 fixtures) | `v_madmk_f32 v5, v1, 0x11213141, v3` → `0x400a0701 0x11213141` | VOP2 opcodes `0x20/0x21/0x2C/0x2D/0x37/0x38` set `extra = literal` |
| 4 | `v_readfirstlane_b32` writes an SGPR, but the decoder classified the vdst field as a VGPR | `v_readfirstlane_b32 s103, v1` → `0x7e0a0567` (vdst field = 103) | VOP1 opcode `2`: vdst via the scalar classifier; width `b32` documented |
| 5 | `v_madmk/v_fmamk` place the literal after a source that may itself be an inline constant, so searching for "the first numeric token" found src0 instead of the literal | `v_madmk_f32 v5, 0, 0x11213141, v3` (38 fixtures) | harness: the literal search starts at operand index 2, the position of the second source |
| 6 | The SDWA decode claimed `dst_sel`/`dst_unused`/`clamp`/`omod` on the compare forms, where those bits are the scalar destination; the selectors were also never compared, so nothing noticed | `v_cmp_f_f32_sdwa s[6:7], v1, v2` = `0x7c0004f9 0x06068601` — bits 15:8 = `0x86` is `s6`, not a selector set | `sc_wave`: compare SDWA decodes `sdst` from bits 15:8 and leaves the destination selectors unclaimed; the harness now compares all four selectors, `clamp`/`omod`, and that `sdst` token |
| 7 | The DPP control word was never decoded (only the source register), and the harness compared no DPP attribute, so a wrong control/mask/fi field would have passed silently; the DPP forms' `-vN`/`|vN|` modifiers were unasserted too | `v_mov_b32_dpp v5, v1 row_shl:1` = `0x7e0a02fa 0x00010101` — control `0x101` at 16:8, while `-v1` sets bit 20 | `sc_wave`: `sc_dpp_control` decodes control, masks, `fi`, `bound_ctrl`, modifiers and the DPP8 lanes with the `0xE9`/`0xEA` `fi` marker; the harness compares every printed attribute |

Harness mistakes corrected while calibrating (not decoder faults): the length check double-counted
the extra dword; `_e64` was tested after stripping the suffix; `v_cmpx` prefix compared 7 chars
against a 6-char literal; VOP2 carry forms and `v_cmpx` print operand lists that shift the source
positions (sdst inserted / destination omitted). The fifth: the "sdst inserted" rule fired on any
four-piece form whose second operand is scalar, which misread the literal forms
(`v_madak_f32 v0, s0, v1, 0x4229ae14` - src0 is `s0`, the literal is last); it now requires that
the last piece not be a number.

Gaps the VOP3 pass exposed in our own view of the form — each one measured before it was coded:
`sc_wave` never emitted VOP3 operands at all (`carries_operands` omitted the format, so 14,562 rows
produced no operands); the VOP3 literal selector was assumed to be src0-only when the fixtures use
src1 and src2 too; and the scalar-destination opcodes above were only partially listed.

## 7. What this does not prove

- **EXP source positions.** All 24 EXP rows encode dword1 = 0 and print v0, v0, v0, v0, so the
  four uniform source slots cannot be isolated; the target (word0 9:4) is readable but no fixture
  varies it.
- **MIMG register widths.** The printed width is a printer rule (mnemonic plus dmask); the words
  carry only the base index, so widths are neither decoded nor decidable from these fixtures.
- **VOP3P.** Seven fixtures now exist (`v_dot2_f32_f16`, `v_dot2_i32_i16`, `v_dot2_u32_u16`,
  `v_dot4_i32_i8`, `v_dot4_u32_u8`, `v_dot8_i32_i4`, `v_dot8_u32_u4` at `0x33`), but every one
  prints the same `v0, v1, v2, v3` operands, so no operand field can be isolated by varying one
  thing at a time. What does differ between the seven lives in bits 22:16, which is not enough to
  name an opcode field (25:16 fits equally), so VOP3P keeps both its opcode field and its operands
  unclaimed. The `v_dot2c`/`v_dot4c` "commit" forms are a 32-bit VOP2 encoding, not VOP3P.
- **`op_sel_hi`.** No harvested fixture prints it, so it is not decoded.
- **VOP3 operand widths.** No fixture pair pins a VOP3 source or destination width, so every VOP3
  operand stays `unknown` width; only the VOP1/SOP1 rules in `sc_wave.cpp` carry widths.
- **SOP2/SOPC widths.** The `_b64` fixtures do print `s[a:b]` pairs (846 pair operands, base
  matches the field), but the module assigns no width for these forms: the same opcode prints a
  pair in some fixtures and a single register in others (`s_lshl_b64 s[0:1], s[2:3], s4`), so a
  per-opcode width rule would be a guess. They stay `unknown`.
- **Special scalar registers beyond the seven measured values.** `vcc_lo/hi`, `exec_lo/hi`, `m0`
  and `ttmp10/11` are named because the fixtures print them by name; `flat_scratch`, `xnack_mask`,
  `ttmp0..9` and `null` are in no harvested fixture, so they stay `unclassified`.
- **SDWA/DPP are decoded only for the forms present.** The field maps above come from 7,772 SDWA
  and 2,308 DPP rows. No fixture gives a DPP source a scalar flag, so DPP sources stay VGPR-only.
- **DPP gaps.** DPP8 has no fixture with a source modifier, a mask or `bound_ctrl`, so those stay
  undecoded for that form; `bound_ctrl`'s printed 0/1 value is not encoded in the fixtures (both
  set bit 19), so only its presence is claimed; DPP16 bit 17 is never set and stays unnamed.
- **Compare-SDWA destinations beyond vcc.** Only `vcc`/`vcc_lo` (field 0, SGPR flag clear) and the
  106 SGPR values the fixtures print are measured for the compare form's `sdst`; any other
  flag-clear encoding has no example and stays unclassified.
- **hwreg packing.** `id`/`offset`/`size` are pinned by two words (`hwreg(52, 8, 3)`,
  `hwreg(HW_REG_TBA_HI, 7, 25)`); the id-to-name table has exactly one measured entry
  (`HW_REG_TBA_HI` = 17), and any other name would be an ISA-text claim, not a measured one.
- **Interp bit 8.** Visible in the attribute field, but it matches no printed modifier, so it is not
  named; the attribute's use as an interpolation attribute is the mnemonic's statement, not ours.
- **Opcode numbering vs AMD.** Only three opcodes are asserted against an external value; the rest is
  self-consistency (one opcode per mnemonic). A wrong field position that is *stable* would pass.
- **Coverage of the ISA.** All 38 gfx10 files, but the pass rate is "everything harvested
  passed", not "the ISA is decoded": the operand fields listed as open above and all of VOP3P's
  layout are open, and the error-test files contribute only where `llvm-mc` printed an encoding.
- **Memory operand fields.** Seventeen roles are decoded and asserted per row: DS moffset and its
  four register roles, MUBUF vdst/srsrc/vaddr/moffset/soffset, SMEM sdata/sbase/soffset/offset,
  MIMG vdst/vaddr/srsrc/ssamp/dmask plus the NSA address list, and the four FLAT/GLOBAL/SCRATCH
  slots with their off encodings. Section 9 shows each agreeing with an independent decoder and
  records the cases where that decoder's printer is the side that is wrong.
- **Real shaders.** These are assembler outputs for RDNA1. Whether console-compiled streams use
  forms outside this set is exactly what the corpus will answer.

## 8. Reproduce

```bat
rem once, with network access (see tests\shader\fixtures\README.md for URLs/hashes)
python tests\shader\fixtures\harvest_gfx10_mc.py

tests\shader\build_conformance_test.bat
rem -> 37,905 fixtures, 0 failures, out\shader_tests\gfx10_conformance.md
```

### Standalone build and link contract

The harness proves the matrix; this proves the code compiles and links as a unit, not just inside one
test binary. From the repo root:

```bat
tests\shader\build_all_no_dxc.bat
```

Measured result (MSVC 14.44.35207, Windows SDK 10.0.26100.0), exit 0:

- `cl -nologo -std:c++17 -EHsc -W4 -WX -DSC_FRONTEND_NO_DXC -Iinclude -c` over all ten no-DXC
  sources (`sc_sha256`, `sc_container`, `sc_xs2`, `sc_frontend`, `sc_isa`, `sc_wave`, `sc_capture`,
  `sc_dxil_backend`, `sc_gpu_render_test`, `sc_mesh_compute`): zero warnings at `/W4 /WX`.
- `sc_dxil_emit.cpp` compiled on its own in its real configuration (without `SC_FRONTEND_NO_DXC`),
  also zero warnings.
- The ten objects archived to `out\shader_tests\sc_shader_nodxc.lib`.
- The same nine compiled again with `-std:c++14`: zero warnings, so nothing here requires C++17.

Only the C++ standard library and the Windows SDK are used. The SDK headers appear in exactly two
files (`sc_dxil_emit.cpp`, `sc_gpu_render_test.cpp`: `d3d12.h`, `dxgi1_6.h`, `wrl/client.h`,
`windows.h`); there is no third-party header, no `dxcompiler.h`, and no `dxcompiler.lib` or
`d3dcompiler.lib` in the link.

**Link contract.** `sc_dxil_backend.cpp` calls `sc_make_dxil_emitter`, which lives in
`sc_dxil_emit.cpp`. That dependency is checked by a repo script, not by a scratch command:

```bat
tests\shader\build_link_smoke.bat
rem -> exit 0, "link contract verified: LNK2019 for sc_make_dxil_emitter without
rem    sc_dxil_emit.obj, clean link and correct decode with it (/W4 /WX, C++17)"
```

It compiles the twelve no-DXC sources (the BCn decode module is the twelfth) plus `tests\shader\link_smoke_main.cpp` (a 26-line program that
decodes `flat_load_dword v5, v[1:2]` = `0xDC300000 0x057D0001`) at `/W4 /WX`, then links twice.
The negative link — the no-DXC objects alone — must fail, and the script fails if it does not:

```
sc_dxil_backend.obj : error LNK2019: unresolved external symbol
  "class std::unique_ptr<class sc::sc_dxil_emitter, ...> sc::sc_make_dxil_emitter(void)"
  referenced in function sc::sc_dxil_backend::create
out\shader_tests\link_smoke\link_no_emit.exe : fatal error LNK1120: 1 unresolved externals
```

The positive link adds `sc_dxil_emit.obj`, links cleanly, and the resulting binary must exit 0 on the
FLAT row above (it also returns non-zero if the measured `vdst`/`vaddr`/`off` fields come back
wrong). So a consumer that never touches `sc_dxil_backend` needs only the no-DXC set; any consumer
of `sc_dxil_backend::create` must also compile and link `src\shader\sc_dxil_emit.cpp`. Nothing else
is required — no define, no extra import library. `sc_dxil_emit.cpp` is deliberately absent from
`sc_shader_nodxc.lib`.

## 9. Independent arbiter: rdna2-dis

`rdna2-dis` (RDNA2/gfx10.3, MIT, tables generated from AMD's machine-readable ISA
specification and CI-checked line-for-line against `llvm-objdump`) was built from a local
reference checkout and run over the same fixture set. It is an arbiter only: nothing from it is
vendored, copied or built into this repository.

```
cargo build --release -p rdna2-dis            # in the reference checkout, not this repo
python tests\shader\fixtures\compare_rdna2_dis.py <path>\rdna2-dis.exe
```

The driver concatenates all 37,905 fixture rows, each followed by `s_endpgm` as a separator, and
decodes the whole blob in one run (wave32 spelling by default, `ARB_WAVE=64` for wave64). Each
output line is matched back to its fixture row by the words in its `// ...` comment, so a length
disagreement is reported instead of shifting every later row. The full per-row list is written to
`out\shader_tests\arbiter_mismatches_w32.txt`.

**Totals.** 37,905 rows compared; 24,655 print exactly the fixture text (65.0%); 21 rows disagree
on instruction length; 559 rows use gfx10.1 opcodes the arbiter's gfx10.3 table does not contain.

**What the arbiter independently confirms.**

| check | rows | result |
|---|---|---|
| MIMG instruction length (`2 + dim`) | 233 | 233/233 identical dword counts |
| MIMG `dmask` | 232 | 232/232 equal |
| MIMG `dim` | 232 | 232/232 equal |
| MIMG NSA address list | 232 | 225/232 equal; 7 differ only in printed register widths (below) |
| MUBUF whole line (vaddr/offset/soffset/flags) | 771 | 771/771 byte-identical |
| DS whole line | 1,865 | 1,705 identical; 160 are gfx10.1-only `ds_*_src2_*` opcodes absent from gfx10.3 |
| SMEM whole line | 459 | 184 identical; 275 are gfx10.1-only `s_atomic_*`/`s_atc_probe*`/buffer-store opcodes |
| FLAT/GLOBAL/SCRATCH | 134 | 16 identical; 118 differ only by gfx10.3's explicit `vdst` |

**Every disagreement, by class.** After normalising the gfx10.3 spellings below, 445 rows remain
where the two texts differ in substance; each group is listed with a fixture and both outputs.

| rows | class | fixture | fixture text | arbiter text |
|---|---|---|---|---|
| 6,239 | gfx10.3 spelling: `_e32`/`_e64` suffix, `v_mac_*`→`v_fmac_*`, `v_mad_*`→`v_fma_*`, `vcc`↔`vcc_lo`, `s6`↔`s[6:7]`, hex vs decimal | `gfx10_asm_vopc.s` | `v_cmp_f_f32 vcc, v1, v2` | `v_cmp_f_f32_e32 vcc_lo, v1, v2` |
| 3,206 | gfx10.3 prints `vcc` (and dst modifiers) where gfx10.1 prints the compare's SGPR-pair destination | `gfx10_asm_vopc_sdwa.s:11` | `v_cmp_f_f32_sdwa s[6:7], v1, v2 ...` | `v_cmp_f_f32_sdwa vcc_lo, v1, v2 mul:4 ...` |
| 2,264 | gfx10.3 prints the implicit EXEC destination of `v_cmpx` | `gfx10_asm_vopcx.s:58` | `v_cmpx_class_f16_e64 -1, v2` | `v_cmpx_class_f16_e64 exec, -1, v2` |
| 559 | gfx10.1 opcode absent from the arbiter's table | `gfx10-constant-bus.s:14` | `v_madak_f32 v0, s0, v1, 0x4229ae14` | `<unknown vop2 opcode 33>`, 1 dword |
| 442 | arbiter prints no SDWA/DPP source modifier (`-`, `\| \|`, `sext`) or `fi:` | `gfx10_asm_dpp16.s:244` | `v_add_f32_dpp v5, -v1, v2 ...` | `v_add_f32_dpp v5, v1, v2 ...` |
| 116 | gfx10.3 prints an explicit `vdst` for flat/global/scratch | `gfx1030_new.s:45` | `global_atomic_csub v0, v2, s[2:3]` | `global_atomic_csub v0, v0, v2, s[2:3]` |
| 224 | `v_cmp` SDWA dst-modifier: arbiter prints `mul:4` where gfx10.1 does not | `gfx10_asm_vopc_sdwa.s:11` | `v_cmp_f_f32_sdwa s[6:7], v1, v2 src0_sel:DWORD ...` | `v_cmp_f_f32_sdwa vcc_lo, v1, v2 mul:4 src0_sel:DWORD ...` |
| 81 | MIMG printed register *widths* (vdst/srsrc/address list) and NSA padding bytes | `gfx10_asm_mimg.s:167` | `image_atomic_fcmpswap v[1:2], v[2:3], s[12:19] ...` | `... v[1:2], v[2:4], s[12:19] ...` |
| 37 | printer residue: `s_waitcnt_depctr` raw word, `ds_swizzle_b32` numeric swizzle, `scratch_* off` as `v0, v0, exec_hi`, `row_mask:0xf` default, `bound_ctrl` bit | `gfx1030_new.s:141` | `s_waitcnt_depctr depctr_va_vdst(8) ...` | `s_waitcnt_depctr 0x8000` |
| 26 | VINTERP `param` naming | `gfx10_asm_vop3.s:13223` | `v_interp_mov_f32_e64 v5, p10, attr0.x` | `v_interp_mov_f32_e64 v5, 0x0, attr0.x` |
| 24 | `op_sel` arity (two flags vs four) | `gfx10_asm_vop3.s:12892` | `v_permlane16_b32 v5, v1, s2, s3 op_sel:[1,0]` | `... op_sel:[1,0,0,0]` |
| 20 | 16-bit inline literals: arbiter prints `0` | `gfx10_asm_vop2.s:12057` | `v_fmamk_f16 v5, v1, 0x1121, v3` | `v_fmamk_f16 v5, v1, 0, v3` |
| 14 | branch displacement: fixture text is the assembler's unsigned spelling, arbiter prints it signed (same 16 bits) | `gfx10_asm_sop.s` | `s_branch 49617` | `s_branch -15919` |
| 8 | 9-bit VGPR source printed as its raw field | `gfx10_asm_vop1.s:279` | `v_readfirstlane_b32 s5, v1` | `v_readfirstlane_b32_e32 s5, 513` |
| 5 | `..., 0x0` printed as `null` (same zero, different name) | `gfx10_asm_smem.s:869` | `s_buffer_load_dword s5, s[4:7], 0x0` | `s_buffer_load_dword s5, s[4:7], null` |
| 4 | dpp8 form of `v_movrel*` gains a `_dpp` suffix | `gfx10_asm_dpp8.s:576` | `v_movreld_b32 v0, v1 dpp8:[7,...] fi:1` | `v_movreld_b32_dpp v0, v1 dpp8:[7,...] fi:1` |
| 2 | `s_endpgm <imm>` | `gfx10_asm_sop.s` | `s_endpgm 1` | `s_endpgm` |

**Did any of this change a decoder result?** No. Every class is either a gfx10.3-only spelling or
table entry, or a printer gap in the arbiter; in the cases where the two outputs cannot both be
right, the words and the fixture text show the arbiter is the one that is wrong:

- `v_madak_f32` / `v_madmk_f32` own one literal dword (`0x11213141`); the arbiter stops after one
  dword because opcodes `0x20`/`0x21` are missing from its gfx10.3 table. The decoder's three-dword
  reading is what the fixture text and the literal word both show.
- `v_readlane_b32 s5, v1, s2` (word `0xD7600005`): the 9-bit source field is `0x101`, i.e. the VGPR
  bit plus register 1. The arbiter prints the raw `513` where LLVM and this decoder print `v1`.
- `v_fmamk_f16 v5, v1, 0x1121, v3`: the second dword's low half is `0x1121`; the arbiter prints `0`
  because its gfx10.3 form takes a 32-bit literal.
- MIMG NSA: where the arbiter prints more addresses than the dimension implies
  (`[v0, v1, v2, v3, v4, v5, v6, v8, v0]` for a 4-dword NSA row), the extra element is a zero
  padding byte printed as `v0`; the measured list stops at the dimension-derived count.
- The 7 MIMG "address list" diffs and 75 width diffs are printed ranges (`v[5:6]` vs `v[5:7]`,
  `s[0:7]` vs `s[0:3]`, `v[1:12]` vs `v1`), not encoded indices; this decoder stores indices, and
  the arbiter's gfx10.3 width rules differ from gfx10.1's.

### Field-level close-out of the four open groups

Whole-line comparison cannot separate a printer difference from a wrong field, so each group was
adjudicated from the instruction words, field by field. The FLAT/GLOBAL/SCRATCH slots and the MIMG
sampler field were then implemented and are asserted per row by the harness.

| group | rows | word evidence | decoder now | verdict |
|---|---|---|---|---|
| FLAT/GLOBAL/SCRATCH vaddr/vdata/saddr/vdst | 153 rows with word0 format `0x37` (134 in the `flat`/`30_new` families) | dword 1 bytes: vaddr 39:32, vdata 47:40, saddr 55:48, vdst 63:56. `gfx10_asm_flat.s:266` `global_atomic_fcmpswap v5, v[1:2], v[2:3], off offset:-1 glc` = `0xDCF98FFF 0x057D0201` -> bytes `01 02 7D 05`; arbiter prints `... v[1:2], v[2:4] ...` (a width difference only) | decoded and asserted: mvaddr 118, mvdata 110, mvdst 12, msaddr 70, 0 violations | every index agrees; the 118 line diffs are gfx10.3's explicit `vdst`, a printer addition |
| FLAT `off` encodings | 55 | 0x7D in the saddr byte on the 42 `(v,v,off)` and 4 `(v,v,v,off)` rows; 0x7F when both vaddr and saddr are off: `gfx10_asm_flat.s:400` `scratch_load_dword off, off` = `0xDC306400 0x007F0000` | `mem_flat_saddr_off` asserted | agree |
| EXP uniform operands | 24 | target (word0 9:4) runs mrt0..param0; all 24 rows carry dword1 = `0x00000000` and print `v0, v0, v0, v0` | not decoded: no slot to compare | no disagreement possible; the four source positions stay unprovable |
| MIMG `ssamp` | 103 | dword1 25:21 times 4: `gfx10_asm_mimg.s:269` `image_sample v[64:66], v32, s[4:11], s[100:103] dmask:0x7 dim:SQ_RSRC_IMG_1D` = `0xF0800700 0x03214020`, `(0x03214020 >> 21) & 0x1F == 25` -> s100; 20:24 and 22:27 fail on all 103 rows | decoded, 103/103 asserted | arbiter prints the same base; the window is pinned by the words, not by the arbiter |
| MIMG printed register widths | 259 | the encoded value is an 8-bit base index; the printed width is a per-mnemonic printer rule: `popcount(dmask)` holds 201/259 (`image_gather4` always writes 4, `d16` halves it, `image_msaa_load` differs) | stores base indices only | the arbiter's wider ranges (`v[2:4]`, `s[0:7]`) come from its own width table; neither side encodes a width, so no decoder result changes |
| VOP3P | 7 | all seven print `v0, v1, v2, v3`; only bits 22:16 differ, which cannot separate opcode 25:16 from the operand fields | not decoded | nothing isolable; the two texts are identical, so there is no field to adjudicate |

**Verdict for each residual class of the 445**, from the words, not from preference. None of them
touches a field this decoder fills, so none changes a decoder result.

| rows | class | verdict |
|---|---|---|
| 224 | `v_cmp` SDWA dst modifier | Spelling. `gfx10_asm_vopc_sdwa.s:11` `v_cmp_f_f32_sdwa s[6:7], v1, v2 src0_sel:DWORD` puts the compare destination in `sdst` with no `mul:4`; the arbiter's gfx10.3 table names the same `sdst` value `vcc_lo` and adds its default `mul:4`. Same register, different spelling. |
| 81 | MIMG printed widths and NSA padding | Printer. `gfx10_asm_mimg.s:167` `image_atomic_fcmpswap v[1:2], v[2:3], s[12:19]` vs `... v[2:4] ...`: the base index is identical, the width is the arbiter's own rule (table above). Its NSA list sometimes prints a zero padding byte as an extra `v0`; the measured list stops at `1 + 4 * dim`. |
| 37 | printer residue | Arbiter gaps: `s_waitcnt_depctr`/`s_setprio` raw word, `ds_swizzle_b32` numeric swizzle, `scratch_* off` printed as `v0, v0, exec_hi` (its table lacks the 0x7D/0x7F `off` encodings, which the words above show), default `row_mask`, `bound_ctrl`. |
| 26 | VINTERP `param` | Printer: `gfx10_asm_vop3.s:13223` `v_interp_mov_f32_e64 v5, p10, attr0.x` vs `v5, 0x0, attr0.x`. The arbiter prints the raw 3-bit selector; this decoder does not decode VINTERP operands, so nothing to check. |
| 24 | `op_sel` arity | Printer: `gfx10_asm_vop3.s:12892` `op_sel:[1,0]` vs `op_sel:[1,0,0,0]`. Same two bits, the arbiter pads the list to four. |
| 20 | 16-bit inline literal | Arbiter defect, proven by the word: `gfx10_asm_vop2.s:12057` `v_fmamk_f16 v5, v1, 0x1121, v3` = `0x6E0A0701 0x00001121`; its gfx10.3 form takes a 32-bit literal and prints `0`. |
| 14 | branch displacement | Spelling: `s_branch 49617` vs `s_branch -15919` is the same 16-bit field read unsigned by llvm-mc and signed by the arbiter. |
| 8 | 9-bit VGPR source | Arbiter defect: `gfx10_asm_vop1.s:279` `v_readfirstlane_b32 s5, v1` = `0xD7600005`, 9-bit field `0x101` (VGRP bit + 1); the arbiter prints the raw `513`. |
| 5 | `0x0` printed as `null` | Spelling: `gfx10_asm_smem.s:869` `s_buffer_load_dword s5, s[4:7], 0x0`; the SMEM offset field is `0` and `null` is the arbiter's name for it. |
| 4 | `_dpp` suffix | Spelling: `gfx10_asm_dpp8.s:576` `v_movreld_b32 v0, v1 dpp8:[...] fi:1` vs `v_movreld_b32_dpp ...`; same encoding, gfx10.3 mnemonic form. |
| 2 | `s_endpgm <imm>` | Printer: `gfx10_asm_sop.s` `s_endpgm 1` is the fixture's offset word; the arbiter prints the mnemonic without it. |

**What changed in the decoder.** Two fields were added, both pinned by the words rather than by the
arbiter: MIMG `ssamp` (dword1 25:21, 103/103) and the FLAT/GLOBAL/SCRATCH slots (39:32, 47:40,
55:48, 63:56 with `0x7D`/`0x7F` for `off`), asserted per row in the conformance pass and covered by
decoded-value checks in `tests/shader/sc_isa_main.cpp`. No existing field changed value: the
conformance run stays at 37,905/37,905 with 0 failures, and the memory-field table above grew from
16 to 21 rows and from 9,445 to 9,858 asserted tokens. Anything the arbiter is wrong about is kept
as a proof from the words above, not as a preference.
