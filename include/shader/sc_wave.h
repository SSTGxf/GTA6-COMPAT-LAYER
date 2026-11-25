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
    uint32_t index = 0; // valid for sgpr/vgpr
    uint32_t field = 0; // the raw field value, always kept
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
sc_wave_decode sc_decode_wave(const sc_isa_walk& walk,
                              uint32_t device_wave_lane_min = 0,
                              uint32_t device_wave_lane_max = 0);

} // namespace sc

#endif // SC_SHADER_WAVE_H
