// ScarlettCompat shader pipeline - Phase 5 harness (operands + wave semantics).
// MIT. Standalone: no game, no guest, no device, no layer, no emitter.
//
//   sc_wave_test.exe [output-dir]
//
// The operand fixtures are real assembler/disassembler words: the GFX10 9-bit
// source field puts SGPRs at 0..255 and VGPRs at 256+n, so `v_mov_b32 v0, s105`
// is 0x7E000269 (src0 = 105) while `v_cvt_f32_i32 v1, v2` is 0x7E020B02
// (src0 = 258). Getting that backwards is the obvious way to mislabel every
// operand in a real shader, so it is pinned here.
//
// Wave size is asserted to be UNKNOWN. That is the honest answer from bytes, and
// the harness fails if the module ever starts claiming one without a source.
#include "shader/sc_frontend.h"
#include "shader/sc_isa.h"
#include "shader/sc_wave.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace sc;

namespace {

int g_checks = 0;
int g_fail = 0;

void check(bool condition, const char* expr, const char* file, int line) {
    ++g_checks;
    if (condition) {
        return;
    }
    ++g_fail;
    std::printf("FAIL %s:%d  %s\n", file, line, expr);
}

#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)

void banner(const char* name) { std::printf("\n== %s ==\n", name); }

void wr32(std::vector<uint8_t>& v, size_t offset, uint32_t value) {
    v[offset + 0] = static_cast<uint8_t>(value);
    v[offset + 1] = static_cast<uint8_t>(value >> 8);
    v[offset + 2] = static_cast<uint8_t>(value >> 16);
    v[offset + 3] = static_cast<uint8_t>(value >> 24);
}

std::vector<uint8_t> u32s(std::initializer_list<uint32_t> values) {
    std::vector<uint8_t> v(values.size() * 4, 0);
    size_t i = 0;
    for (uint32_t value : values) {
        wr32(v, i * 4, value);
        ++i;
    }
    return v;
}

// ------------------------------------------------------------ operand fields
void test_operand_fields() {
    banner("9-bit source field: SGPR 0..105, VGPR 256+n, 0xFF literal");

    struct field_case {
        const char* label;
        uint32_t field;
        sc_operand_file file;
        uint32_t index;
        const char* name;
    };
    const field_case cases[] = {
        {"s0", 0, sc_operand_file::sgpr, 0, "s0"},
        {"s105 (max SGPR)", 105, sc_operand_file::sgpr, 105, "s105"},
        {"v0", 256, sc_operand_file::vgpr, 0, "v0"},
        {"v2", 258, sc_operand_file::vgpr, 2, "v2"},
        {"v255", 511, sc_operand_file::vgpr, 255, "v255"},
        {"literal", 0xFF, sc_operand_file::literal, 0, "literal"},
        {"0xC8 (beyond SGPRs, not transcribed)", 0xC8, sc_operand_file::unclassified, 0, "field:0xc8"},
    };
    for (const auto& c : cases) {
        const sc_operand op = sc_operand_from_src9(c.field);
        CHECK(op.file == c.file);
        CHECK(op.field == c.field);
        if (c.file == sc_operand_file::sgpr || c.file == sc_operand_file::vgpr) {
            CHECK(op.index == c.index);
        }
        CHECK(sc_operand_name(op) == c.name);
        std::printf("   field %-3u -> %-13s %s\n", c.field, sc_operand_file_name(op.file),
                    sc_operand_name(op).c_str());
    }

    // 8-bit vector fields are VGPRs only.
    for (uint32_t f : {0u, 2u, 255u}) {
        const sc_operand op = sc_operand_from_vsrc8(f);
        CHECK(op.file == sc_operand_file::vgpr);
        CHECK(op.index == f);
    }
    CHECK(sc_operand_name(sc_operand_from_vsrc8(255)) == "v255");
    std::printf("   8-bit fields 0/2/255 -> v0/v2/v255 (VGPR only)\n");
}

// ------------------------------------------------------- instructions (real)
void test_instruction_operands() {
    banner("instruction operands from real assembler words");

    struct inst_case {
        const char* label;
        uint32_t word;
        sc_isa_format format;
        const char* vdst;  // "-" when absent
        const char* src0;  // "-" when the form has no register there
        const char* vsrc1; // "-" when absent
    };
    const inst_case cases[] = {
        // v_mov_b32_e32 v0, s105   (gfx10-sgpr-max disassembler test)
        {"v_mov_b32 v0, s105", 0x7E000269u, sc_isa_format::vop1, "v0", "s105", "-"},
        // v_mov_b32_e32 v1, v2     (vop1.s)
        {"v_mov_b32 v1, v2", 0x7E020302u, sc_isa_format::vop1, "v1", "v2", "-"},
        // v_cvt_f32_i32_e32 v1, v2 (vop1.s): src0 = 258 -> v2
        {"v_cvt_f32_i32 v1, v2", 0x7E020B02u, sc_isa_format::vop1, "v1", "v2", "-"},
        // v_mov_b32_sdwa: the low byte is a modifier selector, not a register.
        {"v_mov_b32_sdwa v0, s105", 0x7E0002F9u, sc_isa_format::vop1, "v0", "-", "-"},
    };
    for (const auto& c : cases) {
        const std::vector<uint8_t> bytes =
            c.word == 0x7E0002F9u ? u32s({c.word, 0x00861669u}) : u32s({c.word, 0xBF810000u});
        const sc_isa_walk w = sc_isa_walk_stream(bytes.data(), bytes.size());
        CHECK(!w.instructions.empty());
        const sc_wave_decode d = sc_decode_wave(w);
        CHECK(!d.instructions.empty());
        const sc_instruction_operands& io = d.instructions[0];
        CHECK(io.format == c.format);
        CHECK(std::string(io.has_vdst ? sc_operand_name(io.vdst) : "-") == c.vdst);
        CHECK(std::string(io.has_src0 ? sc_operand_name(io.src0) : "-") == c.src0);
        CHECK(std::string(io.has_vsrc1 ? sc_operand_name(io.vsrc1) : "-") == c.vsrc1);
        std::printf("   %-26s -> vdst=%-6s src0=%-6s vsrc1=%-6s\n", c.label,
                    io.has_vdst ? sc_operand_name(io.vdst).c_str() : "-",
                    io.has_src0 ? sc_operand_name(io.src0).c_str() : "-",
                    io.has_vsrc1 ? sc_operand_name(io.vsrc1).c_str() : "-");
    }

    // A literal operand: src0 = 0xFF selects the following dword, and Phase 4
    // already reports words == 2 for it.
    {
        const std::vector<uint8_t> bytes = u32s({0x7E0002FFu, 0x3F800000u, 0xBF810000u});
        const sc_isa_walk w = sc_isa_walk_stream(bytes.data(), bytes.size());
        CHECK(w.synced);
        CHECK(w.instructions[0].words == 2);
        const sc_wave_decode d = sc_decode_wave(w);
        CHECK(d.instructions[0].has_src0);
        CHECK(d.instructions[0].src0.file == sc_operand_file::literal);
        CHECK(d.census.literal_refs == 1);
        std::printf("   vop1 with src0=0xFF -> %s, words=2, literal_refs=%u\n",
                    sc_operand_name(d.instructions[0].src0).c_str(), d.census.literal_refs);
    }
}

// --------------------------------------------------------- paired widths
// Width is opcode-selected, so only the forms the ISA documents (and, for
// s_mov_b64, the assembler pins) get one. Everything else must stay unknown:
// a decoder that assumes 32-bit would read v[222:223] as one register.
void test_paired_widths() {
    banner("paired-register widths: documented/verified forms only, rest unknown");

    // Scalar field classification first: exec (126) and null (125) are not SGPRs
    // this module names, so they stay unclassified rather than becoming "s126".
    CHECK(sc_operand_from_ssrc8(102).file == sc_operand_file::sgpr);
    CHECK(sc_operand_from_ssrc8(102).index == 102);
    CHECK(sc_operand_from_ssrc8(125).file == sc_operand_file::unclassified);
    CHECK(sc_operand_from_ssrc8(126).file == sc_operand_file::unclassified);
    CHECK(sc_operand_from_ssrc8(0xFF).file == sc_operand_file::literal);

    struct width_case {
        const char* label;
        uint32_t word;
        sc_isa_format format;
        uint16_t opcode;
        sc_operand_width dst;
        sc_operand_width src0;
        sc_width_provenance prov; // checked where a width is known
    };
    const width_case cases[] = {
        // s_mov_b64 s[0:1], s[102:103] -> [0x66,0x04,0x80,0xbe]  (gfx10 bytes)
        {"s_mov_b64 s[0:1], s[102:103]", 0xBE800466u, sc_isa_format::sop1, 4, sc_operand_width::b64,
         sc_operand_width::b64, sc_width_provenance::assembler_verified},
        // v_cvt_i32_f64_e32 v1, v[2:3] -> [0x02,0x07,0x02,0x7e]
        {"v_cvt_i32_f64 v1, v[2:3]", 0x7E020702u, sc_isa_format::vop1, 3, sc_operand_width::b32,
         sc_operand_width::b64, sc_width_provenance::isa_documented},
        // v_cvt_f64_i32_e32 v[1:2], v2 -> [0x02,0x09,0x02,0x7e]
        {"v_cvt_f64_i32 v[1:2], v2", 0x7E020902u, sc_isa_format::vop1, 4, sc_operand_width::b64,
         sc_operand_width::b32, sc_width_provenance::isa_documented},
        // v_cvt_f32_f64_e32 v1, v[2:3] -> [0x02,0x1f,0x02,0x7e]
        {"v_cvt_f32_f64 v1, v[2:3]", 0x7E021F02u, sc_isa_format::vop1, 15, sc_operand_width::b32,
         sc_operand_width::b64, sc_width_provenance::isa_documented},
        // v_cvt_f64_f32_e32 v[1:2], v2 -> [0x02,0x21,0x02,0x7e]
        {"v_cvt_f64_f32 v[1:2], v2", 0x7E022102u, sc_isa_format::vop1, 16, sc_operand_width::b64,
         sc_operand_width::b32, sc_width_provenance::isa_documented},
        // v_mov_b32 is not in the table: width stays unknown on purpose.
        {"v_mov_b32 v0, s105 (not tabulated)", 0x7E000269u, sc_isa_format::vop1, 1,
         sc_operand_width::unknown, sc_operand_width::unknown, sc_width_provenance::none},
    };

    for (const auto& c : cases) {
        const std::vector<uint8_t> bytes = u32s({c.word, 0xBF810000u});
        const sc_isa_walk w = sc_isa_walk_stream(bytes.data(), bytes.size());
        CHECK(!w.instructions.empty());
        CHECK(w.instructions[0].opcode == c.opcode);
        const sc_wave_decode d = sc_decode_wave(w);
        CHECK(!d.instructions.empty());
        const sc_instruction_operands& io = d.instructions[0];
        CHECK(io.format == c.format);
        CHECK(io.vdst.width == c.dst);
        CHECK(io.src0.width == c.src0);
        if (c.prov != sc_width_provenance::none) {
            if (c.dst != sc_operand_width::unknown) {
                CHECK(io.vdst.provenance == c.prov);
            }
            if (c.src0 != sc_operand_width::unknown) {
                CHECK(io.src0.provenance == c.prov);
            }
        }
        std::printf("   %-36s dst=%-6s/%-8s src0=%-6s/%-8s\n", c.label, sc_operand_name(io.vdst).c_str(),
                    sc_operand_width_name(io.vdst.width), sc_operand_name(io.src0).c_str(),
                    sc_operand_width_name(io.src0.width));
    }

    // exec as a destination: the field is understood, the register is not named,
    // so no width is claimed for it while the source pair still is.
    {
        const std::vector<uint8_t> bytes = u32s({0xBEFE0402u, 0xBF810000u}); // s_mov_b64 exec, s[2:3]
        const sc_wave_decode d = sc_decode_wave(sc_isa_walk_stream(bytes.data(), bytes.size()));
        CHECK(d.instructions[0].vdst.file == sc_operand_file::unclassified);
        CHECK(d.instructions[0].vdst.width == sc_operand_width::unknown);
        CHECK(d.instructions[0].src0.file == sc_operand_file::sgpr);
        CHECK(d.instructions[0].src0.index == 2);
        CHECK(d.instructions[0].src0.width == sc_operand_width::b64);
        std::printf("   s_mov_b64 exec, s[2:3]            dst=unclassified/unknown src0=s2/b64\n");
    }

    // An inline constant on the 64-bit side gets no register width either.
    {
        // v_cvt_f64_i32_e32 v[222:223], 1.0 -> [0xf2,0x08,0xbc,0x7f]
        const std::vector<uint8_t> bytes = u32s({0x7FBC08F2u, 0xBF810000u});
        const sc_wave_decode d = sc_decode_wave(sc_isa_walk_stream(bytes.data(), bytes.size()));
        CHECK(d.instructions[0].vdst.width == sc_operand_width::b64);
        CHECK(d.instructions[0].vdst.index == 222);
        CHECK(d.instructions[0].src0.file == sc_operand_file::unclassified);
        CHECK(d.instructions[0].src0.width == sc_operand_width::unknown);
        std::printf("   v_cvt_f64_i32 v[222:223], 1.0     dst=v222/b64 src0=field:0xf2/unknown\n");
    }
}

// ------------------------------------------------------------------- census
void test_census() {
    banner("register census over a walked stream");
    const std::vector<uint8_t> bytes = u32s({
        0x7E000269u, // v_mov_b32 v0, s105          -> vdst v0, src0 s105
        0x7E020302u, // v_mov_b32 v1, v2            -> vdst v1, src0 v2
        0x7E0002F9u, // v_mov_b32_sdwa v0, s105      -> vdst v0 only (modifier form)
        0x00861669u, // sdwa modifier dword
        0xBF810000u, // s_endpgm
    });
    const sc_isa_walk w = sc_isa_walk_stream(bytes.data(), bytes.size());
    CHECK(w.synced);
    const sc_wave_decode d = sc_decode_wave(w);
    CHECK(d.census.instructions == 3);
    CHECK(d.census.sgpr_refs == 1);
    // v0 (vdst), v1 (vdst), v2 (src0), v0 (sdwa vdst) -- the sdwa src0 is a modifier, not a register
    CHECK(d.census.vgpr_refs == 4);
    CHECK(d.census.literal_refs == 0);
    CHECK(d.census.unclassified_refs == 0);
    CHECK(d.census.max_sgpr == 105);
    CHECK(d.census.max_vgpr == 2);
    // v_mov_b32 and v_mov_b32_sdwa are not in the width table, so every operand
    // here is width-unknown -- and the census says so instead of assuming 32-bit.
    CHECK(d.census.b32_refs == 0);
    CHECK(d.census.b64_refs == 0);
    CHECK(d.census.width_unknown_refs == 5);
    std::printf("   instructions=%u sgpr=%u vgpr=%u literal=%u unclassified=%u max_sgpr=%u max_vgpr=%u\n",
                d.census.instructions, d.census.sgpr_refs, d.census.vgpr_refs, d.census.literal_refs,
                d.census.unclassified_refs, d.census.max_sgpr, d.census.max_vgpr);
}

// ---------------------------------------------------------------- wave facts
void test_wave_facts() {
    banner("wave semantics: unknown from bytes, and the harness enforces it");
    const std::vector<uint8_t> bytes = u32s({0x7E000269u, 0xBF810000u});
    const sc_isa_walk w = sc_isa_walk_stream(bytes.data(), bytes.size());

    const sc_wave_decode bare = sc_decode_wave(w);
    CHECK(!bare.wave.wave_size_known);
    CHECK(bare.wave.wave_size == 0);
    CHECK(bare.wave.device_wave_lane_min == 0);
    CHECK(!bare.wave.evidence.empty());
    CHECK(!bare.wave.open_questions.empty());
    CHECK(bare.to_json().find("\"wave_size_known\":false") != std::string::npos);

    // Hardware caps are recorded as separate facts and do not turn into a guess.
    const sc_wave_decode caps = sc_decode_wave(w, 64, 64);
    CHECK(caps.wave.device_wave_lane_min == 64);
    CHECK(caps.wave.device_wave_lane_max == 64);
    CHECK(!caps.wave.wave_size_known);
    CHECK(caps.wave.wave_size == 0);
    bool device_line = false;
    for (const auto& line : caps.wave.evidence) {
        if (line.find("observed (device)") != std::string::npos && line.find("64..64") != std::string::npos) {
            device_line = true;
        }
    }
    CHECK(device_line);

    for (const auto& line : caps.wave.evidence) {
        std::printf("   %s\n", line.c_str());
    }
    for (const auto& q : caps.wave.open_questions) {
        std::printf("   ? %s\n", q.c_str());
    }
}

// ------------------------------------------------------- frontend -> wave seam
void test_seam() {
    banner("code region -> walk -> operands");
    // 32 bytes total, the same shape the Phase 3/4 seam uses (the frontend's
    // prober rejects payloads below its minimum size).
    const std::vector<uint8_t> code = u32s({0x7E000269u, 0x7E020302u, 0xBF800000u, 0xBF810000u});
    const uint32_t code_words = static_cast<uint32_t>(code.size() / 4);
    const uint32_t total_words = 4 + code_words;
    std::vector<uint8_t> blob(total_words * 4u, 0);
    wr32(blob, 0, total_words * 4u);
    wr32(blob, 4, 16u);
    wr32(blob, 8, 1u);
    wr32(blob, 12, 3u);
    std::memcpy(blob.data() + 16, code.data(), code.size());

    const sc_shader_classification c = sc_classify_shader(blob.data(), blob.size());
    CHECK(c.kind == sc_payload_kind::rdna2_xs2);
    CHECK(c.xs2.code_regions.size() == 1);
    if (c.xs2.code_regions.empty()) {
        return;
    }
    const sc_xs2_code_region& r = c.xs2.code_regions[0];
    const sc_isa_walk w = sc_isa_walk_region(blob.data(), blob.size(), r.begin, r.end);
    CHECK(w.synced);
    const sc_wave_decode d = sc_decode_wave(w, 64, 64);
    CHECK(d.census.instructions == 2);
    CHECK(d.census.sgpr_refs == 1);
    CHECK(d.census.vgpr_refs == 3); // v0, v1, v2
    CHECK(!d.wave.wave_size_known);
    std::printf("   region [%u,%u) -> %u instruction(s), sgpr=%u vgpr=%u, wave_size_known=%d\n", r.begin, r.end,
                d.census.instructions, d.census.sgpr_refs, d.census.vgpr_refs,
                d.wave.wave_size_known ? 1 : 0);
    std::printf("   json: %s\n", d.to_json().c_str());
}

} // namespace

int main(int argc, char** argv) {
    const std::string out_dir = (argc > 1) ? argv[1] : "out/shader_tests";
    std::printf("ScarlettCompat shader pipeline - Phase 5 (operands + wave semantics)\n");
    std::printf("output dir: %s (this harness writes nothing)\n", out_dir.c_str());

    test_operand_fields();
    test_instruction_operands();
    test_paired_widths();
    test_census();
    test_wave_facts();
    test_seam();

    std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
