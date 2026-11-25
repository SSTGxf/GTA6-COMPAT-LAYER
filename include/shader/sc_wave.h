// ScarlettCompat shader pipeline - Phase 5: operand widths and wave semantics.
// MIT. New file; not wired into the boot path.
//
// WHAT THIS IS
//   Phase 4 answers "how long is this instruction and where does the stream go
//   next". Phase 5 answers "which register file does each operand name, and what
//   does the stream say about wave size". It is deliberately narrow: it decodes
//   the operand fields whose bit positions Phase 4 already uses for opcode
//   extraction (VOP1/VOP2/VOPC, the 32-bit vector forms), and it refuses to
//   decode anything else.
//
//   The vector forms carry no wave-size field. A shader's wave size (wave32 vs
//   wave64) is chosen per shader or per pipeline, not per instruction, so this
//   module reports it as UNKNOWN from bytes alone and says why. Encoding a guess
//   here would poison every later phase that consumes it, so `wave_size_known`
//   stays false until a real container or corpus record supplies the value.
//
// OBSERVED vs INFERRED
//   Operand classification is OBSERVED: it is a documented encoding applied to
//   the word, pinned in the harness against real assembler output, e.g.
//       v_mov_b32 v0, s105 -> src0 field 105        -> s105   (SGPR)
//       v_cvt_f32_i32 v1, v2 -> src0 field 258      -> v2     (VGPR, 256 + 2)
//   On GFX10 the 9-bit source field puts SGPRs in 0..255 and VGPRs at 256+n --
//   the opposite of the older GCN convention, which is exactly the kind of thing
//   that must come from the ISA table rather than memory.
//
//   Wave size is INFERRED-at-best and therefore reported as unknown, with the
//   hardware facts kept separate from the stream facts.
//
// NOTHING HERE ASSUMES WAVE64. There is no lane-count arithmetic, no EXEC-mask
// model and no per-lane width. Only register-file classification and a census.
#ifndef SC_SHADER_WAVE_H
#define SC_SHADER_WAVE_H

#include "shader/sc_isa.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sc {

// Which register file an operand field names. `unclassified` is a real answer:
// the field is not one of the documented ranges this module verified (the
// inline-constant table, for instance, is documented but not transcribed here).
enum class sc_operand_file : uint32_t {
    unclassified = 0,
    sgpr,
    vgpr,
    literal,   // the following dword, selected by 0xFF in the source field
    attribute, // VINTERP attribute selector (v_interp_* forms): index = bits 5:0,
               // channel = bits 7:6 (measured; bit 8 matches no printed modifier)
    special,   // a named scalar special register (vcc_lo/hi, exec_lo/hi, m0, ttmpN),
               // decoded only for the field values the fixtures print by name
    constant,  // a scalar inline constant (integer 0..64 / -16..-1, or a float)
    hwreg,     // a packed hwreg(id, offset, size) selector (SOPK getreg/setreg)
    param,     // a VINTERP param selector (p0 / p10 / p20); index = printed number
};

const char* sc_operand_file_name(sc_operand_file f);

// Operand width. `unknown` is the default: width is opcode-selected (a VOP1 f64
// conversion names a register PAIR where v_mov_b32 names one register), so it is
// assigned only from the table in sc_wave.cpp. Anything not in that table stays
// unknown rather than being assumed 32-bit.
enum class sc_operand_width : uint32_t {
    unknown = 0,
    b32,
    b64, // a register pair, e.g. v[0:1] / s[0:1]
};

const char* sc_operand_width_name(sc_operand_width w);

// Where a width assignment comes from, so a documented rule is never presented
// as an assembler-verified one.
enum class sc_width_provenance : uint32_t {
    none = 0,
    isa_documented,     // GFX10 ISA lists the operand as a 64-bit pair
    assembler_verified, // real GFX10 assembler/disassembler bytes in the harness
};

const char* sc_width_provenance_name(sc_width_provenance p);

struct sc_operand {
    sc_operand_file file = sc_operand_file::unclassified;
    uint32_t index = 0;  // valid for sgpr/vgpr/attribute
    uint32_t field = 0;  // the raw field value, always kept
    uint32_t special = 0; // valid when file == special: the raw field value
    double constant = 0.0;         // valid when file == constant or literal
    bool constant_is_real = false; // constant came from the float half of the table
    // valid when file == hwreg: the packed word is decoded into its three fields
    // (id = bits 5:0, offset = 10:6, size = bits 15:11 + 1; fixture-pinned).
    uint8_t hwreg_id = 0;
    uint8_t hwreg_offset = 0;
    uint8_t hwreg_size = 0;
    sc_operand_width width = sc_operand_width::unknown;
    sc_width_provenance provenance = sc_width_provenance::none;
};

// A 9-bit VOP source field (src0 of VOP1/VOP2/VOPC; GFX10 layout).
sc_operand sc_operand_from_src9(uint32_t field);

// An 8-bit vector field (vdst, vsrc1). These name VGPRs only on GFX10.
sc_operand sc_operand_from_vsrc8(uint32_t field);

// An 8-bit scalar field (SOP1 sdst / ssrc0). 0..105 are SGPRs, 0xFF is a
// literal; the special register numbers (exec = 126, null = 125, ...) are left
// unclassified because this module does not name them.
sc_operand sc_operand_from_ssrc8(uint32_t field);


std::string sc_operand_name(const sc_operand& op); // "s105", "v2", "literal", "field:0x7e"

// VOP3/VOP3P operand modifiers. Field positions below are assembler-verified by
// the conformance matrix (abs/neg/omod/clamp/op_sel each moved by the fixtures
// that print the modifier), except `op_sel_hi`, which no harvested fixture
// exercises and which therefore stays unclaimed.
struct sc_vop3_modifiers {
    bool known = false;         // modifier bits apply to this instruction form
    bool abs[3] = {false, false, false};
    bool neg[3] = {false, false, false};
    bool clamp = false;
    uint8_t omod = 0;           // 0 = *1, 1 = *2, 2 = *4, 3 = *0.5 (div:2)
    bool op_sel_known = false;  // 4-bit op_sel is verified; op_sel_hi is not
    uint8_t op_sel[4] = {0, 0, 0, 0};
    sc_width_provenance provenance = sc_width_provenance::none;
};

// The operands of one instruction, as far as the encoding makes them
// unambiguous. `src0` is suppressed for SDWA/DPP forms, where the 8-bit field
// holds a modifier selector (0xF9 / 0xFA) rather than a register.
//
// `sdst` covers the scalar destination of VOP3 forms: the compare space
// (opcode < 0x100) writes it in the vdst slot, while the six carry opcodes below
// put it in bits 15:8 and keep a vector destination in 7:0.
// SDWA modifier dword (the instruction's second dword). Every field position was
// measured by varying that attribute alone across the 7,772 `_sdwa` fixtures;
// the source registers are decoded from this dword too (src0 in byte 0 with its
// scalar flag at bit 23, src1 in word0 bits 16:9 with its scalar flag at bit 31).
struct sc_sdwa_modifiers {
    bool known = false;
    // False on the compare forms: with no vector destination, bits 15:8 hold the
    // scalar destination instead of dst_sel/dst_unused/clamp/omod (see sc_wave.cpp).
    bool dst_selector_known = false;
    bool sext[2] = {false, false};
    bool neg[2] = {false, false};
    bool abs[2] = {false, false};
    uint8_t dst_sel = 0;   // 10:8
    uint8_t dst_unused = 0; // 12:11
    uint8_t src0_sel = 0;  // 18:16
    uint8_t src1_sel = 0;  // 26:24
    bool clamp = false;    // 13
    uint8_t omod = 0;      // 15:14
    sc_width_provenance provenance = sc_width_provenance::none;
};

// DPP control word (the instruction's second dword). Every position below was
// measured across the 2,308 `_dpp` fixtures by varying one printed attribute at
// a time; anything the fixtures never exercise keeps `known` false.
struct sc_dpp_control {
    bool known = false;
    bool is_dpp8 = false; // dpp8:[...] form (source-field marker 0xE9 / 0xEA)
    // dpp16 quad mode (control bit 16 clear): four 2-bit lane selectors printed
    // quad_perm:[a,b,c,d], lane i in bits (8+2i+1 : 8+2i).
    bool quad = false;
    uint8_t quad_lane[4] = {0, 0, 0, 0};
    // dpp16 row mode (control bit 16 set): op in bits 15:12, amount in 11:8.
    // op 0 shl, 1 shr, 2 ror, 4 mirror (amount 0 = row_mirror, 1 = half_mirror),
    // 5 share, 6 xmask.
    uint8_t row_op = 0;
    uint8_t row_amount = 0;
    // dpp8: eight 3-bit lane selectors, printed lane i in bits (8+3i+2 : 8+3i).
    uint8_t dpp8_lane[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    bool fi = false;         // dpp16 bit 18; dpp8: marker 0xEA in the source field
    bool bound_ctrl = false; // dpp16 bit 19: a bound_ctrl operand was encoded
    bool masks_known = false; // dpp16 only: bits 31:28 and 27:24
    uint8_t row_mask = 0;
    uint8_t bank_mask = 0;
    bool modifiers_known = false; // dpp16 only: bits 20..23
    bool neg[2] = {false, false};
    bool abs[2] = {false, false};
    sc_width_provenance provenance = sc_width_provenance::none;
};

struct sc_instruction_operands {
    uint32_t offset = 0;
    sc_isa_format format = sc_isa_format::unknown;
    bool has_vdst = false;
    bool has_src0 = false;
    bool has_vsrc1 = false;
    bool has_src1 = false; // VOP3 src1 (9-bit field, bits 17:9 of dword 1)
    bool has_src2 = false; // VOP3 src2 (9-bit field, bits 26:18 of dword 1)
    bool has_sdst = false; // VOP3 scalar destination
    sc_operand vdst;
    sc_operand src0;
    sc_operand vsrc1;
    sc_operand src1;
    sc_operand src2;
    sc_operand sdst;
    sc_vop3_modifiers modifiers;

    // Memory-format operands. Only the fields the fixtures pin are present; the
    // bit positions are measured over the harvested gfx10 MC set (see
    // docs/SHADER-PIPELINE-CONFORMANCE.md) and each one matches every row that
    // prints the role with zero violations:
    //   MUBUF  vdst 47:40, vaddr 39:32, srsrc 52:48 (in 4-SGPR units)
    //   MIMG   vdst 47:40, vaddr 39:32, srsrc 52:48 (in 4-SGPR units)
    //   FLAT   vdst 63:56, vaddr 39:32, vdata 47:40, saddr 55:48 (see below)
    //   SMEM   sdata 12:6 (7 bits)
    bool has_mem_vdst = false;
    bool has_mem_vaddr = false;
    bool has_mem_srsrc = false;
    bool has_mem_sdata = false;
    bool has_mem_offset = false; // DS offset, bits 15:0 of dword 0 (6 rows pin it)
    // MIMG dmask (word0 11:8) and NSA address list (dim bits 2:1 != 0): measured on
    // every MIMG fixture row, 247/247 dmask and 100/100 list rows exact.
    bool has_mem_dmask = false;
    uint32_t mem_dmask = 0;
    bool has_mem_nsa = false;
    uint32_t mem_nsa_slots = 0;      // 1 + 4 * extra dwords
    uint32_t mem_nsa_vgpr[10] = {};  // 8-bit vgpr fields, base first
    // DS registers: dword 1 bytes 7:0, 15:8, 23:16, 31:24 (measured; word0 bytes match
    // no printed register on any row). Which byte each printed position takes is a
    // per-mnemonic permutation, so the words carry the values and the text carries order.
    bool has_mem_ds_roles = false;
    uint32_t mem_ds_role[4] = {};
    // MUBUF vaddr (dword1 7:0), offset (word0 11:0 - bits 12..15 are flag bits) and
    // soffset (dword1 31:24). Measured on all 781 rows: vaddr 100/100 registers and
    // 679/679 `off` rows at 0, offset 717/717 (15:0 disagrees on 193), soffset sN
    // identity 590/590; constant soffsets use an 8-bit encoding table (0x80 = 0).
    bool has_mem_mubuf_vaddr = false;
    bool has_mem_mubuf_offset = false;
    bool has_mem_soffset = false;
    uint32_t mem_mubuf_vaddr = 0;
    uint32_t mem_mubuf_offset = 0;
    uint32_t mem_soffset = 0;
    // SMEM: sbase word0 5:0 (in SGPR pairs, printed value = field * 2), soffset
    // dword1 31:25, offset dword1 20:0. Measured: sbase 440/440, soffset 308/308 sN,
    // offset 106/106 (7:0, 12:0 and 15:0 disagree on 51, 34 and 34 rows).
    bool has_mem_sbase = false;
    bool has_mem_smem_soffset = false;
    bool has_mem_smem_offset = false;
    uint32_t mem_sbase = 0;
    uint32_t mem_smem_soffset = 0;
    uint32_t mem_smem_offset = 0;
    // MIMG ssamp: dword1 25:21 in 4-SGPR units. 103/103 rows that print a sampler
    // operand match; 20:24 and 22:27 disagree on all 103, so the boundaries are pinned.
    bool has_mem_ssamp = false;
    uint32_t mem_ssamp = 0;
    // FLAT/GLOBAL/SCRATCH: dword1 bytes - vaddr 39:32, vdata 47:40, saddr 55:48,
    // vdst 63:56. Pinned by the four-operand rows: `global_atomic_fcmpswap v5,
    // v[1:2], v[2:3], off` is dword1 0x057D0201, i.e. byte3=5 (vdst), byte0=1
    // (vaddr), byte1=2 (vdata), byte2=0x7D (off in the saddr slot); 64 rows print
    // (vaddr, vdata) as bytes 0/1 and 42 more add the 0x7D saddr slot. 0x7D and
    // 0x7F are the `off` encodings; vdst is printed only when it is not zero.
    bool has_mem_flat = false;
    bool mem_flat_saddr_off = false;
    uint32_t mem_flat_vdst = 0;
    uint32_t mem_flat_vaddr = 0;
    uint32_t mem_flat_vdata = 0;
    uint32_t mem_flat_saddr = 0;
    sc_operand mem_vdst;
    sc_operand mem_vaddr;
    sc_operand mem_srsrc;
    sc_operand mem_sdata;
    uint32_t mem_offset = 0;
    sc_sdwa_modifiers sdwa;
    sc_dpp_control dpp;
};

// Counts across a walked stream. Only operand references that this module
// classified are counted; `unclassified` is counted separately so a stream full
// of inline constants cannot look like a stream full of registers.
struct sc_register_census {
    uint32_t instructions = 0;      // instructions that had decodable operands
    uint32_t sgpr_refs = 0;
    uint32_t vgpr_refs = 0;
    uint32_t literal_refs = 0;
    uint32_t attribute_refs = 0;
    uint32_t special_refs = 0;
    uint32_t constant_refs = 0;
    uint32_t unclassified_refs = 0;
    uint32_t b32_refs = 0;          // operands with a known 32-bit width
    uint32_t b64_refs = 0;          // operands with a known 64-bit (pair) width
    uint32_t width_unknown_refs = 0;
    uint32_t max_sgpr = 0;
    uint32_t max_vgpr = 0;
    bool any_sgpr = false;
    bool any_vgpr = false;
};

// Wave-size facts, kept in three separate buckets so they cannot be confused.
struct sc_wave_facts {
    // Hardware capability, supplied by the caller from the Phase 2 device probe.
    // Zero means "not supplied".
    uint32_t device_wave_lane_min = 0;
    uint32_t device_wave_lane_max = 0;

    // What the instruction bytes can say. Nothing so far, and that is a finding,
    // not a gap in the parser: no operand field carries a wave width.
    bool wave_size_known = false;
    uint32_t wave_size = 0;

    std::vector<std::string> evidence;       // observed: / documented: / inferred:
    std::vector<std::string> open_questions;
};

struct sc_wave_decode {
    std::vector<sc_instruction_operands> instructions;
    sc_register_census census;
    sc_wave_facts wave;
    std::string to_json() const; // summary + first operands
};

// Decodes operands over a Phase 4 walk. Stops at the walk's stop reason by
// construction: it only looks at instructions the walker synced through.
// Decodes one instruction whose word list and dword count the caller supplies,
// so a format can be decoded and asserted without the stream walker sizing it.
sc_instruction_operands sc_decode_instruction(const uint32_t* words, uint32_t count);

sc_wave_decode sc_decode_wave(const sc_isa_walk& walk,
                              uint32_t device_wave_lane_min = 0,
                              uint32_t device_wave_lane_max = 0);

} // namespace sc

#endif // SC_SHADER_WAVE_H
