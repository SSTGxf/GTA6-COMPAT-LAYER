// ScarlettCompat shader pipeline - Phase 5: operand widths and wave semantics.
// MIT. New file; not wired into the boot path.
//
// Encoding facts used here, and where they come from:
//
//   VOP1  bits 31:25 = 0x3F, vdst 24:17, opcode 16:9, src0 8:0
//   VOP2  bit 31 = 0, opcode 30:25, vdst 24:17, vsrc1 16:9, src0 8:0
//   VOPC  bits 31:25 = 0x3E, opcode 24:17, vsrc1 16:9, src0 8:0 (no vdst; VCC)
//
//   A 9-bit source field on GFX10 names SGPRs at 0..255 and VGPRs at 256+n
//   (256..511); 0xFF selects the following literal dword. Pinned against real
//   disassembler/assembler output in the harness:
//       v_mov_b32 v0, s105     -> 7E000269, src0 field 105  -> s105
//       v_cvt_f32_i32 v1, v2   -> 7E020B02, src0 field 258  -> v2
//   The 8-bit vector fields (vdst, vsrc1) name VGPRs only. In SDWA/DPP forms the
//   low byte is a modifier selector (0xF9/0xFA), not a register, so src0 is not
//   reported for those -- that is already how Phase 4 classified their lengths.
//
// Wave size: nothing above carries a wave width. It is a per-shader/per-pipeline
// property, so this module reports it as unknown and explains why, rather than
// inferring it from, say, the presence of a mask instruction.
#include "shader/sc_wave.h"

#include <cstdio>

namespace sc {
namespace {

constexpr uint32_t kSgprMax = 105;           // gfx10 exposes s0..s105
constexpr uint32_t kVgprBase = 256;          // 9-bit field: VGPR vn is 256 + n
constexpr uint32_t kVgprCount = 256;         // v0..v255
constexpr uint32_t kLiteralField9 = 0xFFu;   // selects the following dword
constexpr uint32_t kSrc0Mask9 = 0x1FFu;

std::string hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%x", v);
    return std::string(buf);
}

bool carries_operands(sc_isa_format f) {
    return f == sc_isa_format::vop1 || f == sc_isa_format::vop2 || f == sc_isa_format::vopc ||
           f == sc_isa_format::sop1 || f == sc_isa_format::sop2 || f == sc_isa_format::sopc ||
           f == sc_isa_format::sopk || f == sc_isa_format::sopp || f == sc_isa_format::vop3 ||
           f == sc_isa_format::mubuf || f == sc_isa_format::mimg || f == sc_isa_format::smem ||
           f == sc_isa_format::ds || f == sc_isa_format::flat;
}

// VOP2 forms that own a literal dword in the middle of their operand list: the
// mnemonic prints [dst, src0, imm32, src1] and the literal is the second source,
// so it is decoded as such instead of being left out of the operand list.
constexpr uint16_t kVop2LiteralOpcodes[] = {0x20u, 0x21u, 0x2cu, 0x2du, 0x37u, 0x38u};

bool is_vop2_literal_opcode(uint16_t opcode) {
    for (uint16_t op : kVop2LiteralOpcodes) {
        if (op == opcode) {
            return true;
        }
    }
    return false;
}

// Operand widths are opcode-selected, so only these forms get a width. The
// gfx10 ISA pages list the f64 conversion operands as 64-bit (`vdst_bdb32f`,
// `src_d5cd94`) while their 32-bit side stays a single register; s_mov_b64 is
// pinned to real gfx10 bytes. No other form is assumed anything:
//   s_mov_b64 s[0:1], s[102:103]  -> [0x66,0x04,0x80,0xbe]  (SOP1 opcode 4)
//   s_mov_b64 exec, s[2:3]        -> [0x02,0x04,0xfe,0xbe]
// and the VOP1 f64 conversions are opcodes 3/4/15/16 (v_cvt_i32_f64,
// v_cvt_f64_i32, v_cvt_f32_f64, v_cvt_f64_f32).
struct width_rule {
    sc_isa_format format;
    uint16_t opcode;
    bool dst64;
    bool src0_64;
    sc_width_provenance provenance;
};

// VOP1 opcode 2 is v_readfirstlane_b32: the one VOP1 that writes an SGPR, so its
// vdst field is a scalar index (v_readfirstlane_b32 s103, v1 -> 0x7e0a0567 puts
// 103 in the 8-bit vdst field). Both sides are single registers.
constexpr uint16_t kOpVop1Readfirstlane = 2;

// VOP3 forms whose scalar destination lives in bits 15:8 (leaving a vector
// destination in 7:0) instead of the modifier bits. Verified by the conformance
// matrix: these are exactly the fixtures that print two destinations
// (v_add_co_ci_u32_e64 v5, s[12:13], v1, v2, s[6:7]).
// Measured the same way: 0x16d (v_div_scale_f32), 0x176 (v_mad_u64_u32) and
// 0x177 (v_mad_i64_i32) print a scalar second destination too, and their bits
// 15:8 hold exactly the printed index across all 234 such fixtures.
constexpr uint16_t kSdstHighOpcodes[] = {0x128u, 0x129u, 0x12au, 0x30fu, 0x310u,
                                         0x319u, 0x16du, 0x176u, 0x177u};
constexpr uint16_t kOpVop3Nop = 0x180u;  // v_nop_e64: no operands
constexpr uint16_t kOpVop3InterpMovF32 = 0x202u; // v_interp_mov_f32: param selector source
// DPP source-field markers (replacing the 9-bit source selector).
constexpr uint32_t kSrc0MarkerDpp8 = 0x0E9u;   // dpp8, fi clear
constexpr uint32_t kSrc0MarkerDpp8Fi = 0x0EAu; // dpp8, fi set
constexpr uint16_t kOpVop1Pipeflush = 0x1bu;   // v_pipeflush: no operands
constexpr uint16_t kOpVop1Clrexcp = 0x41u;   // v_clrexcp: no operands

// 0x360 is v_readlane_b32: the only VOP3 form whose destination (7:0) is a
// scalar. Measured: all 8 fixtures print an s-register there, and the field
// matches it.
constexpr uint16_t kOpVop3ScalarVdst = 0x360u;

// The VINTERP-encoded VOP3 forms present in the fixtures, each with a single
// mnemonic: v_interp_p1_f32_e64, p2_f32_e64, mov_f32_e64, p1ll_f16, p1lv_f16,
// p2_f16. Their src0 slot is an attribute selector, not a register.
constexpr uint16_t kInterpOpcodes[] = {0x200u, 0x201u, 0x202u, 0x342u, 0x343u, 0x35au};

// SOPK forms whose 15:0 field is not a plain immediate (measured from the
// fixtures): getreg/setreg carry a packed hwreg there, setreg_imm32 carries the
// hwreg and puts its immediate in the next dword, and s_version prints only the
// immediate with no destination.
constexpr uint16_t kSopkVersion = 0x01u;
constexpr uint16_t kSopkGetregB32 = 0x12u;
constexpr uint16_t kSopkSetregB32 = 0x13u;
constexpr uint16_t kSopkSetregImm32B32 = 0x15u;

// SOPP opcodes whose fixtures print a plain 16-bit number as their only operand:
// nop/endpgm-less forms, the branch family, sleep/halt/prio/trap, perf levels,
// prefetch/clause, round/denorm mode and ttracedata_imm. The packed forms
// (s_waitcnt, s_sendmsg, s_waitcnt_depctr) and the no-operand forms are excluded.
bool sopp_immediate_form(uint16_t opcode) {
    switch (opcode) {
    case 0x00u: // s_nop
    case 0x02u: // s_branch
    case 0x04u: // s_cbranch_scc0
    case 0x05u: // s_cbranch_scc1
    case 0x06u: // s_cbranch_vccz
    case 0x07u: // s_cbranch_vccnz
    case 0x08u: // s_cbranch_execz
    case 0x09u: // s_cbranch_execnz
    case 0x0bu: // s_setkill
    case 0x0du: // s_sethalt
    case 0x0eu: // s_sleep
    case 0x0fu: // s_setprio
    case 0x12u: // s_trap
    case 0x14u: // s_incperflevel
    case 0x15u: // s_decperflevel
    case 0x17u: // s_cbranch_cdbgsys
    case 0x18u: // s_cbranch_cdbguser
    case 0x19u: // s_cbranch_cdbgsys_or_user
    case 0x1au: // s_cbranch_cdbgsys_and_user
    case 0x20u: // s_inst_prefetch
    case 0x21u: // s_clause
    case 0x24u: // s_round_mode
    case 0x25u: // s_denorm_mode
    case 0x28u: // s_ttracedata_imm
        return true;
    default:
        return false;
    }
}

bool is_interp_opcode(uint16_t opcode) {
    for (uint16_t op : kInterpOpcodes) {
        if (op == opcode) {
            return true;
        }
    }
    return false;
}

bool is_sdst_high_opcode(uint16_t opcode) {
    for (uint16_t op : kSdstHighOpcodes) {
        if (op == opcode) {
            return true;
        }
    }
    return false;
}

const width_rule kWidthRules[] = {
    {sc_isa_format::vop1, kOpVop1Readfirstlane, false, false,
     sc_width_provenance::isa_documented},                                        // v_readfirstlane_b32
    {sc_isa_format::sop1, 4, true, true, sc_width_provenance::assembler_verified},  // s_mov_b64
    {sc_isa_format::vop1, 3, false, true, sc_width_provenance::isa_documented},     // v_cvt_i32_f64
    {sc_isa_format::vop1, 4, true, false, sc_width_provenance::isa_documented},     // v_cvt_f64_i32
    {sc_isa_format::vop1, 15, false, true, sc_width_provenance::isa_documented},    // v_cvt_f32_f64
    {sc_isa_format::vop1, 16, true, false, sc_width_provenance::isa_documented},    // v_cvt_f64_f32
};

void set_width(sc_operand& op, bool wide, sc_width_provenance prov) {
    if (op.file != sc_operand_file::sgpr && op.file != sc_operand_file::vgpr) {
        return; // literals and unclassified fields carry no register width
    }
    op.width = wide ? sc_operand_width::b64 : sc_operand_width::b32;
    op.provenance = prov;
}

void apply_width_rule(const sc_isa_instruction& inst, sc_instruction_operands& io) {
    for (const auto& r : kWidthRules) {
        if (r.format != inst.format || r.opcode != inst.opcode) {
            continue;
        }
        if (io.has_vdst) {
            set_width(io.vdst, r.dst64, r.provenance);
        }
        if (io.has_src0) {
            set_width(io.src0, r.src0_64, r.provenance);
        }
        return;
    }
}

void count_width(sc_register_census& c, const sc_operand& op) {
    switch (op.width) {
    case sc_operand_width::b32:
        ++c.b32_refs;
        break;
    case sc_operand_width::b64:
        ++c.b64_refs;
        break;
    case sc_operand_width::unknown:
    default:
        ++c.width_unknown_refs;
        break;
    }
}

void count_operand(sc_register_census& c, const sc_operand& op) {
    count_width(c, op);
    switch (op.file) {
    case sc_operand_file::sgpr:
        ++c.sgpr_refs;
        c.any_sgpr = true;
        if (op.index > c.max_sgpr) {
            c.max_sgpr = op.index;
        }
        break;
    case sc_operand_file::vgpr:
        ++c.vgpr_refs;
        c.any_vgpr = true;
        if (op.index > c.max_vgpr) {
            c.max_vgpr = op.index;
        }
        break;
    case sc_operand_file::literal:
        ++c.literal_refs;
        break;
    case sc_operand_file::attribute:
        ++c.attribute_refs;
        break;
    case sc_operand_file::special:
        ++c.special_refs;
        break;
    case sc_operand_file::constant:
        ++c.constant_refs;
        break;
    default:
        ++c.unclassified_refs;
        break;
    }
}

} // namespace

const char* sc_operand_file_name(sc_operand_file f) {
    switch (f) {
    case sc_operand_file::sgpr: return "sgpr";
    case sc_operand_file::vgpr: return "vgpr";
    case sc_operand_file::literal: return "literal";
    case sc_operand_file::attribute: return "attribute";
    case sc_operand_file::unclassified:
    default: return "unclassified";
    }
}

// Named scalar special registers. These seven field values are exactly the ones
// the harvested fixtures print by name (vcc_lo/vcc_hi/exec_lo/exec_hi/m0 and two
// ttmp registers); the rest of the 106..127 window (flat_scratch, xnack_mask,
// ttmp0..9, null) never appears there, so it stays unclassified rather than being
// filled in from memory.
struct special_entry {
    uint32_t field;
    const char* name;
};

constexpr special_entry kSpecialRegisters[] = {
    {106u, "vcc_lo"}, {107u, "vcc_hi"}, {118u, "ttmp10"},
    {119u, "ttmp11"}, {124u, "m0"},     {126u, "exec_lo"},
    {127u, "exec_hi"},
};

const char* special_register_name(uint32_t field) {
    for (const auto& entry : kSpecialRegisters) {
        if (entry.field == field) {
            return entry.name;
        }
    }
    return nullptr;
}

// GFX10 scalar inline constants: 128..192 are 0..64, 193..208 are -1..-16 and
// 240..247 are {0.5, -0.5, 1.0, -1.0, 2.0, -2.0, 4.0, -4.0}. The fixtures pin
// 0 -> 0x80, -1 -> 0xC1, 0.5 -> 0xF0 and -4.0 -> 0xF7, i.e. both ends of each
// run; the values between are the documented tables those ends bracket.
bool scalar_constant(uint32_t field, double& value, bool& is_real) {
    is_real = false;
    if (field >= 128u && field <= 192u) {
        value = static_cast<double>(field - 128u);
        return true;
    }
    if (field >= 193u && field <= 208u) {
        value = -static_cast<double>(field - 192u);
        return true;
    }
    if (field >= 240u && field <= 247u) {
        static const double kReals[8] = {0.5, -0.5, 1.0, -1.0, 2.0, -2.0, 4.0, -4.0};
        value = kReals[field - 240u];
        is_real = true;
        return true;
    }
    return false;
}

// Shared classifier for the scalar source fields: 8-bit (SOP) or 9-bit (VOP).
// `vgpr_range` is true for the 9-bit form, where 256..511 names a VGPR.
void classify_scalar(sc_operand& op, uint32_t field, bool vgpr_range) {
    op.field = field;
    if (op.field <= kSgprMax) {
        op.file = sc_operand_file::sgpr;
        op.index = op.field;
        return;
    }
    if (op.field == kLiteralField9) {
        op.file = sc_operand_file::literal;
        return;
    }
    if (vgpr_range && op.field >= kVgprBase && op.field < kVgprBase + kVgprCount) {
        op.file = sc_operand_file::vgpr;
        op.index = op.field - kVgprBase;
        return;
    }
    if (special_register_name(op.field) != nullptr) {
        op.file = sc_operand_file::special;
        op.special = op.field;
        return;
    }
    double value = 0.0;
    bool is_real = false;
    if (scalar_constant(op.field, value, is_real)) {
        op.file = sc_operand_file::constant;
        op.constant = value;
        op.constant_is_real = is_real;
        op.provenance = sc_width_provenance::assembler_verified;
        return;
    }
    op.file = sc_operand_file::unclassified;
}

sc_operand sc_operand_from_src9(uint32_t field) {
    sc_operand op;
    classify_scalar(op, field & kSrc0Mask9, true);
    return op;
}

// A 16-bit SOPK/SOPP immediate: the field is the value itself.
sc_operand sc_operand_from_imm16(uint32_t field) {
    sc_operand op;
    op.field = field & 0xffffu;
    op.file = sc_operand_file::constant;
    op.constant = static_cast<double>(op.field);
    op.provenance = sc_width_provenance::assembler_verified;
    return op;
}

// SDWA/DPP source register. The register index sits in the modifier dword, and a
// scalar source is flagged by a bit there instead of by the 9-bit VGPR offset:
// bit 23 for src0, bit 31 for src1 (measured across all 7,772 `_sdwa` fixtures).
sc_operand sc_operand_from_sdwa_source(uint32_t field, bool is_scalar) {
    if (is_scalar) {
        return sc_operand_from_ssrc8(field);
    }
    sc_operand op;
    op.field = field & 0xffu;
    op.file = sc_operand_file::vgpr;
    op.index = op.field;
    return op;
}

// A packed hwreg(id, offset, size) word (SOPK getreg/setreg). The packing is
// pinned by the two measured words: hwreg(52, 8, 3) = 0x1234 and
// hwreg(HW_REG_TBA_HI, 7, 25) = 0xC1D1, i.e. id = bits 5:0, offset = bits 10:6
// and size = bits 15:11 + 1 (the printed size is one-based).
sc_operand sc_operand_from_hwreg(uint32_t field) {
    sc_operand op;
    op.field = field & 0xffffu;
    op.file = sc_operand_file::hwreg;
    op.hwreg_id = static_cast<uint8_t>(op.field & 0x3fu);
    op.hwreg_offset = static_cast<uint8_t>((op.field >> 6) & 0x1fu);
    op.hwreg_size = static_cast<uint8_t>(((op.field >> 11) & 0x1fu) + 1u);
    op.provenance = sc_width_provenance::assembler_verified;
    return op;
}

sc_operand sc_operand_from_vsrc8(uint32_t field) {
    sc_operand op;
    op.field = field & 0xFFu;
    op.file = sc_operand_file::vgpr; // 8-bit vector fields are VGPRs only
    op.index = op.field;
    return op;
}

sc_operand sc_operand_from_ssrc8(uint32_t field) {
    sc_operand op;
    classify_scalar(op, field & 0xFFu, false);
    return op;
}

const char* sc_operand_width_name(sc_operand_width w) {
    switch (w) {
    case sc_operand_width::b32: return "b32";
    case sc_operand_width::b64: return "b64";
    case sc_operand_width::unknown:
    default: return "unknown";
    }
}

const char* sc_width_provenance_name(sc_width_provenance p) {
    switch (p) {
    case sc_width_provenance::isa_documented: return "isa_documented";
    case sc_width_provenance::assembler_verified: return "assembler_verified";
    case sc_width_provenance::none:
    default: return "none";
    }
}

std::string sc_operand_name(const sc_operand& op) {
    switch (op.file) {
    case sc_operand_file::sgpr: return "s" + std::to_string(op.index);
    case sc_operand_file::vgpr: return "v" + std::to_string(op.index);
    case sc_operand_file::literal: return "literal";
    case sc_operand_file::attribute: {
        static const char* kChannels = "xyzw";
        return "attr" + std::to_string(op.index) + "." +
               std::string(1, kChannels[(op.field >> 6) & 3u]);
    }
    case sc_operand_file::special: {
        const char* name = special_register_name(op.special);
        return name != nullptr ? std::string(name) : "field:" + hex(op.field);
    }
    case sc_operand_file::hwreg:
        return "hwreg(" + std::to_string(op.hwreg_id) + ", " + std::to_string(op.hwreg_offset) +
               ", " + std::to_string(op.hwreg_size) + ")";
    case sc_operand_file::param: return "p" + std::to_string(op.index);
    case sc_operand_file::constant: {
        if (op.constant_is_real) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", op.constant);
            return std::string(buf);
        }
        return std::to_string(static_cast<long long>(op.constant));
    }
    case sc_operand_file::unclassified:
    default: return "field:" + hex(op.field);
    }
}


// Memory-format operand fields, each position measured over the harvested gfx10
// MC set with tests/shader/fixtures/measure_memory_operands.py (a range is only
// accepted when the field equals the printed operand in every row that prints
// that role):
//   MUBUF  vdst 47:40, srsrc 52:48 (holds the V# base / 4)
//   MIMG   vdst 47:40, vaddr 39:32, srsrc 52:48 (holds the V# base / 4)
//   SMEM   sdata 12:6 (7 bits)
//   DS     offset 15:0 of dword 0
//   MIMG   dmask 11:8 of dword 0 (247/247 rows that print dmask:0xN, 0 violations)
//   MIMG   NSA list: dim 2:1 != 0; component j is an 8-bit vgpr: base in dword 1
//          7:0, then four per extra dword (100/100 list rows exact, 0 violations)
//   DS     registers are dword 1 bytes 7:0, 15:8, 23:16, 31:24 (1,855/1,855 rows
//          with a printed register; word0 bytes match no register anywhere). Which
//          byte a printed position takes is a per-mnemonic permutation, so the
//          decoder exposes the four values (mem_ds_role) and the caller keeps order.
//   MIMG   ssamp 25:21 (in 4-SGPR units) of dword 1: 103/103 rows that print a
//          sampler, with 20:24 and 22:27 failing on all 103
//   FLAT   vaddr 39:32, vdata 47:40, saddr 55:48, vdst 63:56 of dword 1, with
//          0x7D/0x7F meaning `off` in the saddr slot: 118/110/12/70 rows asserted
// Not filled, because the fixtures cannot isolate the role: MUBUF soffset/offset
// (constant forms use an operand value table), EXP's four uniform vgprs (all 24
// rows encode dword 1 = 0) and MIMG printed widths (a printer rule, not a field).
void fill_memory_operands(const sc_isa_instruction& inst, sc_instruction_operands& io) {
    const uint32_t w1 = inst.dwords[1];
    switch (inst.format) {
    case sc_isa_format::mubuf:
        io.has_mem_vdst = true;
        io.mem_vdst = sc_operand_from_vsrc8((w1 >> 8) & 0xffu);
        io.has_mem_srsrc = true;
        io.mem_srsrc = sc_operand_from_ssrc8(((w1 >> 16) & 0x1fu) * 4u);
        io.has_mem_mubuf_vaddr = true;
        io.mem_mubuf_vaddr = w1 & 0xffu;
        io.has_mem_mubuf_offset = true;
        io.mem_mubuf_offset = inst.dwords[0] & 0xfffu;
        io.has_mem_soffset = true;
        io.mem_soffset = (w1 >> 24) & 0xffu;
        break;
    case sc_isa_format::mimg:
        io.has_mem_vdst = true;
        io.mem_vdst = sc_operand_from_vsrc8((w1 >> 8) & 0xffu);
        io.has_mem_vaddr = true;
        io.mem_vaddr = sc_operand_from_vsrc8(w1 & 0xffu);
        io.has_mem_srsrc = true;
        io.mem_srsrc = sc_operand_from_ssrc8(((w1 >> 16) & 0x1fu) * 4u);
        io.has_mem_ssamp = true;
        io.mem_ssamp = ((w1 >> 21) & 0x1fu) * 4u;
        io.has_mem_dmask = true;
        io.mem_dmask = (inst.dwords[0] >> 8) & 0xfu;
        {
            const uint32_t dim = (inst.dwords[0] >> 1) & 3u;
            if (dim != 0 && inst.words >= 2u + dim) {
                io.has_mem_nsa = true;
                io.mem_nsa_slots = 1u + 4u * dim;
                io.mem_nsa_vgpr[0] = w1 & 0xffu;
                for (uint32_t d = 0; d < dim; ++d) {
                    const uint32_t wd = inst.dwords[2u + d];
                    for (uint32_t b = 0; b < 4u; ++b) {
                        io.mem_nsa_vgpr[1u + 4u * d + b] = (wd >> (8u * b)) & 0xffu;
                    }
                }
            }
        }
        break;
    case sc_isa_format::smem:
        io.has_mem_sdata = true;
        io.mem_sdata = sc_operand_from_ssrc8((inst.dwords[0] >> 6) & 0x7fu);
        io.has_mem_sbase = true;
        io.mem_sbase = (inst.dwords[0] & 0x3fu) * 2u;
        io.has_mem_smem_soffset = true;
        io.mem_smem_soffset = (w1 >> 25) & 0x7fu;
        io.has_mem_smem_offset = true;
        io.mem_smem_offset = w1 & 0x1fffffu;
        break;
    case sc_isa_format::flat:
        io.has_mem_flat = true;
        io.mem_flat_vaddr = w1 & 0xffu;
        io.mem_flat_vdata = (w1 >> 8) & 0xffu;
        io.mem_flat_saddr = (w1 >> 16) & 0xffu;
        io.mem_flat_vdst = (w1 >> 24) & 0xffu;
        io.mem_flat_saddr_off = io.mem_flat_saddr == 0x7du || io.mem_flat_saddr == 0x7fu;
        break;
    case sc_isa_format::ds:
        io.has_mem_offset = true;
        io.mem_offset = inst.dwords[0] & 0xffffu;
        io.has_mem_ds_roles = true;
        io.mem_ds_role[0] = w1 & 0xffu;
        io.mem_ds_role[1] = (w1 >> 8) & 0xffu;
        io.mem_ds_role[2] = (w1 >> 16) & 0xffu;
        io.mem_ds_role[3] = (w1 >> 24) & 0xffu;
        break;
    default:
        break;
    }
}
sc_instruction_operands sc_decode_instruction(const uint32_t* words, uint32_t count) {
    sc_instruction_operands io;
    if (count == 0 || words == nullptr) {
        return io;
    }
    sc_isa_instruction inst = sc_isa_decode_word(words[0]);
    inst.words = count; // the caller owns the length: no length rule is applied here
    // Copy every dword the caller supplies (MIMG NSA puts 8-bit address components
    // in dwords 2..), bounded by the fixed instruction buffer.
    const uint32_t copy = count < 8u ? count : 8u;
    for (uint32_t i = 1; i < copy; ++i) {
        inst.dwords[i] = words[i];
    }
    io.format = inst.format;
    if (carries_operands(inst.format)) {
        fill_memory_operands(inst, io);
    }
    return io;
}
sc_wave_decode sc_decode_wave(const sc_isa_walk& walk, uint32_t device_wave_lane_min,
                              uint32_t device_wave_lane_max) {
    sc_wave_decode d;
    d.wave.device_wave_lane_min = device_wave_lane_min;
    d.wave.device_wave_lane_max = device_wave_lane_max;

    for (const auto& inst : walk.instructions) {
        if (!carries_operands(inst.format)) {
            continue; // only the 32-bit vector forms have verified operand fields
        }
        const uint32_t word = inst.dwords[0];
        sc_instruction_operands io;
        io.offset = inst.offset;
        io.format = inst.format;

        if (inst.format == sc_isa_format::mubuf || inst.format == sc_isa_format::mimg ||
            inst.format == sc_isa_format::smem || inst.format == sc_isa_format::ds) {
            if (inst.words >= 2) {
                fill_memory_operands(inst, io);
            }
            d.instructions.push_back(io);
            continue;
        }

        if (inst.format == sc_isa_format::vop3 && inst.words >= 2) {
            // VOP3 field positions, each pinned by fixtures that vary only that
            // field (see docs/SHADER-PIPELINE-CONFORMANCE.md):
            //   dword0: vdst 7:0, abs 10:8, op_sel 14:11, clamp 15, opcode 25:16
            //   dword1: src0 8:0, src1 17:9, src2 26:18, omod 28:27, neg 31:29
            const uint32_t w0 = inst.dwords[0];
            const uint32_t w1 = inst.dwords[1];
            const uint16_t op = inst.opcode;
            const bool compare = inst.opcode_known && op < 0x100u;
            const bool sdst_high = inst.opcode_known && is_sdst_high_opcode(op);
            const bool scalar_vdst = inst.opcode_known && op == kOpVop3ScalarVdst;
            const bool is_nop = inst.opcode_known && op == kOpVop3Nop;

            if (is_nop) {
                // v_nop_e64 names no operands; its unused fields decode as inline
                // constants, which must not become registers in the census.
                d.instructions.push_back(io);
                continue;
            }

            if (compare) {
                // Compare space: the scalar destination is in the vdst slot and
                // there is no vector destination.
                io.has_sdst = true;
                io.sdst = sc_operand_from_ssrc8(w0 & 0xffu);
            } else if (scalar_vdst) {
                io.has_sdst = true;
                io.sdst = sc_operand_from_ssrc8(w0 & 0xffu);
            } else {
                io.has_vdst = true;
                io.vdst = sc_operand_from_vsrc8(w0 & 0xffu);
                if (sdst_high) {
                    io.has_sdst = true;
                    io.sdst = sc_operand_from_ssrc8((w0 >> 8) & 0xffu);
                }
            }

            // 0xFF in any of the three slots selects the appended literal dword,
            // so all three use the same 9-bit classifier as the 32-bit forms.
            io.has_src0 = true;
            if (is_interp_opcode(op)) {
                // v_interp_* put the attribute selector in the src0 slot and the
                // VGPRs in src1/src2, even though the disassembler prints the
                // attribute last. Measured over all 67 interp fixtures:
                //   index = field & 0x3f, channel = bits 7:6 (x/y/z/w = 0..3)
                //   bit 8 does not match the printed `high` flag, so it stays
                //   unclaimed.
                sc_operand attr;
                attr.field = w1 & 0x1ffu;
                attr.index = attr.field & 0x3fu;
                attr.file = sc_operand_file::attribute;
                attr.provenance = sc_width_provenance::assembler_verified;
                io.src0 = attr;
            } else {
                io.src0 = sc_operand_from_src9(w1 & 0x1ffu);
            }
            io.has_src1 = true;
            if (op == kOpVop3InterpMovF32) {
                // v_interp_mov_f32 (only this interp form) takes a param selector as
                // its second source, not a register: measured 0 -> p10, 1 -> p20,
                // 2 -> p0 over the 14 fixtures that print one. The other interp
                // forms keep a plain VGPR there (v_interp_p1ll_f16 v5, v2, ...).
                static constexpr uint32_t kInterpParams[] = {10u, 20u, 0u};
                const uint32_t sel = (w1 >> 9) & 0x1ffu;
                sc_operand p;
                p.field = sel;
                if (sel < 3u) {
                    p.file = sc_operand_file::param;
                    p.index = kInterpParams[sel];
                    p.provenance = sc_width_provenance::assembler_verified;
                }
                io.src1 = p;
            } else {
                io.src1 = sc_operand_from_src9((w1 >> 9) & 0x1ffu);
            }
            io.has_src2 = true;
            io.src2 = sc_operand_from_src9((w1 >> 18) & 0x1ffu);

            if (!compare && !sdst_high && !scalar_vdst) {
                // Modifier bits only exist on the plain VOP3 forms. On compare
                // and carry forms those same bits hold the scalar destination.
                io.modifiers.known = true;
                io.modifiers.provenance = sc_width_provenance::assembler_verified;
                io.modifiers.clamp = ((w0 >> 15) & 1u) != 0;
                for (int i = 0; i < 3; ++i) {
                    io.modifiers.abs[i] = ((w0 >> (8 + i)) & 1u) != 0;
                    io.modifiers.neg[i] = ((w1 >> (29 + i)) & 1u) != 0;
                }
                io.modifiers.op_sel_known = true; // 4 bits at 14:11, fixture-pinned
                for (int i = 0; i < 4; ++i) {
                    io.modifiers.op_sel[i] = static_cast<uint8_t>((w0 >> (11 + i)) & 1u);
                }
                io.modifiers.omod = static_cast<uint8_t>((w1 >> 27) & 0x3u);
            }
        } else if (inst.format == sc_isa_format::sop1) {
            // SOP1: sdst 22:16 (7 bits), opcode 15:8, ssrc0 7:0.
            io.has_vdst = true;
            io.vdst = sc_operand_from_ssrc8((word >> 16) & 0x7fu);
            io.has_src0 = true;
            io.src0 = sc_operand_from_ssrc8(word & 0xffu);
        } else if (inst.format == sc_isa_format::sop2) {
            // SOP2: sdst 22:16 (7 bits), opcode 29:23, ssrc1 15:8, ssrc0 7:0.
            // Measured over all 1,656 sop2 fixtures: every plain register in the
            // printed `dst, src0, src1` list sits in exactly those fields, with no
            // exception. Scalar forms report their destination in vdst, which is
            // the convention SOP1 already uses here.
            io.has_vdst = true;
            io.vdst = sc_operand_from_ssrc8((word >> 16) & 0x7fu);
            io.has_src0 = true;
            io.src0 = sc_operand_from_ssrc8(word & 0xffu);
            io.has_src1 = true;
            io.src1 = sc_operand_from_ssrc8((word >> 8) & 0xffu);
        } else if (inst.format == sc_isa_format::sopc) {
            // SOPC: opcode 22:16, ssrc1 15:8, ssrc0 7:0. It writes SCC, so the
            // mnemonic prints no destination at all. Measured over all 342 sopc
            // fixtures: every plain register matches those two fields.
            io.has_src0 = true;
            io.src0 = sc_operand_from_ssrc8(word & 0xffu);
            io.has_src1 = true;
            io.src1 = sc_operand_from_ssrc8((word >> 8) & 0xffu);
        } else if (inst.format == sc_isa_format::sopk) {
            // SOPK: encoding 31:28 = 0xB, opcode 27:23, sdst 22:16, simm16 15:0.
            // Measured over all 203 sopk fixtures: the register operand is 22:16
            // (vcc_lo/exec_hi/m0/s0/s105 all match exactly) and the second printed
            // operand is 15:0.
            const uint16_t op = inst.opcode;
            const bool known = inst.opcode_known;
            const bool version = known && op == kSopkVersion;
            const bool getreg = known && op == kSopkGetregB32;
            const bool setreg = known && op == kSopkSetregB32;
            const bool setreg_imm = known && op == kSopkSetregImm32B32;
            if (!version && !setreg_imm) {
                // On s_setreg_b32 this field is the source register, not a
                // destination; the harness knows that from the printed order.
                io.has_sdst = true;
                io.sdst = sc_operand_from_ssrc8((word >> 16) & 0x7fu);
            }
            if (getreg || setreg) {
                io.has_src0 = true;
                io.src0 = sc_operand_from_hwreg(word & 0xffffu);
            } else if (setreg_imm) {
                io.has_src0 = true;
                io.src0 = sc_operand_from_hwreg(word & 0xffffu);
                io.has_src1 = true;
                io.src1 = sc_operand_from_imm16(0u);
                io.src1.constant = static_cast<double>(inst.words >= 2 ? inst.dwords[1] : 0u);
                io.src1.field = inst.words >= 2 ? inst.dwords[1] : 0u;
            } else {
                io.has_src0 = true;
                io.src0 = sc_operand_from_imm16(word & 0xffffu);
            }
        } else if (inst.format == sc_isa_format::sopp) {
            // SOPP: opcode 22:16, simm16 15:0. Only the forms whose fixtures print
            // one plain number get an immediate operand; the packed encodings
            // (s_waitcnt, s_sendmsg, s_waitcnt_depctr) and the no-operand forms
            // stay undecoded rather than reporting a number that is not one.
            if (inst.opcode_known && sopp_immediate_form(inst.opcode)) {
                io.has_src0 = true;
                io.src0 = sc_operand_from_imm16(word & 0xffffu);
            }
        } else if (inst.format == sc_isa_format::vop1 && inst.opcode_known &&
                   (inst.opcode == 0u || inst.opcode == kOpVop1Pipeflush ||
                    inst.opcode == kOpVop1Clrexcp)) {
            // v_nop / v_pipeflush / v_clrexcp print no operands at all, so their
            // vdst and src0 fields are unused rather than operands.
            d.instructions.push_back(io);
            ++d.census.instructions;
            continue;
        } else {
            if (inst.format != sc_isa_format::vopc) {
                io.has_vdst = true;
                // v_readfirstlane_b32 is the documented exception: its vdst field
                // names a scalar register, not a vector one.
                if (inst.format == sc_isa_format::vop1 && inst.opcode_known &&
                    inst.opcode == kOpVop1Readfirstlane) {
                    io.vdst = sc_operand_from_ssrc8((word >> 17) & 0xffu);
                } else {
                    io.vdst = sc_operand_from_vsrc8((word >> 17) & 0xffu);
                }
            }
            if (inst.format == sc_isa_format::vop2 || inst.format == sc_isa_format::vopc) {
                io.has_vsrc1 = true;
                io.vsrc1 = sc_operand_from_vsrc8((word >> 9) & 0xffu);
            }
            const bool modifier_form = inst.extra == sc_isa_extra::sdwa || inst.extra == sc_isa_extra::dpp;
            if (modifier_form) {
                // The low byte of the source field is a marker (0xF9/0xFA); the
                // register lives in the modifier dword, byte 0.
                const uint32_t w1 = inst.dwords[1];
                // Only SDWA has a scalar flag at bit 23; in a DPP word that bit is
                // part of the control field, so DPP sources are plain VGPRs (the
                // fixtures print DPP sources as vN only).
                const bool sdwa = inst.extra == sc_isa_extra::sdwa;
                io.has_src0 = true;
                io.src0 = sc_operand_from_sdwa_source(w1 & 0xffu, sdwa && ((w1 >> 23) & 1u) != 0);
                if (inst.extra == sc_isa_extra::dpp) {
                    // The source-field marker picks the form: 0xFA is DPP16, and
                    // 0xE9 / 0xEA are DPP8 (the 0xEA marker is its fi flag). Both
                    // carry the source register in dword1 byte 0.
                    const uint32_t marker = word & kSrc0Mask9;
                    io.dpp.known = true;
                    io.dpp.provenance = sc_width_provenance::assembler_verified;
                    if (marker == kSrc0MarkerDpp8 || marker == kSrc0MarkerDpp8Fi) {
                        io.dpp.is_dpp8 = true;
                        io.dpp.fi = marker == kSrc0MarkerDpp8Fi;
                        for (int i = 0; i < 8; ++i) {
                            io.dpp.dpp8_lane[i] = static_cast<uint8_t>((w1 >> (8 + 3 * i)) & 7u);
                        }
                    } else {
                        const uint32_t control = (w1 >> 8) & 0x1ffu;
                        io.dpp.quad = (control & 0x100u) == 0;
                        if (io.dpp.quad) {
                            for (int i = 0; i < 4; ++i) {
                                io.dpp.quad_lane[i] = static_cast<uint8_t>((control >> (2 * i)) & 3u);
                            }
                        } else {
                            io.dpp.row_op = static_cast<uint8_t>((control >> 4) & 0xfu);
                            io.dpp.row_amount = static_cast<uint8_t>(control & 0xfu);
                        }
                        io.dpp.fi = ((w1 >> 18) & 1u) != 0;
                        io.dpp.bound_ctrl = ((w1 >> 19) & 1u) != 0;
                        io.dpp.modifiers_known = true;
                        for (int i = 0; i < 2; ++i) {
                            io.dpp.neg[i] = ((w1 >> (20 + 2 * i)) & 1u) != 0;
                            io.dpp.abs[i] = ((w1 >> (21 + 2 * i)) & 1u) != 0;
                        }
                        io.dpp.masks_known = true;
                        io.dpp.bank_mask = static_cast<uint8_t>((w1 >> 24) & 0xfu);
                        io.dpp.row_mask = static_cast<uint8_t>((w1 >> 28) & 0xfu);
                    }
                }
                if (sdwa) {
                    io.sdwa.known = true;
                    io.sdwa.provenance = sc_width_provenance::assembler_verified;
                    io.sdwa.src0_sel = static_cast<uint8_t>((w1 >> 16) & 7u);
                    io.sdwa.src1_sel = static_cast<uint8_t>((w1 >> 24) & 7u);
                    if (inst.format == sc_isa_format::vopc) {
                        // A compare form has no vector destination, so the same bits
                        // carry the scalar destination rather than a destination
                        // selector: {bit 15 = SGPR flag, bits 14:8 = field}. Measured
                        // on the 3,452 compare rows that print one: s6 / s[6:7] ->
                        // 0x86, s8 -> 0x88, s100 -> 0xE4, and vcc / vcc_lo -> 0x00
                        // (field 0 with the flag clear is the vcc pair, 106).
                        const uint32_t sdst_field = (w1 >> 8) & 0xffu;
                        io.has_sdst = true;
                        if ((sdst_field & 0x80u) != 0) {
                            io.sdst = sc_operand_from_ssrc8(sdst_field & 0x7fu);
                        } else if (sdst_field == 0u) {
                            sc_operand vcc;
                            vcc.field = 0u;
                            vcc.file = sc_operand_file::special;
                            vcc.special = 106u;
                            vcc.provenance = sc_width_provenance::assembler_verified;
                            io.sdst = vcc;
                        }
                        // Any other encoding has no fixture, so it stays
                        // unclassified with its raw field kept.
                    } else {
                        io.sdwa.dst_selector_known = true;
                        io.sdwa.dst_sel = static_cast<uint8_t>((w1 >> 8) & 7u);
                        io.sdwa.dst_unused = static_cast<uint8_t>((w1 >> 11) & 3u);
                        io.sdwa.clamp = ((w1 >> 13) & 1u) != 0;
                        io.sdwa.omod = static_cast<uint8_t>((w1 >> 14) & 3u);
                    }
                    io.sdwa.sext[0] = ((w1 >> 19) & 1u) != 0;
                    io.sdwa.neg[0] = ((w1 >> 20) & 1u) != 0;
                    io.sdwa.abs[0] = ((w1 >> 21) & 1u) != 0;
                    io.sdwa.sext[1] = ((w1 >> 27) & 1u) != 0;
                    io.sdwa.neg[1] = ((w1 >> 28) & 1u) != 0;
                    io.sdwa.abs[1] = ((w1 >> 29) & 1u) != 0;
                    if (inst.format == sc_isa_format::vop2 || inst.format == sc_isa_format::vopc) {
                        // src1 keeps the normal 8-bit vector field in word0; its
                        // scalar flag is bit 31 of the modifier dword.
                        io.has_vsrc1 = true;
                        io.vsrc1 = sc_operand_from_sdwa_source((word >> 9) & 0xffu, ((w1 >> 31) & 1u) != 0);
                    }
                }
            } else {
                io.has_src0 = true;
                io.src0 = sc_operand_from_src9(word & kSrc0Mask9);
            }
            if (inst.format == sc_isa_format::vop2 && inst.opcode_known &&
                is_vop2_literal_opcode(inst.opcode) && inst.extra == sc_isa_extra::literal) {
                io.has_src1 = true;
                io.src1.file = sc_operand_file::literal;
                io.src1.constant = static_cast<double>(inst.literal_value);
                io.src1.provenance = sc_width_provenance::assembler_verified;
            }
        }

        // A literal operand's value is the instruction's extra dword, so the
        // operand can be compared against the number the mnemonic printed.
        auto apply_literal = [&](sc_operand& op) {
            if (op.file == sc_operand_file::literal) {
                op.constant = static_cast<double>(inst.literal_value);
                op.constant_is_real = false;
                op.provenance = sc_width_provenance::assembler_verified;
            }
        };
        if (io.has_vdst) apply_literal(io.vdst);
        if (io.has_src0) apply_literal(io.src0);
        if (io.has_vsrc1) apply_literal(io.vsrc1);
        if (io.has_src1) apply_literal(io.src1);
        if (io.has_src2) apply_literal(io.src2);
        if (io.has_sdst) apply_literal(io.sdst);

        if (inst.opcode_known) {
            apply_width_rule(inst, io);
        }

        if (io.has_vdst) {
            count_operand(d.census, io.vdst);
        }
        if (io.has_src0) {
            count_operand(d.census, io.src0);
        }
        if (io.has_vsrc1) {
            count_operand(d.census, io.vsrc1);
        }
        if (io.has_src1) {
            count_operand(d.census, io.src1);
        }
        if (io.has_src2) {
            count_operand(d.census, io.src2);
        }
        if (io.has_sdst) {
            count_operand(d.census, io.sdst);
        }

        d.instructions.push_back(io);
        ++d.census.instructions;
    }

    d.wave.evidence.push_back("observed: " + std::to_string(d.census.instructions) +
                              " instruction(s) with decodable vector operands: " +
                              std::to_string(d.census.sgpr_refs) + " SGPR, " +
                              std::to_string(d.census.vgpr_refs) + " VGPR, " +
                              std::to_string(d.census.literal_refs) + " literal, " +
                              std::to_string(d.census.attribute_refs) + " attribute, " +
                              std::to_string(d.census.special_refs) + " special, " +
                              std::to_string(d.census.constant_refs) + " constant, " +
                              std::to_string(d.census.unclassified_refs) + " unclassified reference(s)");
    d.wave.evidence.push_back(
        "documented: GFX10 9-bit VOP source field = SGPR (0..105), VGPR (256+n), 0xFF literal; 8-bit "
        "vector fields are VGPRs only (verified against assembler output in the harness)");
    if (device_wave_lane_min != 0 || device_wave_lane_max != 0) {
        d.wave.evidence.push_back("observed (device): wave lane count " +
                                  std::to_string(device_wave_lane_min) + ".." +
                                  std::to_string(device_wave_lane_max) + " from the Phase 2 adapter probe");
    }
    d.wave.evidence.push_back(
        "inferred: wave size is a per-shader/per-pipeline property and no instruction field carries it; "
        "wave_size_known stays false until a container or corpus record supplies the value");

    d.wave.open_questions.push_back(
        "per-shader wave size must come from the shader record (container metadata or corpus capture), not "
        "from instruction bytes; nothing here assumes wave64");
    d.wave.open_questions.push_back(
        "inline constants and the named special registers are decoded only for the values the harvested "
        "fixtures print by name; flat_scratch, xnack_mask, ttmp0..9 and null stay unclassified because no "
        "fixture shows them");
    if (walk.stop != sc_isa_stop::terminator) {
        d.wave.open_questions.push_back(
            "the walk stopped (" + std::string(sc_isa_stop_name(walk.stop)) +
            "), so this census covers only the verified prefix");
    }

    return d;
}

std::string sc_wave_decode::to_json() const {
    std::string s = "{";
    s += "\"instructions\":" + std::to_string(census.instructions);
    s += ",\"sgpr_refs\":" + std::to_string(census.sgpr_refs);
    s += ",\"vgpr_refs\":" + std::to_string(census.vgpr_refs);
    s += ",\"literal_refs\":" + std::to_string(census.literal_refs);
    s += ",\"attribute_refs\":" + std::to_string(census.attribute_refs);
    s += ",\"special_refs\":" + std::to_string(census.special_refs);
    s += ",\"constant_refs\":" + std::to_string(census.constant_refs);
    s += ",\"unclassified_refs\":" + std::to_string(census.unclassified_refs);
    s += ",\"b32_refs\":" + std::to_string(census.b32_refs);
    s += ",\"b64_refs\":" + std::to_string(census.b64_refs);
    s += ",\"width_unknown_refs\":" + std::to_string(census.width_unknown_refs);
    s += ",\"max_sgpr\":" + std::to_string(census.max_sgpr);
    s += ",\"max_vgpr\":" + std::to_string(census.max_vgpr);
    s += ",\"wave_size_known\":";
    s += wave.wave_size_known ? "true" : "false";
    s += ",\"wave_size\":" + std::to_string(wave.wave_size);
    s += ",\"first\":[";
    const size_t shown = instructions.size() < 8 ? instructions.size() : 8;
    for (size_t i = 0; i < shown; ++i) {
        if (i != 0) {
            s.push_back(',');
        }
        const sc_instruction_operands& io = instructions[i];
        s += "{\"offset\":" + std::to_string(io.offset);
        s += ",\"vdst\":\"" + (io.has_vdst ? sc_operand_name(io.vdst) : std::string("-")) + "\"";
        s += ",\"src0\":\"" + (io.has_src0 ? sc_operand_name(io.src0) : std::string("-")) + "\"";
        s += ",\"vsrc1\":\"" + (io.has_vsrc1 ? sc_operand_name(io.vsrc1) : std::string("-")) + "\"";
        s += ",\"dst_width\":\"" +
             std::string(io.has_vdst ? sc_operand_width_name(io.vdst.width) : "-") + "\"";
        s += ",\"src0_width\":\"" +
             std::string(io.has_src0 ? sc_operand_width_name(io.src0.width) : "-") + "\"}";
    }
    s += "]}";
    return s;
}

} // namespace sc
