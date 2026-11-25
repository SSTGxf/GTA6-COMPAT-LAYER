// ScarlettCompat shader pipeline - Phase 4 harness (RDNA2 instruction decode).
// MIT. Standalone: no game, no guest, no device, no layer, no emitter.
//
//   sc_isa_test.exe [output-dir]
//
// The encoding table is pinned against words produced by a real assembler
// (LLVM MC test vectors, quoted with their source line next to each fixture), so
// the decoder is not only self-consistent. Terminator opcodes come from AMD's
// machine-readable RDNA2 ISA (not from an assembler), and are labelled as such.
// The walk cases then cover every stop reason, including the settled MIMG stop,
// and the last section goes through the Phase 3 frontend so the
// frontend -> code region -> instruction stream seam is exercised end to end.
#include "shader/sc_wave.h"
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

// ------------------------------------------------------- assembler word table
// Source: LLVM test/MC/AMDGPU (gfx10 disassembly/assembly tests). Each comment
// keeps the quoted encoding line so the constant can be re-checked.
void test_assembler_words() {
    banner("decoder table pinned to real assembler output (LLVM MC, gfx10)");
    struct word_fixture {
        const char* label;
        uint32_t word;
        sc_isa_format format;
        uint32_t words;
        bool opcode_known;
        uint16_t opcode;
        sc_isa_extra extra;
    };
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
        // VOP3 opcode is bits 25:16 (10 bits); 2 is v_cmp_eq_f32 in that space,
        // confirmed across the 426 vop3 mnemonics of the conformance matrix.
        {"v_cmp_eq_f32_e64 s105, v0, s105", 0xD4020069u, sc_isa_format::vop3, 2, true, 2, sc_isa_extra::none},
        // v_cmp_eq_f32_sdwa s105, v0, s105 -> [0xf9,0xd2,0x04,0x7c, 0x00,0xe9,0x06,0x86]
        {"v_cmp_eq_f32_sdwa s105, v0, s105", 0x7C04D2F9u, sc_isa_format::vopc, 2, true, 2, sc_isa_extra::sdwa},
        // flat_load_dword v1, v[3:4] -> [0x00,0x00,0x30,0xdc, 0x03,0x00,0x7d,0x01]
        // FLAT opcode is bits 24:18: the range that maps all 88 mnemonics of
        // gfx10_asm_flat.s to exactly one value each (46 distinct values, since
        // flat_/global_/scratch_ share an opcode). Here bits 24:18 = 0x0C = 12.
        {"flat_load_dword v1, v[3:4]", 0xDC300000u, sc_isa_format::flat, 2, true, 12, sc_isa_extra::none},
        // s_setreg_imm32_b32 hwreg(HW_REG_MODE, 0, 2), 3
        //   gfx1010: [0x01,0x08,0x80,0xba,0x03,0x00,0x00,0x00]  -> 64-bit SOPK, opcode 0x15
        {"s_setreg_imm32_b32 hwreg(MODE,0,2), 3", 0xBA800801u, sc_isa_format::sopk, 2, true, 21,
         sc_isa_extra::imm32},
        // s_nop 0               -> encoding: [0x00,0x00,0x80,0xbf]
        // (also the padding dword of SOPP's 64-bit relaxation form, see below)
        {"s_nop 0", 0xBF800000u, sc_isa_format::sopp, 1, true, 0, sc_isa_extra::none},
        // s_branch 2            -> encoding: [0x02,0x00,0x82,0xbf]
        {"s_branch 2", 0xBF820002u, sc_isa_format::sopp, 1, true, 2, sc_isa_extra::none},
        // s_endpgm              -> encoding: [0x00,0x00,0x81,0xbf]
        {"s_endpgm", 0xBF810000u, sc_isa_format::sopp, 1, true, 1, sc_isa_extra::none},
    };

    for (const auto& f : fixtures) {
        const sc_isa_instruction inst = sc_isa_decode_word(f.word);
        const bool match = inst.format == f.format && inst.words == f.words && inst.extra == f.extra &&
                           inst.opcode_known == f.opcode_known && (!f.opcode_known || inst.opcode == f.opcode);
        CHECK(match);
        std::printf("   %-34s 0x%08x -> %-6s words=%u op=%s%u extra=%s%s\n", f.label, f.word,
                    sc_isa_format_name(inst.format), inst.words, inst.opcode_known ? "" : "?",
                    inst.opcode, sc_isa_extra_name(inst.extra),
                    inst.is_terminator() ? " TERMINATOR" : "");
        if (!match) {
            std::printf("      expected %s words=%u extra=%s\n", sc_isa_format_name(f.format), f.words,
                        sc_isa_extra_name(f.extra));
        }
    }
}

// ------------------------------------------------------------ terminators
// Opcode numbers: AMD machine-readable RDNA2 ISA (s_endpgm 1, s_endpgm_saved 27,
// s_endpgm_ordered_ps_done 30, s_code_end 31). SOPP layout puts the opcode at
// bits 22:16 over the 0xBF800000 base, so the whole words are as below. These are
// documented encodings, not assembler-verified ones.
void test_terminators() {
    banner("terminators: every documented SOPP form, and s_code_end excluded");
    struct term_case {
        const char* label;
        uint32_t word;
        sc_isa_terminator expected;
    };
    const term_case cases[] = {
        {"s_endpgm", 0xBF810000u, sc_isa_terminator::s_endpgm},
        {"s_endpgm_saved", 0xBF9B0000u, sc_isa_terminator::s_endpgm_saved},
        {"s_endpgm_ordered_ps_done", 0xBF9E0000u, sc_isa_terminator::s_endpgm_ordered_ps_done},
        {"s_code_end (marker, not a terminator)", 0xBF9F0000u, sc_isa_terminator::none},
        {"s_branch (SOPP opcode 2)", 0xBF820000u, sc_isa_terminator::none},
    };
    for (const auto& c : cases) {
        const sc_isa_instruction inst = sc_isa_decode_word(c.word);
        CHECK(inst.format == sc_isa_format::sopp);
        CHECK(inst.terminator == c.expected);
        CHECK(inst.is_terminator() == (c.expected != sc_isa_terminator::none));
        CHECK(sc_isa_is_terminator_word(c.word) == (c.expected != sc_isa_terminator::none));
        std::printf("   %-38s 0x%08x -> opcode=%u terminator=%s\n", c.label, c.word, inst.opcode,
                    sc_isa_terminator_name(inst.terminator));
    }

    // Each form ends a stream cleanly when it is the last word.
    for (const auto& c : cases) {
        if (c.expected == sc_isa_terminator::none) {
            continue;
        }
        const std::vector<uint8_t> bytes = u32s({0x7E000269u, c.word});
        const sc_isa_walk w = sc_isa_walk_stream(bytes.data(), bytes.size());
        CHECK(w.synced);
        CHECK(w.stop == sc_isa_stop::terminator);
        CHECK(w.terminator == c.expected);
        CHECK(w.stop_offset == bytes.size());
        std::printf("   walk ending in %-24s synced=%d bytes_covered=%u\n", c.label, w.synced ? 1 : 0,
                    w.bytes_covered);
    }

    // s_code_end at the end of a region is not a termination: the walk must keep
    // going and report that no terminator was found, not claim success.
    {
        const std::vector<uint8_t> bytes = u32s({0x7E000269u, 0xBF9F0000u});
        const sc_isa_walk w = sc_isa_walk_stream(bytes.data(), bytes.size());
        CHECK(!w.synced);
        CHECK(w.stop == sc_isa_stop::no_terminator);
        std::printf("   s_code_end last -> stop=%s (correctly not a terminator)\n", sc_isa_stop_name(w.stop));
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
        {"mimg", 0xF0000000u, sc_isa_format::mimg, 2},   {"unknown", 0xFFFFFFFFu, sc_isa_format::unknown, 0},
    };
    for (const auto& b : buckets) {
        const sc_isa_instruction inst = sc_isa_decode_word(b.word);
        CHECK(inst.format == b.format);
        CHECK(inst.words == b.words);
        std::printf("   %-8s 0x%08x -> %-8s words=%u\n", b.label, b.word, sc_isa_format_name(inst.format),
                    inst.words);
    }
}

// ------------------------------------------------ S_SETREG_IMM32_B32 (64-bit SOPK)
// GFX10 encodes this as SOPK opcode 0x15 with the immediate in the second dword
// (LLVM SOPK_Real64<0x015> for gfx6_gfx7_gfx10_gfx13; assembler bytes from the
// gfx1010 setreg test lines). It used to be walked as one dword, which desynced
// the rest of any stream containing it.
void test_setreg_imm32() {
    banner("S_SETREG_IMM32_B32: verified 64-bit SOPK form is stepped, not desynced");

    // hwreg(HW_REG_MODE, 0, 2), 3 on gfx1010.
    const uint32_t word0 = 0xBA800801u;
    const uint32_t word1 = 0x00000003u;
    const sc_isa_instruction inst = sc_isa_decode_word(word0);
    CHECK(inst.format == sc_isa_format::sopk);
    CHECK(inst.words == 2);
    CHECK(inst.opcode_known);
    CHECK(inst.opcode == 0x15);
    CHECK(inst.extra == sc_isa_extra::imm32);
    CHECK(std::string(sc_isa_extra_name(inst.extra)) == "imm32");
    std::printf("   0x%08x -> %s opcode=0x%x words=%u extra=%s\n", word0, sc_isa_format_name(inst.format),
                inst.opcode, inst.words, sc_isa_extra_name(inst.extra));

    // Every other SOPK instruction stays one dword: s_addk_i32 s2, 6.
    const sc_isa_instruction addk = sc_isa_decode_word(0xB7020006u);
    CHECK(addk.format == sc_isa_format::sopk);
    CHECK(addk.opcode == 0x0e);
    CHECK(addk.words == 1);
    CHECK(addk.extra == sc_isa_extra::none);
    std::printf("   0x%08x -> s_addk_i32 opcode=0x%x words=%u (unchanged)\n", 0xB7020006u, addk.opcode,
                addk.words);

    // The stream keeps synchronisation across it: vop1; setreg+imm; s_endpgm.
    {
        const std::vector<uint8_t> bytes = u32s({0x7E000269u, word0, word1, 0xBF810000u});
        const sc_isa_walk w = sc_isa_walk_stream(bytes.data(), bytes.size());
        CHECK(w.synced);
        CHECK(w.stop == sc_isa_stop::terminator);
        CHECK(w.bytes_covered == bytes.size());
        CHECK(w.instructions.size() == 3);
        CHECK(w.instructions[1].offset == 4);
        CHECK(w.instructions[1].words == 2);
        CHECK(w.instructions[1].dwords[1] == word1);
        CHECK(w.instructions[2].offset == 12);
        std::printf("   walk through it -> synced=%d instructions=%zu bytes_covered=%u next_offset=%u\n",
                    w.synced ? 1 : 0, w.instructions.size(), w.bytes_covered, w.instructions[2].offset);
    }

    // A setreg at the very end is a truncation, not a silent one-word step.
    {
        const std::vector<uint8_t> cut = u32s({word0});
        const sc_isa_walk w = sc_isa_walk_stream(cut.data(), cut.size());
        CHECK(!w.synced);
        CHECK(w.stop == sc_isa_stop::truncated);
        CHECK(w.stop_offset == 0);
        CHECK(w.instructions.empty());
        std::printf("   missing immediate -> stop=%s at %u\n", sc_isa_stop_name(w.stop), w.stop_offset);
    }

    // The GFX9 numbering (0x14) is not a GFX10 encoding: documented here so the
    // distinction is visible rather than discovered by a wrong step later.
    const sc_isa_instruction gfx9 = sc_isa_decode_word(0xBA000801u);
    CHECK(gfx9.format == sc_isa_format::sopk);
    CHECK(gfx9.opcode == 0x14);
    CHECK(gfx9.words == 1);
    std::printf("   0x%08x -> SOPK opcode=0x%x words=%u (GFX9 numbering, not stepped as the GFX10 form)\n",
                0xBA000801u, gfx9.opcode, gfx9.words);
}

// ------------------------------------------ SOPP 64-bit relaxation / padding
// LLVM's SOPP_Real_64 (used only by the branch-relaxation `_pad_s_nop` forms)
// keeps the normal 32-bit SOPP word and appends a second dword with
// Inst{63-55} = 0x17f and everything else zero, i.e. 0xBF800000 -- exactly the
// assembler encoding of `s_nop 0`. So the 64-bit form is byte-identical to a
// 32-bit SOPP followed by an s_nop 0, and one-dword stepping is the only reading
// that does not invent a distinction.
void test_sopp_relaxation() {
    banner("SOPP 64-bit relaxation form: the pad dword is s_nop 0, so one-dword stepping is exact");

    // Same derivation as the encoding class: bits 63:55 hold the SOPP prefix.
    const uint32_t pad = 0xBF800000u;
    CHECK(pad == (0x17Fu << 23));
    const sc_isa_instruction pad_inst = sc_isa_decode_word(pad);
    CHECK(pad_inst.format == sc_isa_format::sopp);
    CHECK(pad_inst.words == 1);
    CHECK(pad_inst.opcode == 0); // s_nop
    CHECK(!pad_inst.is_terminator());
    CHECK(!sc_isa_is_terminator_word(pad));
    std::printf("   pad 0x%08x == 0x17F<<23 -> %s opcode=%u (s_nop) words=%u terminator=%s\n", pad,
                sc_isa_format_name(pad_inst.format), pad_inst.opcode, pad_inst.words,
                sc_isa_terminator_name(pad_inst.terminator));

    // s_branch 2 -> [0x02,0x00,0x82,0xbf] (llvm/test/MC/AMDGPU/sopp.s).
    const uint32_t branch = 0xBF820002u;
    const sc_isa_instruction br = sc_isa_decode_word(branch);
    CHECK(br.format == sc_isa_format::sopp);
    CHECK(br.words == 1);
    CHECK(br.opcode == 2);

    // Relaxed form in a stream: branch + pad + endpgm. The pad is a nop, and the
    // instruction after the pair is at the same offset under either reading.
    {
        const std::vector<uint8_t> bytes = u32s({branch, pad, 0xBF810000u});
        const sc_isa_walk w = sc_isa_walk_stream(bytes.data(), bytes.size());
        CHECK(w.synced);
        CHECK(w.instructions.size() == 3);
        CHECK(w.bytes_covered == bytes.size());
        CHECK(w.instructions[0].offset == 0);
        CHECK(w.instructions[1].offset == 4);
        CHECK(w.instructions[1].opcode == 0);
        CHECK(w.instructions[2].offset == 8);
        std::printf("   branch + pad + endpgm -> synced=%d offsets=%u,%u,%u\n", w.synced ? 1 : 0,
                    w.instructions[0].offset, w.instructions[1].offset, w.instructions[2].offset);
    }

    // Without a terminator both words are still consumed: no desync, just no end.
    {
        const std::vector<uint8_t> bytes = u32s({branch, pad});
        const sc_isa_walk w = sc_isa_walk_stream(bytes.data(), bytes.size());
        CHECK(!w.synced);
        CHECK(w.stop == sc_isa_stop::no_terminator);
        CHECK(w.instructions.size() == 2);
        CHECK(w.bytes_covered == 8);
        std::printf("   branch + pad alone -> stop=%s, %zu instructions, %u bytes (both words consumed)\n",
                    sc_isa_stop_name(w.stop), w.instructions.size(), w.bytes_covered);
    }

    // The same bytes also spell vop1 + genuine s_nop 0 padding. Nothing in the
    // bytes separates the two, which is why no length rule is attached to them.
    {
        const std::vector<uint8_t> genuine = u32s({0x7E000269u, pad, 0xBF810000u});
        const sc_isa_walk w = sc_isa_walk_stream(genuine.data(), genuine.size());
        CHECK(w.synced);
        CHECK(w.instructions.size() == 3);
        std::printf("   vop1 + s_nop 0 + endpgm -> synced=%d instructions=%zu (same reading)\n", w.synced ? 1 : 0,
                    w.instructions.size());
    }

    // No GFX10 terminator has a 64-bit form (S_ENDPGM, S_ENDPGM_SAVED and
    // S_ENDPGM_ORDERED_PS_DONE are all SOPP_Real_32 in LLVM), but padding after a
    // terminator is reported rather than swallowed.
    {
        const std::vector<uint8_t> bytes = u32s({0xBF810000u, pad});
        const sc_isa_walk w = sc_isa_walk_stream(bytes.data(), bytes.size());
        CHECK(!w.synced);
        CHECK(w.stop == sc_isa_stop::trailing_after_terminator);
        CHECK(w.stop_offset == 4);
        std::printf("   endpgm + pad -> stop=%s at %u (padding is reported, never swallowed)\n",
                    sc_isa_stop_name(w.stop), w.stop_offset);
    }
}

// --------------------------------------------------------------- stream walks
void test_walk_cases() {
    banner("stream walk: every stop reason");

    struct walk_case {
        const char* label;
        std::vector<uint8_t> bytes;
    } cases[] = {
        {"literal + vop1 + endpgm", u32s({0xBE8103FFu, 0x00000064u, 0x7E000269u, 0xBF810000u})},
        {"sdwa extra dword", u32s({0x7E0002F9u, 0x00861669u, 0xBF810000u})},
        {"flat 64-bit", u32s({0xDC300000u, 0x017D0003u, 0xBF810000u})},
        {"two vop2 then endpgm", u32s({0x00000000u, 0x00000000u, 0xBF810000u})},
        {"dpp extra dword", u32s({0x7E0002FAu, 0x00000000u, 0xBF810000u})},
        {"ordered_ps_done", u32s({0xBE810302u, 0xBF9E0000u})},
    };
    CHECK(cases[0].bytes.size() == 16);

    for (const auto& c : cases) {
        const sc_isa_walk w = sc_isa_walk_stream(c.bytes.data(), c.bytes.size());
        CHECK(w.synced);
        CHECK(w.stop == sc_isa_stop::terminator);
        CHECK(w.stop_offset == c.bytes.size());
        CHECK(w.bytes_covered == c.bytes.size());
        CHECK(!w.instructions.empty());
        CHECK(w.instructions.back().is_terminator());
        std::printf("   %-24s synced=%d instructions=%zu literal_words=%u terminator=%s\n", c.label, w.synced ? 1 : 0,
                    w.instructions.size(), w.literal_words, sc_isa_terminator_name(w.terminator));
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
        CHECK(w.terminator == sc_isa_terminator::none);
        std::printf("   no terminator -> stop=%s\n", sc_isa_stop_name(w.stop));
    }
    // Trailing bytes after the terminator.
    {
        const std::vector<uint8_t> trailing = u32s({0xBF810000u, 0x7E000269u});
        const sc_isa_walk w = sc_isa_walk_stream(trailing.data(), trailing.size());
        CHECK(!w.synced);
        CHECK(w.stop == sc_isa_stop::trailing_after_terminator);
        CHECK(w.stop_offset == 4);
        std::printf("   trailing after terminator -> stop=%s at %u\n", sc_isa_stop_name(w.stop), w.stop_offset);
    }
    // MIMG is sized from its dim field (bits 2:1): measured on all 232 text-bearing
    // MIMG fixture rows as exactly one extra dword per dim code (0->2, 1->3, 2->4).
    {
        const std::vector<uint8_t> img = u32s({0xF0000000u, 0x00000000u, 0xBF810000u});
        const sc_isa_walk w = sc_isa_walk_stream(img.data(), img.size());
        CHECK(w.synced);
        CHECK(w.instructions.size() == 2);
        CHECK(w.bytes_covered == 12);
        CHECK(sc_isa_decode_word(0xF0000000u).words == 2);
        CHECK(sc_isa_decode_word(0xF0000002u).words == 3);
        CHECK(sc_isa_decode_word(0xF0000004u).words == 4);
        std::printf("   mimg sized by dim: words=%u/%u/%u synced=%d",
                    sc_isa_decode_word(0xF0000000u).words, sc_isa_decode_word(0xF0000002u).words,
                    sc_isa_decode_word(0xF0000004u).words, w.synced ? 1 : 0);
        std::printf("%c", 10);
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
std::vector<uint8_t> make_xs2_payload(const std::vector<uint8_t>& code, uint32_t terminator_word = 0xBF810000u) {
    const uint32_t code_words = static_cast<uint32_t>(code.size() / 4);
    const uint32_t total_words = 4 + code_words;
    std::vector<uint8_t> blob(total_words * 4u, 0);
    wr32(blob, 0, total_words * 4u); // payload size, corroborates the prober
    wr32(blob, 4, 16u);              // code offset
    wr32(blob, 8, 1u);
    wr32(blob, 12, 3u);
    std::memcpy(blob.data() + 16, code.data(), code.size());
    wr32(blob, 16 + (code_words - 1) * 4u, terminator_word); // last code word
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

    // Frontend -> code region -> instruction stream, for each documented
    // termination form the prober accepts.
    const uint32_t terminators[] = {0xBF810000u, 0xBF9B0000u, 0xBF9E0000u};
    for (uint32_t term : terminators) {
        const std::vector<uint8_t> payload = make_xs2_payload(code, term);
        const sc_shader_classification c = sc_classify_shader(payload.data(), payload.size());
        std::printf("   classify (term 0x%08x) -> kind=%s regions=%zu found=%d\n", term, c.kind_name(),
                    c.xs2.code_regions.size(), c.xs2.found ? 1 : 0);
        CHECK(c.kind == sc_payload_kind::rdna2_xs2);
        CHECK(c.xs2.code_regions.size() == 1);
        if (c.xs2.code_regions.empty()) {
            continue;
        }
        const sc_xs2_code_region& r = c.xs2.code_regions[0];
        const sc_isa_walk w = sc_isa_walk_region(payload.data(), payload.size(), r.begin, r.end);
        CHECK(w.synced);
        CHECK(w.instructions.back().is_terminator());
        CHECK(w.stop_offset == r.end);
        std::printf("   walk [%u,%u) -> synced=%d instructions=%zu terminator=%s\n", r.begin, r.end, w.synced ? 1 : 0,
                    w.instructions.size(), sc_isa_terminator_name(w.terminator));
        if (term == 0xBF810000u) {
            for (const auto& line : w.evidence) {
                std::printf("      %s\n", line.c_str());
            }
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
    CHECK(json.find("\"stop\":\"terminator\"") != std::string::npos);
    CHECK(json.find("\"terminator\":\"s_endpgm\"") != std::string::npos);
    // A setreg walk must expose the 64-bit form in JSON as well.
    const std::vector<uint8_t> setreg = u32s({0xBA800801u, 0x00000003u, 0xBF810000u});
    const sc_isa_walk sw = sc_isa_walk_stream(setreg.data(), setreg.size());
    const std::string sjson = sw.to_json();
    CHECK(sw.synced);
    CHECK(sjson.find("\"extra\":\"imm32\"") != std::string::npos);
    CHECK(sjson.find("\"words\":2") != std::string::npos);
    CHECK(sjson.find("\"sopk\":1") != std::string::npos);
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

// ------------------------------------------------------------ memory operands
// Words copied from the fixture rows named below; every position was measured
// over the whole harvested gfx10 set (measure_memory_operands.py).
void test_memory_operands() {
    banner("memory formats: the measured operand fields");
    auto decode2 = [](uint32_t w0, uint32_t w1, sc_wave_decode& out) {
        std::vector<uint8_t> bytes;
        for (uint32_t w : {w0, w1}) {
            for (int shift = 0; shift < 32; shift += 8) {
                bytes.push_back(static_cast<uint8_t>((w >> shift) & 0xffu));
            }
        }
        out = sc_decode_wave(sc_isa_walk_stream(bytes.data(), bytes.size()));
    };
    {
        // gfx10_asm_mubuf.s: buffer_atomic_add v255, off, s[8:11], s3 offset:4095
        sc_wave_decode d;
        decode2(0xE0C80FFFu, 0x0302FF00u, d);
        CHECK(!d.instructions.empty());
        if (!d.instructions.empty()) {
            const sc_instruction_operands& io = d.instructions[0];
            CHECK(io.format == sc_isa_format::mubuf);
            CHECK(io.has_mem_vdst && io.mem_vdst.file == sc_operand_file::vgpr &&
                  io.mem_vdst.index == 255);
            CHECK(io.has_mem_srsrc && io.mem_srsrc.index == 8);
            // off -> 39:32 == 0; offset:4095 -> word0 11:0; s3 -> soffset 63:56.
            CHECK(io.has_mem_mubuf_vaddr && io.mem_mubuf_vaddr == 0u);
            CHECK(io.has_mem_mubuf_offset && io.mem_mubuf_offset == 0xFFFu);
            CHECK(io.has_mem_soffset && io.mem_soffset == 3u);
        }
        // gfx10_asm_mubuf.s: buffer_atomic_add v5, off, s[12:15], s3 offset:4095
        sc_wave_decode d2;
        decode2(0xE0C80FFFu, 0x03030500u, d2);
        CHECK(!d2.instructions.empty());
        if (!d2.instructions.empty()) {
            CHECK(d2.instructions[0].mem_vdst.index == 5);
            CHECK(d2.instructions[0].mem_srsrc.index == 12);
            CHECK(d2.instructions[0].mem_mubuf_vaddr == 0u);
            CHECK(d2.instructions[0].mem_mubuf_offset == 0xFFFu);
            CHECK(d2.instructions[0].mem_soffset == 3u);
        }
    }
    {
        // MIMG fixture rows decoded through sc_decode_instruction: the caller supplies
        // the words and their count, so these assertions do not depend on the stream
        // walker. vdst 47:40, vaddr 39:32, srsrc 52:48 (V# base / 4).
        { // image_load v[1:4], [v2, v3], s[4:11] dmask:0xf dim:S
            const uint32_t w[] = {0xF0001F0Au, 0x00010102u, 0x00000003u};
            const sc_instruction_operands io = sc_decode_instruction(w, 3u);
            CHECK(io.format == sc_isa_format::mimg);
            CHECK(io.has_mem_vdst && io.mem_vdst.index == 1u);
            CHECK(io.has_mem_vaddr && io.mem_vaddr.index == 2u);
            CHECK(io.has_mem_srsrc && io.mem_srsrc.index == 4u);
            CHECK(io.has_mem_dmask && io.mem_dmask == 0xFu);   // dmask:0xf, word0 11:8
            CHECK(io.has_mem_nsa && io.mem_nsa_slots == 5u);   // dim 2:1 == 1: one extra dword
            CHECK(io.mem_nsa_vgpr[0] == 2u && io.mem_nsa_vgpr[1] == 3u);
        }
        { // image_load v[0:3], [v4, v5, v6], s[8:15] dmask:0xf d
            const uint32_t w[] = {0xF0001F12u, 0x00020004u, 0x00000605u};
            const sc_instruction_operands io = sc_decode_instruction(w, 3u);
            CHECK(io.format == sc_isa_format::mimg);
            CHECK(io.has_mem_vdst && io.mem_vdst.index == 0u);
            CHECK(io.has_mem_vaddr && io.mem_vaddr.index == 4u);
            CHECK(io.has_mem_srsrc && io.mem_srsrc.index == 8u);
            CHECK(io.has_mem_dmask && io.mem_dmask == 0xFu);   // dmask:0xf, word0 11:8
            CHECK(io.has_mem_nsa && io.mem_nsa_slots == 5u);   // dim 2:1 == 1: one extra dword
            CHECK(io.mem_nsa_vgpr[0] == 4u && io.mem_nsa_vgpr[1] == 5u && io.mem_nsa_vgpr[2] == 6u);
        }
        { // image_load v[0:3], [v4, v5, v6, v7], s[8:15] dmask:0
            const uint32_t w[] = {0xF0001F3Au, 0x00020004u, 0x00070605u};
            const sc_instruction_operands io = sc_decode_instruction(w, 3u);
            CHECK(io.format == sc_isa_format::mimg);
            CHECK(io.has_mem_vdst && io.mem_vdst.index == 0u);
            CHECK(io.has_mem_vaddr && io.mem_vaddr.index == 4u);
            CHECK(io.has_mem_srsrc && io.mem_srsrc.index == 8u);
            CHECK(io.has_mem_dmask && io.mem_dmask == 0xFu);   // dmask:0xf, word0 11:8
            CHECK(io.has_mem_nsa && io.mem_nsa_slots == 5u);   // dim 2:1 == 1: one extra dword
            CHECK(io.mem_nsa_vgpr[0] == 4u && io.mem_nsa_vgpr[1] == 5u && io.mem_nsa_vgpr[2] == 6u);
        }
    }
    {
        // gfx10_asm_smem.s: s_atomic_add s5, s[2:3], 0x64
        sc_wave_decode d;
        decode2(0xF6080141u, 0xFA000064u, d);
        CHECK(!d.instructions.empty());
        if (!d.instructions.empty()) {
            CHECK(d.instructions[0].has_mem_sdata && d.instructions[0].mem_sdata.index == 5);
            // s[2:3] -> (word0 5:0) * 2; 0x64 -> dword1 20:0; the 0x7d at 31:25 is the
            // offset-form marker, so no s[] soffset is claimed here.
            CHECK(d.instructions[0].has_mem_sbase && d.instructions[0].mem_sbase == 2u);
            CHECK(d.instructions[0].has_mem_smem_offset && d.instructions[0].mem_smem_offset == 0x64u);
            CHECK(d.instructions[0].has_mem_smem_soffset && d.instructions[0].mem_smem_soffset == 0x7du);
        }
    }
    {
        // gfx10_asm_ds.s: ds_add_f32 v0, v1 offset:4660
        sc_wave_decode d;
        decode2(0xD8541234u, 0x00000100u, d);
        CHECK(!d.instructions.empty());
        if (!d.instructions.empty()) {
            const sc_instruction_operands& io = d.instructions[0];
            CHECK(io.has_mem_offset && io.mem_offset == 4660);
            // Printed DS registers are dword1 bytes: ds_add_f32 v0, v1 is (7:0, 15:8).
            CHECK(io.has_mem_ds_roles);
            CHECK(io.mem_ds_role[0] == 0u && io.mem_ds_role[1] == 1u);
            CHECK(!io.has_mem_vdst && !io.has_mem_vaddr);
        }
    }
    {
        // gfx10_asm_mimg.s:269  image_sample v[64:66], v32, s[4:11], s[100:103]
        // dmask:0x7 dim:SQ_RSRC_IMG_1D -> 0xF0800700 0x03214020. The sampler is
        // dword1 25:21 in 4-SGPR units: (0x03214020 >> 21) & 0x1F == 25 -> s100.
        {
            const uint32_t w[] = {0xF0800700u, 0x03214020u};
            const sc_instruction_operands io = sc_decode_instruction(w, 2u);
            CHECK(io.format == sc_isa_format::mimg);
            CHECK(io.has_mem_srsrc && io.mem_srsrc.index == 4u);
            CHECK(io.has_mem_ssamp && io.mem_ssamp == 100u);
        }
        // gfx10_asm_flat.s:200  flat_load_dword v5, v[1:2] -> 0xDC300000 0x057D0001:
        // vdst 63:56 = 5, vaddr 39:32 = 1, saddr 55:48 = 0x7D (off).
        {
            const uint32_t w[] = {0xDC300000u, 0x057D0001u};
            const sc_instruction_operands io = sc_decode_instruction(w, 2u);
            CHECK(io.format == sc_isa_format::flat);
            CHECK(io.has_mem_flat);
            CHECK(io.mem_flat_vdst == 5u && io.mem_flat_vaddr == 1u);
            CHECK(io.mem_flat_saddr == 0x7Du && io.mem_flat_saddr_off);
        }
        // gfx10_asm_flat.s:266  global_atomic_fcmpswap v5, v[1:2], v[2:3], off
        // offset:-1 glc -> 0xDCF98FFF 0x057D0201: vdst 5, vaddr 1, vdata 2, saddr off.
        {
            const uint32_t w[] = {0xDCF98FFFu, 0x057D0201u};
            const sc_instruction_operands io = sc_decode_instruction(w, 2u);
            CHECK(io.format == sc_isa_format::flat);
            CHECK(io.mem_flat_vdst == 5u && io.mem_flat_vaddr == 1u && io.mem_flat_vdata == 2u);
            CHECK(io.mem_flat_saddr_off);
        }
    }
    {
        // gfx10_asm_exp.s: exp mrtz, v0, v0, v0, v0 - the target is bits 7:4 and
        // varies per row, so `exp` has no opcode field: an operand, not an opcode.
        // (MIMG operand words are asserted above through sc_decode_instruction, and
        // the walker itself steps MIMG using the measured 2 + dim length.)
        CHECK(!sc_isa_decode_word(0xF800008Fu).opcode_known);
    }
}
int main(int argc, char** argv) {
    const std::string out_dir = (argc > 1) ? argv[1] : "out/shader_tests";
    std::printf("ScarlettCompat shader pipeline - Phase 4 (RDNA2 instruction decode)\n");
    std::printf("output dir: %s (this harness writes nothing)\n", out_dir.c_str());

    test_assembler_words();
    test_terminators();
    test_format_buckets();
    test_setreg_imm32();
    test_sopp_relaxation();
    test_walk_cases();
    test_region_and_seam();
    test_json();
    test_memory_operands();

    std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
