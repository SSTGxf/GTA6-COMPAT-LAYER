// ScarlettCompat shader pipeline - Phase 4 harness (RDNA2 instruction decode).
// MIT. Standalone: no game, no guest, no device, no layer, no emitter.
//
//   sc_isa_test.exe [output-dir]
//
// The encoding table is pinned against words produced by a real assembler
// (LLVM MC test vectors, quoted with their source line next to each fixture), so
// the decoder is not only self-consistent. The walk cases then cover every stop
// reason, and the last section goes through the Phase 3 frontend so the
// frontend -> code region -> instruction stream seam is exercised end to end.
#include "shader/sc_frontend.h"
#include "shader/sc_isa.h"

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

struct word_fixture {
    const char* label;
    uint32_t word;
    sc_isa_format format;
    uint32_t words;
    bool opcode_known;
    uint16_t opcode;
    sc_isa_extra extra;
};

// ------------------------------------------------------- assembler word table
// Source: LLVM test/MC/AMDGPU (gfx10 disassembly/assembly tests). Each comment
// keeps the quoted encoding line so the constant can be re-checked.
void test_assembler_words() {
    banner("decoder table pinned to real assembler output (LLVM MC, gfx10)");
    const word_fixture fixtures[] = {
        // s_mov_b32 s1, s2      -> encoding: [0x02,0x03,0x81,0xbe]
        {"s_mov_b32 s1, s2", 0xBE810302u, sc_isa_format::sop1, 1, true, 3, sc_isa_extra::none},
        // s_mov_b32 s1, 1       -> encoding: [0x81,0x03,0x81,0xbe]
        {"s_mov_b32 s1, 1", 0xBE810381u, sc_isa_format::sop1, 1, true, 3, sc_isa_extra::none},
        // s_mov_b32 s1, 100     -> encoding: [0xff,0x03,0x81,0xbe,0x64,0x00,0x00,0x00]
        {"s_mov_b32 s1, 100", 0xBE8103FFu, sc_isa_format::sop1, 2, true, 3, sc_isa_extra::literal},
        // s_cmov_b32 s1, 200    -> encoding: [0xff,0x05,0x81,0xbe,0xc8,0x00,0x00,0x00]
        {"s_cmov_b32 s1, 200", 0xBE8105FFu, sc_isa_format::sop1, 2, true, 5, sc_isa_extra::literal},
        // v_mov_b32_e32 v0, s105 -> encoding: [0x69,0x02,0x00,0x7e]
        {"v_mov_b32 v0, s105", 0x7E000269u, sc_isa_format::vop1, 1, true, 1, sc_isa_extra::none},
        // v_mov_b32_sdwa v0, s105 -> [0xf9,0x02,0x00,0x7e, 0x69,0x16,0x86,0x00]
        {"v_mov_b32_sdwa v0, s105", 0x7E0002F9u, sc_isa_format::vop1, 2, true, 1, sc_isa_extra::sdwa},
        // v_cmp_eq_f32_e64 s105, v0, s105 -> [0x69,0x00,0x02,0xd4, 0x00,0xd3,0x00,0x00]
        {"v_cmp_eq_f32_e64 s105, v0, s105", 0xD4020069u, sc_isa_format::vop3, 2, false, 0, sc_isa_extra::none},
        // v_cmp_eq_f32_sdwa s105, v0, s105 -> [0xf9,0xd2,0x04,0x7c, 0x00,0xe9,0x06,0x86]
        {"v_cmp_eq_f32_sdwa s105, v0, s105", 0x7C04D2F9u, sc_isa_format::vopc, 2, true, 2, sc_isa_extra::sdwa},
        // flat_load_dword v1, v[3:4] -> [0x00,0x00,0x30,0xdc, 0x03,0x00,0x7d,0x01]
        {"flat_load_dword v1, v[3:4]", 0xDC300000u, sc_isa_format::flat, 2, false, 0, sc_isa_extra::none},
        // s_endpgm              -> encoding: [0x00,0x00,0x81,0xbf]
        {"s_endpgm", 0xBF810000u, sc_isa_format::sopp, 1, true, 1, sc_isa_extra::none},
    };

    for (const auto& f : fixtures) {
        const sc_isa_instruction inst = sc_isa_decode_word(f.word);
        const bool match = inst.format == f.format && inst.words == f.words && inst.extra == f.extra &&
                           inst.opcode_known == f.opcode_known && (!f.opcode_known || inst.opcode == f.opcode) &&
                           inst.is_endpgm == (std::strcmp(f.label, "s_endpgm") == 0);
        CHECK(match);
        std::printf("   %-34s 0x%08x -> %-6s words=%u op=%s%u extra=%s%s\n", f.label, f.word,
                    sc_isa_format_name(inst.format), inst.words, inst.opcode_known ? "" : "?",
                    inst.opcode, sc_isa_extra_name(inst.extra), inst.is_endpgm ? " ENDPGM" : "");
        if (!match) {
            std::printf("      expected %s words=%u extra=%s\n", sc_isa_format_name(f.format), f.words,
                        sc_isa_extra_name(f.extra));
        }
    }
}

// ------------------------------------------------------- every format bucket
void test_format_buckets() {
    banner("every GFX10 format bucket is recognised");
    struct bucket {
        const char* label;
        uint32_t word;
        sc_isa_format format;
        uint32_t words;
    };
    const bucket buckets[] = {
        {"sop2", 0x80000000u, sc_isa_format::sop2, 1},   {"sop1", 0xBE810302u, sc_isa_format::sop1, 1},
        {"sopc", 0xBF000000u, sc_isa_format::sopc, 1},   {"sopp", 0xBF820000u, sc_isa_format::sopp, 1},
        {"sopk", 0xB0000000u, sc_isa_format::sopk, 1},   {"vop2", 0x04000000u, sc_isa_format::vop2, 1},
        {"vop1", 0x7E000269u, sc_isa_format::vop1, 1},   {"vopc", 0x7C000000u, sc_isa_format::vopc, 1},
        {"vop3a", 0xD4000000u, sc_isa_format::vop3, 2},  {"vop3b", 0xD5000000u, sc_isa_format::vop3, 2},
        {"vop3p", 0xCC000000u, sc_isa_format::vop3p, 2}, {"vintrp", 0xC8000000u, sc_isa_format::vintrp, 1},
        {"ds", 0xD8000000u, sc_isa_format::ds, 2},       {"flat", 0xDC000000u, sc_isa_format::flat, 2},
        {"mubuf", 0xE0000000u, sc_isa_format::mubuf, 2}, {"mtbuf", 0xE8000000u, sc_isa_format::mtbuf, 2},
        {"smem", 0xF4000000u, sc_isa_format::smem, 2},   {"exp", 0xF8000000u, sc_isa_format::exp, 2},
        {"mimg", 0xF0000000u, sc_isa_format::mimg, 0},   {"unknown", 0xFFFFFFFFu, sc_isa_format::unknown, 0},
    };
    for (const auto& b : buckets) {
        const sc_isa_instruction inst = sc_isa_decode_word(b.word);
        CHECK(inst.format == b.format);
        CHECK(inst.words == b.words);
        std::printf("   %-8s 0x%08x -> %-8s words=%u\n", b.label, b.word, sc_isa_format_name(inst.format),
                    inst.words);
    }
    // s_branch (SOPP opcode 2) must not be mistaken for a terminator.
    const sc_isa_instruction branch = sc_isa_decode_word(0xBF820000u);
    CHECK(!branch.is_endpgm);
}

// --------------------------------------------------------------- stream walks
void test_walk_cases() {
    banner("stream walk: every stop reason");

    const struct {
        const char* label;
        std::vector<uint8_t> bytes;
    } cases[] = {
        {"literal + vop1 + endpgm", u32s({0xBE8103FFu, 0x00000064u, 0x7E000269u, 0xBF810000u})},
        {"sdwa extra dword", u32s({0x7E0002F9u, 0x00861669u, 0xBF810000u})},
        {"flat 64-bit", u32s({0xDC300000u, 0x017D0003u, 0xBF810000u})},
        {"two vop2 then endpgm", u32s({0x00000000u, 0x00000000u, 0xBF810000u})},
        {"dpp extra dword", u32s({0x7E0002FAu, 0x00000000u, 0xBF810000u})},
    };
    CHECK(cases[0].bytes.size() == 16);

    for (const auto& c : cases) {
        const sc_isa_walk w = sc_isa_walk_stream(c.bytes.data(), c.bytes.size());
        CHECK(w.synced);
        CHECK(w.stop == sc_isa_stop::s_endpgm);
        CHECK(w.stop_offset == c.bytes.size());
        CHECK(w.bytes_covered == c.bytes.size());
        CHECK(!w.instructions.empty());
        CHECK(w.instructions.back().is_endpgm);
        std::printf("   %-24s synced=%d instructions=%zu literal_words=%u formats=%s\n", c.label, w.synced ? 1 : 0,
                    w.instructions.size(), w.literal_words, w.format_histogram_json().c_str());
    }

    // The literal stream must consume its constant as a dword, not as an opcode.
    const sc_isa_walk lit = sc_isa_walk_stream(cases[0].bytes.data(), cases[0].bytes.size());
    CHECK(lit.instructions.size() == 3); // sop1+literal, vop1, sopp -- the literal is not an instruction
    CHECK(lit.literal_words == 1);
    CHECK(lit.instructions[0].words == 2);
    CHECK(lit.instructions[0].dwords[1] == 0x00000064u);
    CHECK(lit.instructions[0].offset == 0);
    CHECK(lit.instructions[1].offset == 8);

    // No terminator.
    {
        const std::vector<uint8_t> no_end = u32s({0x7E000269u});
        const sc_isa_walk w = sc_isa_walk_stream(no_end.data(), no_end.size());
        CHECK(!w.synced);
        CHECK(w.stop == sc_isa_stop::no_terminator);
        CHECK(w.stop_offset == 4);
        std::printf("   no terminator -> stop=%s\n", sc_isa_stop_name(w.stop));
    }
    // Trailing bytes after the terminator.
    {
        const std::vector<uint8_t> trailing = u32s({0xBF810000u, 0x7E000269u});
        const sc_isa_walk w = sc_isa_walk_stream(trailing.data(), trailing.size());
        CHECK(!w.synced);
        CHECK(w.stop == sc_isa_stop::trailing_after_endpgm);
        CHECK(w.stop_offset == 4);
        std::printf("   trailing after endpgm -> stop=%s at %u\n", sc_isa_stop_name(w.stop), w.stop_offset);
    }
    // MIMG stops with an explicit reason instead of guessing NSA length.
    {
        const std::vector<uint8_t> img = u32s({0x7E000269u, 0xF0000000u, 0xBF810000u});
        const sc_isa_walk w = sc_isa_walk_stream(img.data(), img.size());
        CHECK(!w.synced);
        CHECK(w.stop == sc_isa_stop::mimg_unsupported);
        CHECK(w.stop_offset == 4);
        CHECK(w.instructions.size() == 1);
        std::printf("   mimg -> stop=%s at %u (instructions=%zu)\n", sc_isa_stop_name(w.stop), w.stop_offset,
                    w.instructions.size());
    }
    // Unknown encoding.
    {
        const std::vector<uint8_t> junk = u32s({0xFFFFFFFFu});
        const sc_isa_walk w = sc_isa_walk_stream(junk.data(), junk.size());
        CHECK(!w.synced);
        CHECK(w.stop == sc_isa_stop::unsupported_format);
        CHECK(w.stop_dword == 0xFFFFFFFFu);
        std::printf("   unknown word -> stop=%s\n", sc_isa_stop_name(w.stop));
    }
    // Truncated multi-word instruction.
    {
        const std::vector<uint8_t> cut = u32s({0xDC300000u});
        const sc_isa_walk w = sc_isa_walk_stream(cut.data(), cut.size());
        CHECK(w.stop == sc_isa_stop::truncated);
        CHECK(w.instructions.empty());
        std::printf("   truncated 64-bit -> stop=%s\n", sc_isa_stop_name(w.stop));
    }
    // A literal instruction whose literal dword is missing.
    {
        const std::vector<uint8_t> cut = u32s({0xBE8103FFu});
        const sc_isa_walk w = sc_isa_walk_stream(cut.data(), cut.size());
        CHECK(w.stop == sc_isa_stop::truncated);
        std::printf("   literal without its dword -> stop=%s\n", sc_isa_stop_name(w.stop));
    }
    // Unaligned and empty.
    {
        const uint8_t six[6] = {0, 0, 0, 0, 0, 0};
        const sc_isa_walk w = sc_isa_walk_stream(six, sizeof(six));
        CHECK(w.stop == sc_isa_stop::not_word_aligned);
        const sc_isa_walk e = sc_isa_walk_stream(nullptr, 0);
        CHECK(e.stop == sc_isa_stop::empty);
        std::printf("   unaligned -> stop=%s ; empty -> stop=%s\n", sc_isa_stop_name(w.stop),
                    sc_isa_stop_name(e.stop));
    }
}

// ------------------------------------------------------------ region + seam
std::vector<uint8_t> make_xs2_payload(const std::vector<uint8_t>& code) {
    const uint32_t code_words = static_cast<uint32_t>(code.size() / 4);
    const uint32_t total_words = 4 + code_words;
    std::vector<uint8_t> blob(total_words * 4u, 0);
    wr32(blob, 0, total_words * 4u); // payload size, corroborates the prober
    wr32(blob, 4, 16u);              // code offset
    wr32(blob, 8, 1u);
    wr32(blob, 12, 3u);
    std::memcpy(blob.data() + 16, code.data(), code.size());
    return blob;
}

void test_region_and_seam() {
    banner("code region offsets + frontend seam");

    const std::vector<uint8_t> code = u32s({0xBE8103FFu, 0x00000064u, 0x7E000269u, 0xBF810000u});
    std::vector<uint8_t> blob(8, 0xab);
    blob.insert(blob.end(), code.begin(), code.end());
    const sc_isa_walk region = sc_isa_walk_region(blob.data(), blob.size(), 8, 8 + static_cast<uint32_t>(code.size()));
    CHECK(region.synced);
    CHECK(region.instructions.front().offset == 8);
    CHECK(region.instructions.back().offset == 8 + code.size() - 4);
    CHECK(region.stop_offset == 8 + code.size());
    std::printf("   region [8,%zu) -> synced=%d first=%u last=%u stop_offset=%u\n", 8 + code.size(),
                region.synced ? 1 : 0, region.instructions.front().offset, region.instructions.back().offset,
                region.stop_offset);

    // Out-of-range region bounds are refused, not read.
    const sc_isa_walk bad = sc_isa_walk_region(blob.data(), blob.size(), 4, static_cast<uint32_t>(blob.size() + 8));
    CHECK(!bad.synced);
    CHECK(bad.stop == sc_isa_stop::truncated);

    // Frontend -> code region -> instruction stream.
    const std::vector<uint8_t> payload = make_xs2_payload(code);
    const sc_shader_classification c = sc_classify_shader(payload.data(), payload.size());
    std::printf("   classify -> kind=%s regions=%zu found=%d\n", c.kind_name(), c.xs2.code_regions.size(),
                c.xs2.found ? 1 : 0);
    CHECK(c.kind == sc_payload_kind::rdna2_xs2);
    CHECK(c.xs2.code_regions.size() == 1);
    if (!c.xs2.code_regions.empty()) {
        const sc_xs2_code_region& r = c.xs2.code_regions[0];
        const sc_isa_walk w = sc_isa_walk_region(payload.data(), payload.size(), r.begin, r.end);
        CHECK(w.synced);
        CHECK(w.instructions.back().offset == r.terminator_offset);
        CHECK(w.stop_offset == r.end);
        std::printf("   walk [%u,%u) -> synced=%d instructions=%zu formats=%s\n", r.begin, r.end, w.synced ? 1 : 0,
                    w.instructions.size(), w.format_histogram_json().c_str());
        for (const auto& line : w.evidence) {
            std::printf("      %s\n", line.c_str());
        }
    }
}

void test_json() {
    banner("JSON + determinism");
    const std::vector<uint8_t> code = u32s({0xBE8103FFu, 0x00000064u, 0x7E000269u, 0xBF810000u});
    const sc_isa_walk a = sc_isa_walk_stream(code.data(), code.size());
    const sc_isa_walk b = sc_isa_walk_stream(code.data(), code.size());
    CHECK(a.to_json() == b.to_json());
    const std::string json = a.to_json();
    CHECK(json.find("\"synced\":true") != std::string::npos);
    CHECK(json.find("\"stop\":\"s_endpgm\"") != std::string::npos);
    CHECK(json.find("\"sop1\":1") != std::string::npos);
    CHECK(json.find("\"vop1\":1") != std::string::npos);
    CHECK(json.find("\"sopp\":1") != std::string::npos);
    CHECK(json.find("\"formats\":") != std::string::npos);
    CHECK(!a.evidence.empty());
    CHECK(!a.open_questions.empty());
    std::printf("   %zu bytes of JSON, %zu evidence line(s), %zu open question(s)\n", json.size(), a.evidence.size(),
                a.open_questions.size());
}

} // namespace

int main(int argc, char** argv) {
    const std::string out_dir = (argc > 1) ? argv[1] : "out/shader_tests";
    std::printf("ScarlettCompat shader pipeline - Phase 4 (RDNA2 instruction decode)\n");
    std::printf("output dir: %s (this harness writes nothing)\n", out_dir.c_str());

    test_assembler_words();
    test_format_buckets();
    test_walk_cases();
    test_region_and_seam();
    test_json();

    std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
