// ScarlettCompat shader pipeline - Phase 4: RDNA2 instruction decode (MIT).
//
// Encoding table (GFX10, documented). Verified against assembler output for the
// formats marked [v] in the harness; the rest come from the published layout
// tables and are the reason a decode failure stops the walk rather than guessing.
//
//   SOP2    bits 31:30 = 10, opcode 29:23, sdst 22:16, ssrc1 15:8, ssrc0 7:0
//   SOP1    bits 31:23 = 0x17D, sdst 22:16, opcode 15:8, ssrc0 7:0
//   SOPC    bits 31:23 = 0x17E, opcode 22:16, ssrc1 15:8, ssrc0 7:0
//   SOPP    bits 31:23 = 0x17F, opcode 22:16, simm16 15:0
//   SOPK    bits 31:28 = 0xB, opcode 27:23, sdst 22:16, simm16 15:0
//   VOP2    bit 31 = 0, opcode 30:25, vdst 24:17, vsrc1 16:9, src0 8:0
//   VOP1    bits 31:25 = 0x3F, vdst 24:17, opcode 16:9, src0 8:0
//   VOPC    bits 31:25 = 0x3E, opcode 24:17, vsrc1 16:9, src0 8:0
//   VOP3    bits 31:26 = 0x34 (VOP3a) / 0x35 (VOP3b)         64-bit
//   VOP3P   bits 31:26 = 0x33                                64-bit
//   VINTRP  bits 31:26 = 0x32                                32-bit
//   DS      bits 31:26 = 0x36                                64-bit
//   FLAT    bits 31:26 = 0x37 (FLAT/GLOBAL/SCRATCH)          64-bit
//   MUBUF   bits 31:26 = 0x38                                64-bit
//   MTBUF   bits 31:26 = 0x3A                                64-bit
//   MIMG    bits 31:26 = 0x3C                                64-bit + NSA words
//   SMEM    bits 31:26 = 0x3D                                64-bit
//   EXP     bits 31:26 = 0x3E                                64-bit
//
// Length rules: scalar ALU (SOP1/SOP2/SOPC) is one dword plus one literal dword
// when the 8-bit source field is 0xFF [v]; 32-bit VOP forms are one dword plus
// one dword when src0 is 0xFF (literal) [v], 0xF9 (SDWA) [v] or 0xFA (DPP);
// everything listed as 64-bit above has no extra words.
#include "shader/sc_isa.h"

#include <cstdio>

namespace sc {
namespace {

constexpr uint32_t kSop1Encoding = 0x17Du;
constexpr uint32_t kSopcEncoding = 0x17Eu;
constexpr uint32_t kSoppEncoding = 0x17Fu;
constexpr uint32_t kSopkEncoding = 0xBu;
constexpr uint32_t kLiteralSrc8 = 0xFFu; // SOP source field selects a literal dword
constexpr uint32_t kLiteralSrc9 = 0xFFu; // VOP src0 selects a literal dword
constexpr uint32_t kSdwaSrc9 = 0xF9u;
constexpr uint32_t kDppSrc9 = 0xFAu;
constexpr uint32_t kEndpgmOpcode = 1u;

uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void json_escape(std::string& out, const std::string& s) {
    out.push_back('"');
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(ch);
            }
            break;
        }
    }
    out.push_back('"');
}

std::string hex8(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08x", v);
    return std::string(buf);
}

// 32-bit VOP forms: one extra dword for a literal, an SDWA selection or a DPP
// modifier, all signalled in src0.
void vop_extra(uint32_t word, sc_isa_instruction& inst) {
    const uint32_t src0 = word & 0x1ffu;
    if (src0 == kLiteralSrc9) {
        inst.words = 2;
        inst.extra = sc_isa_extra::literal;
    } else if (src0 == kSdwaSrc9) {
        inst.words = 2;
        inst.extra = sc_isa_extra::sdwa;
    } else if (src0 == kDppSrc9) {
        inst.words = 2;
        inst.extra = sc_isa_extra::dpp;
    } else {
        inst.words = 1;
    }
}

// Scalar ALU: one extra dword when the 8-bit source field holds 0xFF.
void sop_extra(uint32_t word, sc_isa_instruction& inst) {
    if ((word & 0xffu) == kLiteralSrc8) {
        inst.words = 2;
        inst.extra = sc_isa_extra::literal;
    } else {
        inst.words = 1;
    }
}

} // namespace

const char* sc_isa_format_name(sc_isa_format f) {
    switch (f) {
    case sc_isa_format::sop1: return "sop1";
    case sc_isa_format::sop2: return "sop2";
    case sc_isa_format::sopc: return "sopc";
    case sc_isa_format::sopp: return "sopp";
    case sc_isa_format::sopk: return "sopk";
    case sc_isa_format::vop1: return "vop1";
    case sc_isa_format::vop2: return "vop2";
    case sc_isa_format::vopc: return "vopc";
    case sc_isa_format::vop3: return "vop3";
    case sc_isa_format::vop3p: return "vop3p";
    case sc_isa_format::vintrp: return "vintrp";
    case sc_isa_format::smem: return "smem";
    case sc_isa_format::ds: return "ds";
    case sc_isa_format::mubuf: return "mubuf";
    case sc_isa_format::mtbuf: return "mtbuf";
    case sc_isa_format::mimg: return "mimg";
    case sc_isa_format::flat: return "flat";
    case sc_isa_format::exp: return "exp";
    case sc_isa_format::unknown:
    default: return "unknown";
    }
}

const char* sc_isa_extra_name(sc_isa_extra e) {
    switch (e) {
    case sc_isa_extra::literal: return "literal";
    case sc_isa_extra::sdwa: return "sdwa";
    case sc_isa_extra::dpp: return "dpp";
    case sc_isa_extra::none:
    default: return "none";
    }
}

const char* sc_isa_stop_name(sc_isa_stop s) {
    switch (s) {
    case sc_isa_stop::s_endpgm: return "s_endpgm";
    case sc_isa_stop::no_terminator: return "no_terminator";
    case sc_isa_stop::trailing_after_endpgm: return "trailing_after_endpgm";
    case sc_isa_stop::unsupported_format: return "unsupported_format";
    case sc_isa_stop::mimg_unsupported: return "mimg_unsupported";
    case sc_isa_stop::truncated: return "truncated";
    case sc_isa_stop::not_word_aligned: return "not_word_aligned";
    case sc_isa_stop::empty: return "empty";
    case sc_isa_stop::none:
    default: return "none";
    }
}

sc_isa_instruction sc_isa_decode_word(uint32_t word) {
    sc_isa_instruction inst;
    inst.dwords[0] = word;
    const uint32_t top2 = word >> 30;

    if (top2 == 0x2u) {
        // bit31 = 1, bit30 = 0: the scalar encodings.
        const uint32_t enc9 = word >> 23;
        if (enc9 == kSop1Encoding) {
            inst.format = sc_isa_format::sop1;
            inst.opcode = static_cast<uint16_t>((word >> 8) & 0xffu);
            inst.opcode_known = true;
            sop_extra(word, inst);
        } else if (enc9 == kSopcEncoding) {
            inst.format = sc_isa_format::sopc;
            inst.opcode = static_cast<uint16_t>((word >> 16) & 0x7fu);
            inst.opcode_known = true;
            sop_extra(word, inst);
        } else if (enc9 == kSoppEncoding) {
            inst.format = sc_isa_format::sopp;
            inst.opcode = static_cast<uint16_t>((word >> 16) & 0x7fu);
            inst.opcode_known = true;
            inst.words = 1;
            inst.is_endpgm = inst.opcode == kEndpgmOpcode;
        } else if ((word >> 28) == kSopkEncoding) {
            // SOPK shares the scalar prefix: its 4-bit encoding is 1011, which
            // falls inside SOP2's 7-bit opcode space. Real SOP2 opcodes stay
            // below 0x60 (so their bits 29:28 are 0), which is why the 4-bit
            // pattern decides after the 9-bit SOP1/SOPC/SOPP patterns.
            inst.format = sc_isa_format::sopk;
            inst.opcode = static_cast<uint16_t>((word >> 23) & 0x1fu);
            inst.opcode_known = true;
            inst.words = 1;
        } else {
            inst.format = sc_isa_format::sop2;
            inst.opcode = static_cast<uint16_t>((word >> 23) & 0x7fu);
            inst.opcode_known = true;
            sop_extra(word, inst);
        }
        return inst;
    }

    if (top2 == 0x3u) {
        // bit31 = 1, bit30 = 1: every 64-bit memory/vector encoding. (SOPK has
        // bit30 = 0, so it is handled in the scalar branch above.)
        switch (word >> 26) {
        case 0x32u: inst.format = sc_isa_format::vintrp; inst.words = 1; break;
        case 0x33u: inst.format = sc_isa_format::vop3p; inst.words = 2; break;
        case 0x34u:
        case 0x35u: inst.format = sc_isa_format::vop3; inst.words = 2; break;
        case 0x36u: inst.format = sc_isa_format::ds; inst.words = 2; break;
        case 0x37u: inst.format = sc_isa_format::flat; inst.words = 2; break;
        case 0x38u: inst.format = sc_isa_format::mubuf; inst.words = 2; break;
        case 0x3Au: inst.format = sc_isa_format::mtbuf; inst.words = 2; break;
        case 0x3Cu:
            // 64-bit base plus one dword per extra address component (NSA). The
            // count lives in the image opcode's dimension, which is not decoded.
            inst.format = sc_isa_format::mimg;
            inst.words = 0;
            break;
        case 0x3Du: inst.format = sc_isa_format::smem; inst.words = 2; break;
        case 0x3Eu: inst.format = sc_isa_format::exp; inst.words = 2; break;
        default:
            inst.format = sc_isa_format::unknown;
            inst.words = 0;
            break;
        }
        return inst;
    }

    // bit31 = 0: the 32-bit vector encodings.
    const uint32_t enc7 = word >> 25;
    if (enc7 == 0x3fu) {
        inst.format = sc_isa_format::vop1;
        inst.opcode = static_cast<uint16_t>((word >> 9) & 0xffu);
        inst.opcode_known = true;
        vop_extra(word, inst);
    } else if (enc7 == 0x3eu) {
        inst.format = sc_isa_format::vopc;
        inst.opcode = static_cast<uint16_t>((word >> 17) & 0xffu);
        inst.opcode_known = true;
        vop_extra(word, inst);
    } else {
        inst.format = sc_isa_format::vop2;
        inst.opcode = static_cast<uint16_t>((word >> 25) & 0x3fu);
        inst.opcode_known = true;
        vop_extra(word, inst);
    }
    return inst;
}

std::string sc_isa_walk::format_histogram_json() const {
    uint32_t counts[19] = {};
    for (const auto& inst : instructions) {
        ++counts[static_cast<uint32_t>(inst.format)];
    }
    std::string s = "{";
    bool first = true;
    for (uint32_t i = 1; i < 19; ++i) {
        if (counts[i] == 0) {
            continue;
        }
        if (!first) {
            s.push_back(',');
        }
        first = false;
        json_escape(s, sc_isa_format_name(static_cast<sc_isa_format>(i)));
        s += ":" + std::to_string(counts[i]);
    }
    s.push_back('}');
    return s;
}

std::string sc_isa_walk::to_json() const {
    std::string s = "{";
    s += "\"synced\":";
    s += synced ? "true" : "false";
    s += ",\"stop\":";
    json_escape(s, sc_isa_stop_name(stop));
    s += ",\"stop_offset\":" + std::to_string(stop_offset);
    s += ",\"bytes_covered\":" + std::to_string(bytes_covered);
    s += ",\"instructions\":" + std::to_string(instructions.size());
    s += ",\"literal_words\":" + std::to_string(literal_words);
    s += ",\"formats\":" + format_histogram_json();
    s += ",\"first\":[";
    const size_t shown = instructions.size() < 8 ? instructions.size() : 8;
    for (size_t i = 0; i < shown; ++i) {
        if (i != 0) {
            s.push_back(',');
        }
        const sc_isa_instruction& inst = instructions[i];
        s += "{\"offset\":" + std::to_string(inst.offset);
        s += ",\"format\":";
        json_escape(s, sc_isa_format_name(inst.format));
        s += ",\"words\":" + std::to_string(inst.words);
        s += ",\"extra\":";
        json_escape(s, sc_isa_extra_name(inst.extra));
        s += ",\"opcode\":" + std::to_string(inst.opcode);
        s.push_back('}');
    }
    s += "]}";
    return s;
}

sc_isa_walk sc_isa_walk_stream(const void* code, size_t size) {
    sc_isa_walk walk;
    walk.open_questions.push_back(
        "no real XS2 payload has been decoded yet; the encoding table is documented and exercised against "
        "assembler-produced words in the harness, not against console bytes");
    walk.open_questions.push_back(
        "opcodes are extracted only for SOP1/SOP2/SOPC/SOPP/SOPK/VOP1/VOP2/VOPC; VOP3/VOP3P/VINTRP and "
        "memory/image opcode field positions are not verified");
    walk.open_questions.push_back(
        "SOPK is walked as one dword; the documented 64-bit S_SETREG_IMM32_B32 form would desynchronise here");
    walk.open_questions.push_back("MIMG length depends on the NSA address count, which needs the image opcode dimension");

    if (code == nullptr || size == 0) {
        walk.stop = sc_isa_stop::empty;
        walk.evidence.push_back("observed: zero bytes");
        return walk;
    }
    if (size > 0xffffffffull) {
        walk.stop = sc_isa_stop::truncated;
        walk.evidence.push_back("observed: stream larger than 4 GiB; this stepper is 32-bit");
        return walk;
    }
    if (size % 4 != 0) {
        walk.stop = sc_isa_stop::not_word_aligned;
        walk.stop_offset = static_cast<uint32_t>(size - size % 4);
        walk.evidence.push_back("observed: stream size " + std::to_string(size) +
                                " is not a multiple of 4; microcode is word-aligned");
        return walk;
    }

    const uint8_t* p = static_cast<const uint8_t*>(code);
    uint32_t offset = 0;
    walk.stop = sc_isa_stop::none;
    while (offset < size) {
        const uint32_t word = rd32(p + offset);
        sc_isa_instruction inst = sc_isa_decode_word(word);
        inst.offset = offset;

        if (inst.format == sc_isa_format::unknown) {
            walk.stop = sc_isa_stop::unsupported_format;
            walk.stop_offset = offset;
            walk.stop_dword = word;
            walk.evidence.push_back("observed: word " + hex8(word) + " at byte " + std::to_string(offset) +
                                    " has no GFX10 encoding field; stopped instead of guessing a length");
            break;
        }
        if (inst.format == sc_isa_format::mimg) {
            walk.stop = sc_isa_stop::mimg_unsupported;
            walk.stop_offset = offset;
            walk.stop_dword = word;
            walk.evidence.push_back("inferred: word " + hex8(word) + " at byte " + std::to_string(offset) +
                                    " is MIMG; its NSA address words make the length opcode-dependent");
            break;
        }
        if (static_cast<uint64_t>(offset) + static_cast<uint64_t>(inst.words) * 4ull > size) {
            walk.stop = sc_isa_stop::truncated;
            walk.stop_offset = offset;
            walk.stop_dword = word;
            walk.evidence.push_back("observed: instruction at byte " + std::to_string(offset) + " needs " +
                                    std::to_string(inst.words) +
                                    " words but the region ends first; the region bound or the encoding is wrong");
            break;
        }
        if (inst.words == 2) {
            inst.dwords[1] = rd32(p + offset + 4);
        }
        if (inst.extra == sc_isa_extra::literal) {
            ++walk.literal_words;
        }

        const bool endpgm = inst.is_endpgm;
        walk.instructions.push_back(inst);
        offset += inst.words * 4u;
        if (endpgm) {
            walk.stop = sc_isa_stop::s_endpgm;
            walk.stop_offset = offset;
            break;
        }
    }

    if (walk.stop == sc_isa_stop::none) {
        walk.stop = sc_isa_stop::no_terminator;
        walk.stop_offset = static_cast<uint32_t>(size);
    } else if (walk.stop == sc_isa_stop::s_endpgm && offset < size) {
        walk.stop = sc_isa_stop::trailing_after_endpgm;
        walk.stop_offset = offset;
    }

    walk.bytes_covered = offset;
    walk.synced = walk.stop == sc_isa_stop::s_endpgm && offset == size;

    walk.evidence.insert(walk.evidence.begin(),
                         "observed: " + std::to_string(walk.instructions.size()) + " instruction(s), " +
                             std::to_string(walk.literal_words) + " literal dword(s), " +
                             std::to_string(walk.bytes_covered) + " of " + std::to_string(size) + " bytes covered");
    if (walk.synced) {
        walk.evidence.push_back("observed: stream ends exactly on s_endpgm at byte " +
                                std::to_string(walk.stop_offset - 4) + " -- every word stepped cleanly");
    } else if (walk.stop == sc_isa_stop::trailing_after_endpgm) {
        walk.evidence.push_back("observed: s_endpgm at byte " + std::to_string(walk.stop_offset - 4) + " is followed by " +
                                std::to_string(size - walk.stop_offset) + " more byte(s) in the region");
    }
    return walk;
}

sc_isa_walk sc_isa_walk_region(const void* blob, size_t blob_size, uint32_t begin, uint32_t end) {
    sc_isa_walk walk;
    if (blob == nullptr || begin >= blob_size || end > blob_size || end < begin) {
        walk.stop = sc_isa_stop::truncated;
        walk.evidence.push_back("inferred: code region [" + std::to_string(begin) + ", " + std::to_string(end) +
                                ") is not inside a " + std::to_string(blob_size) +
                                "-byte blob; Phase 3 region bounds are wrong for these bytes");
        return walk;
    }
    const uint8_t* p = static_cast<const uint8_t*>(blob);
    walk = sc_isa_walk_stream(p + begin, end - begin);
    for (auto& inst : walk.instructions) {
        inst.offset += begin;
    }
    walk.stop_offset += begin;
    walk.evidence.insert(walk.evidence.begin(), "observed: code region [" + std::to_string(begin) + ", " +
                                                    std::to_string(end) + ") from the Phase 3 frontend");
    return walk;
}

} // namespace sc
