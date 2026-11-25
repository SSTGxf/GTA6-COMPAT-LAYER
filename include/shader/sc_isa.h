// ScarlettCompat shader pipeline - Phase 4: RDNA2 instruction decode.
// MIT. New file; not wired into the boot path.
//
// WHAT THIS IS
//   A stream stepper for the microcode regions the Phase 3 frontend finds. It
//   answers, from bytes only:
//     * what GFX10 instruction format is this 32-bit word?
//     * how many words does the instruction occupy (32/64-bit, plus the extra
//       dword a literal / SDWA / DPP form appends)?
//     * is it one of the documented shader terminators, and which?
//     * where does the stream stop, and exactly why?
//   It does not lift anything and does not name memory/image opcodes it cannot
//   verify. The point is to stay *synchronised* through a real payload and to
//   report the first offset where synchronisation is lost -- that is the data
//   needed to triage the first corpus dump.
//
// OBSERVED vs INFERRED
//   OBSERVED lines report a documented ISA fact applied to the bytes in front of
//   us: the 6/7/9-bit encoding fields below are from AMD's published GFX10
//   ISA/LLVM tables, and the length rules are documented ("vector memory
//   instructions are 64 bits, exports are 64 bits, LDS/GDS are 64 bits, scalar
//   ALU is 32 bits plus an optional literal dword"). The harness pins the table
//   against words produced by a real assembler, including
//       s_mov_b32 s1, 100      -> BE8103FF 00000064   (SOP1, SSRC0=0xFF literal)
//       v_mov_b32_e32 v0, s105 -> 7E000269             (VOP1)
//       flat_load_dword v1,... -> DC300000 017D0003    (FLAT, 64-bit)
//       s_setreg_imm32_b32 ... -> BA800801 00000003    (SOPK, 64-bit, immediate in dword 2)
//   SOPP's 64-bit relaxation form is deliberately *not* two dwords here: it is the
//   instruction plus 0xBF800000, which is the encoding of s_nop 0, so one dword per
//   instruction reaches the same offset without inventing a distinction from a real
//   `instruction; s_nop 0` pair.
//   INFERRED lines report where a documented rule is *not* enough. That is MIMG:
//   its length grows by one dword per non-sequential (NSA) address component,
//   the count lives in the instruction, but the field's bit position has not
//   been verified and the image opcode's address count is a table we will not
//   guess. The walk therefore stops at MIMG and says so (see the Phase 4
//   handoff, "the MIMG settlement").
//
//   No real XS2 payload has been decoded yet. Nothing here claims that a console
//   stream obeys these rules until the corpus proves it.
#ifndef SC_SHADER_ISA_H
#define SC_SHADER_ISA_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sc {

// GFX10 (RDNA1/RDNA2) instruction formats. Distinguished by the encoding field
// in the top bits of the 32-bit word (see sc_isa.cpp for the exact masks).
enum class sc_isa_format : uint32_t {
    unknown = 0,
    sop1,
    sop2,
    sopc,
    sopp,
    sopk,
    vop1,
    vop2,
    vopc,
    vop3,
    vop3p,
    vintrp,
    smem,
    ds,
    mubuf,
    mtbuf,
    mimg,
    flat,
    exp,
};

const char* sc_isa_format_name(sc_isa_format f);

// Documented SOPP instructions that end a shader. Opcodes from the RDNA2
// machine-readable ISA: s_endpgm = 1, s_endpgm_saved = 27,
// s_endpgm_ordered_ps_done = 30. s_code_end (31) is a code marker, NOT a
// terminator, and is deliberately absent.
enum class sc_isa_terminator : uint32_t {
    none = 0,
    s_endpgm,
    s_endpgm_saved,
    s_endpgm_ordered_ps_done,
};

const char* sc_isa_terminator_name(sc_isa_terminator t);

// True when this 32-bit word is a documented shader terminator. Lets callers
// scan a payload without duplicating the opcode table.
bool sc_isa_is_terminator_word(uint32_t word);

// Why an instruction is longer than one dword.
enum class sc_isa_extra : uint32_t {
    none = 0,
    literal, // 32-bit constant in the following dword (SSRC0/SRC0 == 0xFF)
    sdwa,    // SDWA sub-dword addressing dword (SRC0 == 0xF9)
    dpp,     // DPP modifier dword: DPP16 (SRC0 == 0xFA) or DPP8 (0xE9, 0xEA with fi)
    imm32,   // second dword of S_SETREG_IMM32_B32 (GFX10 SOPK opcode 0x15): the
             // opcode itself makes the instruction 64-bit, with the immediate in
             // bits 63:32. Every other SOPK form is one dword.
};

const char* sc_isa_extra_name(sc_isa_extra e);

struct sc_isa_instruction {
    uint32_t offset = 0;   // byte offset inside the walked stream
    uint32_t words = 0;    // 1..3; 0 means "length not determinable" (2 = 64-bit
                           // form, 3 = VOP3 with a literal dword)
    sc_isa_format format = sc_isa_format::unknown;
    sc_isa_extra extra = sc_isa_extra::none;
    sc_isa_terminator terminator = sc_isa_terminator::none;
    uint16_t opcode = 0;   // valid only when opcode_known
    bool opcode_known = false;
    uint32_t dwords[8] = {0, 0}; // both words of every 64-bit form; MIMG adds up to dim extra dwords
    uint32_t literal_value = 0;  // the literal dword when extra == literal (dword 1
                                 // for 32-bit forms, dword 2 for VOP3)

    bool is_terminator() const { return terminator != sc_isa_terminator::none; }
};

// Decodes one word in isolation: format, length, terminator kind and (where
// verified) opcode. A `words == 0` result means the length depends on
// information this module does not decode (MIMG NSA) or the encoding is not a
// GFX10 format.
sc_isa_instruction sc_isa_decode_word(uint32_t word);

// Total dwords the instruction occupies. A VOP3 literal selector lives in the
// SECOND dword (bits 8:0 / 17:9 / 26:18 == 0xFF), so a caller that only has
// dword 0 must pass the following dword here; sc_isa_decode_word alone cannot
// report that length. Returns inst.words when no such rule applies.
uint32_t sc_isa_length(const sc_isa_instruction& inst, uint32_t next_dword);

enum class sc_isa_stop : uint32_t {
    none = 0,
    terminator,             // stopped on a documented terminator
    no_terminator,          // ran out of words without a terminator
    trailing_after_terminator, // terminator reached before the end of the region
    unsupported_format,     // word at stop_offset is not a GFX10 encoding
    mimg_unsupported,       // MIMG: length needs the NSA count (see handoff)
    truncated,              // last instruction claims a dword that is not there
    not_word_aligned,       // region size is not a multiple of 4
    empty,
};

const char* sc_isa_stop_name(sc_isa_stop s);

struct sc_isa_walk {
    // True only when the stream walked cleanly and a terminator is the last word
    // of the region (no trailing padding, no stop, no truncation).
    bool synced = false;
    sc_isa_stop stop = sc_isa_stop::none;
    uint32_t stop_offset = 0;
    uint32_t stop_dword = 0;
    uint32_t bytes_covered = 0; // bytes consumed, including the terminator
    uint32_t literal_words = 0;
    sc_isa_terminator terminator = sc_isa_terminator::none; // set when stop == terminator
    std::vector<sc_isa_instruction> instructions;
    std::vector<std::string> evidence;       // each line starts observed:/inferred:
    std::vector<std::string> open_questions;
    std::string format_histogram_json() const; // {"sop1":3,...}, non-zero only
    std::string to_json() const;               // summary + first instructions
};

// Steps a buffer that starts on an instruction boundary.
sc_isa_walk sc_isa_walk_stream(const void* code, size_t size);

// Steps [begin, end) of a larger blob (e.g. a Phase 3 code region) and reports
// absolute offsets. No dependency on the frontend types on purpose.
sc_isa_walk sc_isa_walk_region(const void* blob, size_t blob_size, uint32_t begin, uint32_t end);

} // namespace sc

#endif // SC_SHADER_ISA_H
