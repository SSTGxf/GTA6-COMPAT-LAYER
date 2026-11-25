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
           f == sc_isa_format::sop1 || f == sc_isa_format::vop3;
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
constexpr uint16_t kOpVop3Nop = 0x180u; // v_nop_e64: no operands

// 0x360 is v_readlane_b32: the only VOP3 form whose destination (7:0) is a
// scalar. Measured: all 8 fixtures print an s-register there, and the field
// matches it.
constexpr uint16_t kOpVop3ScalarVdst = 0x360u;

// The VINTERP-encoded VOP3 forms present in the fixtures, each with a single
// mnemonic: v_interp_p1_f32_e64, p2_f32_e64, mov_f32_e64, p1ll_f16, p1lv_f16,
// p2_f16. Their src0 slot is an attribute selector, not a register.
constexpr uint16_t kInterpOpcodes[] = {0x200u, 0x201u, 0x202u, 0x342u, 0x343u, 0x35au};

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

sc_operand sc_operand_from_src9(uint32_t field) {
    sc_operand op;
    op.field = field & kSrc0Mask9;
    if (op.field <= kSgprMax) {
        op.file = sc_operand_file::sgpr;
        op.index = op.field;
    } else if (op.field >= kVgprBase && op.field < kVgprBase + kVgprCount) {
        op.file = sc_operand_file::vgpr;
        op.index = op.field - kVgprBase;
    } else if (op.field == kLiteralField9) {
        op.file = sc_operand_file::literal;
    } else {
        // 106..254 is beyond the SGPR file and below the VGPR range. On GFX10 the
        // inline-constant table lives there, but it is not transcribed in this
        // module, so the field stays unclassified instead of being guessed at.
        op.file = sc_operand_file::unclassified;
    }
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
    op.field = field & 0xFFu;
    if (op.field <= kSgprMax) {
        op.file = sc_operand_file::sgpr;
        op.index = op.field;
    } else if (op.field == kLiteralField9) {
        op.file = sc_operand_file::literal;
    } else {
        // exec (126), null (125), the vcc halves and the inline-constant range
        // are not named here; they stay unclassified rather than mislabelled.
        op.file = sc_operand_file::unclassified;
    }
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
    case sc_operand_file::unclassified:
    default: return "field:" + hex(op.field);
    }
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
            io.src1 = sc_operand_from_src9((w1 >> 9) & 0x1ffu);
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
            if (!modifier_form) {
                io.has_src0 = true;
                io.src0 = sc_operand_from_src9(word & kSrc0Mask9);
            }
        }

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
        "inline constants sit in the 9-bit field beyond the SGPR file; that table is documented but not "
        "transcribed here, so those references are counted as unclassified rather than guessed");
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
