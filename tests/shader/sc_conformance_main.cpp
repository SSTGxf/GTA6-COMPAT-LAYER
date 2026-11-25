// ScarlettCompat shader pipeline - GFX10 decode conformance matrix (MIT).
// Standalone: no game, no guest, no layer, no GPU.
//
//   sc_conformance_test.exe [fixtures.tsv] [report-dir]
//
// WHAT THIS IS
//   Harvested assembler-pinned encodings (tests/shader/fixtures/gfx10_mc_fixtures.tsv,
//   generated from LLVM's gfx10 MC tests -- SYNTHETIC, assembler output, never
//   captured game bytes) are decoded by our Phase 4/5 code and compared against
//   the mnemonic llvm-mc printed for the same bytes:
//
//     * format   : the source file's instruction family is the independent signal
//                  (gfx10_asm_vop2.s -> a VOP2 word), so a mismatch is a bug here
//     * length   : the dwords the fixture encodes must be the dwords we consume
//                  (64-bit forms and the extra dword for literal/SDWA/DPP/imm32)
//     * opcode   : every (format, mnemonic) pair must resolve to one opcode value,
//                  plus three opcodes already pinned by hand elsewhere
//     * operands : for the formats we decode (VOP1/VOP2/VOPC srcs, SOP1), each
//                  plain register token must classify to the same file and index
//
//   Everything we deliberately do not decode is reported as a work-queue item
//   with a count, not as a pass and not as a failure: VOP3/VOP3P operand fields,
//   VOPC's sdst, SOP2/SOPC/SOPK/SOPP operands, SDWA/DPP src0 (suppressed by
//   design), and operands the module does not name (vcc, exec, literals).
//
//   The run writes a Markdown report with a per-format pass rate and the failure
//   list, so "how much of the documented encoding do we actually decode" is a
//   measured number instead of a claim.
#include "shader/sc_isa.h"
#include "shader/sc_wave.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <array>
#include <map>
#include <set>
#include <sstream>
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

struct fixture {
    std::string source;
    uint32_t line = 0;
    std::string kind;
    std::string text;
    std::vector<uint32_t> words;
};

bool load_fixtures(const char* path, std::vector<fixture>& out, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = std::string("cannot open ") + path;
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::vector<std::string> fields;
        size_t start = 0;
        while (start <= line.size()) {
            const size_t tab = line.find('\t', start);
            fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
            if (tab == std::string::npos) {
                break;
            }
            start = tab + 1;
        }
        if (fields.size() != 5) {
            continue;
        }
        fixture f;
        f.source = fields[0];
        f.kind = fields[2];
        f.text = fields[3];
        try {
            f.line = static_cast<uint32_t>(std::stoul(fields[1]));
        } catch (...) {
            continue;
        }
        std::istringstream words(fields[4]);
        std::string token;
        while (words >> token) {
            f.words.push_back(static_cast<uint32_t>(std::stoul(token, nullptr, 16)));
        }
        if (f.words.empty()) {
            continue;
        }
        out.push_back(f);
    }
    if (out.empty()) {
        error = std::string("no fixtures parsed from ") + path;
        return false;
    }
    return true;
}

std::string format_name(sc_isa_format f) { return std::string(sc_isa_format_name(f)); }

bool has_suffix(const std::string& s, const char* suffix) {
    const size_t len = std::strlen(suffix);
    return s.size() > len && s.compare(s.size() - len, len, suffix) == 0;
}

std::string base_mnemonic(const std::string& text) {
    size_t end = text.find_first_of(" \t");
    std::string mnemonic = text.substr(0, end);
    for (const char* suffix : {"_e32", "_e64"}) {
        const size_t len = std::strlen(suffix);
        if (mnemonic.size() > len && mnemonic.compare(mnemonic.size() - len, len, suffix) == 0) {
            mnemonic.erase(mnemonic.size() - len);
        }
    }
    return mnemonic;
}

// The source file's family is the independent expectation. Where a file mixes
// families (sop.s, dpp8.s, dpp16.s) the allowed set is stated explicitly rather
// than pretended to be exact.
std::vector<sc_isa_format> expected_formats(const std::string& kind, const std::string& text) {
    // The file name is only the expected *encoding* family for the plain 32-bit
    // forms. An `_e64` mnemonic inside gfx10_asm_vop1.s is a VOP3 encoding
    // (v_mov_b32_e64, v_cmp_eq_f32_e64, ...), and the fixture proves which it is.
    const size_t token_end = text.find_first_of(" \t");
    const std::string raw_mnemonic = text.substr(0, token_end);
    if (has_suffix(raw_mnemonic, "_e64")) {
        return {sc_isa_format::vop3, sc_isa_format::vop3p};
    }
    if (kind == "vop1") return {sc_isa_format::vop1};
    if (kind == "vop2") return {sc_isa_format::vop2};
    if (kind == "vopc" || kind == "vopcx" || kind == "vopc_sdwa") return {sc_isa_format::vopc};
    // VOP3a (0x34) and VOP3b (0x35) both decode to sc_isa_format::vop3, so the
    // expectation is that single format name; vop3.s also covers VOP3P.
    if (kind == "vopc_e64") return {sc_isa_format::vop3};
    if (kind == "vop3") return {sc_isa_format::vop3, sc_isa_format::vop3p};
    if (kind == "dpp8" || kind == "dpp16") {
        return {sc_isa_format::vop1, sc_isa_format::vop2, sc_isa_format::vopc};
    }
    if (kind == "sop") {
        return {sc_isa_format::sop1, sc_isa_format::sop2, sc_isa_format::sopc, sc_isa_format::sopk,
                sc_isa_format::sopp};
    }
    // One memory family per file.
    if (kind == "ds") return {sc_isa_format::ds};
    if (kind == "exp") return {sc_isa_format::exp};
    if (kind == "flat") return {sc_isa_format::flat};
    if (kind == "mimg") return {sc_isa_format::mimg};
    if (kind == "mubuf") return {sc_isa_format::mubuf};
    if (kind == "smem") return {sc_isa_format::smem};
    // Files whose name says nothing about the encoding family; what each one
    // contains was read off its own rows rather than guessed from the name:
    //   gfx1011_dlops.s          every row is 0x33 (VOP3P: the dot instructions)
    //   gfx1013.s                every row is 0x3C (MIMG: image_bvh_*/image_msaa_*)
    //   gfx1030_new.s            every row is 0x38 (MUBUF: buffer_atomic_csub/...)
    //   gfx10-vop2be-literal.s   every row is a 32-bit VOP2 with a literal
    //   gfx10-constant-bus.s     VOP2 (v_madak) and VOP3 (v_add_f32_e64, ...)
    // gfx1011_dlops.s:
    //   dot2/dot4/dot8 (four printed sources)  -> 0x33 VOP3P
    //   dot2c/dot4c (three printed sources)    -> 0x0200040a, a 32-bit VOP2 form
    if (kind == "11_dlops") return {sc_isa_format::vop3p, sc_isa_format::vop2};
    if (kind == "13") return {sc_isa_format::mimg};
    // gfx1030_new.s is a grab bag of "new on gfx1030" instructions from many
    // families, so its allowed set is stated explicitly (measured by classifying
    // every row's leading dword) rather than pretended to be exact:
    //   buffer_atomic_csub         0x38 MUBUF      global_*              0x37 FLAT
    //   image_bvh*_intersect_ray   0x3C MIMG       s_*                   scalar
    //   v_* (_e64 / _e32)          VOP3 / VOP2
    if (kind == "30_new")
        return {sc_isa_format::mubuf, sc_isa_format::flat, sc_isa_format::mimg, sc_isa_format::sop1,
                sc_isa_format::sop2, sc_isa_format::sopc, sc_isa_format::sopk, sc_isa_format::sopp,
                sc_isa_format::vop2, sc_isa_format::vop3};
    if (kind == "vop2be-literal") return {sc_isa_format::vop2};
    if (kind == "constant-bus") return {sc_isa_format::vop2, sc_isa_format::vop3};
    return {};
}

struct op_token {
    bool plain = false; // a bare vN/sN or v[a:b]/s[a:b] (modifiers stripped)
    bool vgpr = false;
    bool pair = false;
    bool neg = false;   // printed -vN / -|vN|
    bool abs = false;   // printed |vN|
    bool sext = false;  // printed sext(vN) (SDWA source modifier)
    uint32_t index = 0;
    bool special = false;      // vcc_lo / vcc / exec / m0 / ttmpN (normalised)
    std::string special_name;
    bool number = false;       // a literal or inline constant printed as a number
    bool number_is_real = false;
    double number_value = 0.0;
    bool hwreg = false;        // hwreg(<id>, <offset>, <size>) (SOPK selector word)
    uint32_t hwreg_id = 0, hwreg_offset = 0, hwreg_size = 0;
    bool param = false;        // p0 / p10 / p20 (VINTERP param selector)
    uint32_t param_index = 0;
};

bool token_unrecognized(const op_token& t) {
    return !t.plain && !t.number && !t.special && !t.hwreg && !t.param;
}

bool token_is_number(const std::string& t, bool& is_real, double& value) {
    size_t i = (!t.empty() && t[0] == '-') ? 1 : 0;
    bool hex = false;
    if (t.size() > i + 2 && t[i] == '0' && (t[i + 1] == 'x' || t[i + 1] == 'X')) {
        hex = true;
        i += 2;
    }
    if (i >= t.size()) {
        return false;
    }
    bool dots = false;
    for (size_t k = i; k < t.size(); ++k) {
        const char c = t[k];
        if (c == '.') {
            if (hex) {
                return false;
            }
            dots = true;
            continue;
        }
        const bool digit = c >= '0' && c <= '9';
        const bool hexdigit = digit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (hex ? !hexdigit : !digit) {
            return false;
        }
    }
    is_real = dots;
    if (hex) {
        value = static_cast<double>(std::strtoull(t.c_str() + i, nullptr, 16));
    } else {
        value = std::strtod(t.c_str(), nullptr);
    }
    return true;
}

bool token_is_special(const std::string& t, std::string& name) {
    if (t == "vcc" || t == "exec" || t == "vcc_lo" || t == "vcc_hi" || t == "exec_lo" ||
        t == "exec_hi" || t == "m0") {
        name = t;
        return true;
    }
    if (t.rfind("ttmp", 0) == 0) {
        std::string digits = t.substr(4);
        if (!digits.empty() && digits[0] == '[') {
            const size_t colon = digits.find(':');
            digits = digits.substr(1, colon == std::string::npos ? std::string::npos : colon - 1);
        }
        if (digits.empty()) {
            return false;
        }
        for (char c : digits) {
            if (c < '0' || c > '9') {
                return false;
            }
        }
        name = "ttmp" + std::to_string(std::strtoul(digits.c_str(), nullptr, 10));
        return true;
    }
    return false;
}

// Which printed names a decoded special-register field may carry. 106/126 are the
// low halves of vcc/exec; the 64-bit forms print the bare pair name (`vcc`,
// `exec`), which is a width fact this module does not decode, so both spellings
// are accepted for the same field.
bool special_field_accepts(uint32_t field, const std::string& name) {
    switch (field) {
    case 106u: return name == "vcc_lo" || name == "vcc";
    case 107u: return name == "vcc_hi";
    case 118u: return name == "ttmp10";
    case 119u: return name == "ttmp11";
    case 124u: return name == "m0";
    case 126u: return name == "exec_lo" || name == "exec";
    case 127u: return name == "exec_hi";
    default: return false;
    }
}

bool operand_matches(const sc_operand& op, const op_token& token) {
    switch (op.file) {
    case sc_operand_file::sgpr: return token.plain && !token.vgpr && op.index == token.index;
    case sc_operand_file::vgpr: return token.plain && token.vgpr && op.index == token.index;
    case sc_operand_file::special:
        return token.special && special_field_accepts(op.special, token.special_name);
    case sc_operand_file::constant:
    case sc_operand_file::literal: return token.number && token.number_value == op.constant;
    case sc_operand_file::hwreg:
        return token.hwreg && op.hwreg_id == token.hwreg_id &&
               op.hwreg_offset == token.hwreg_offset && op.hwreg_size == token.hwreg_size;
    case sc_operand_file::param: return token.param && op.index == token.param_index;
    case sc_operand_file::attribute:
    case sc_operand_file::unclassified:
    default: return false;
    }
}

// A printed `key:NAME` attribute (SDWA dst_sel/dst_unused/srcN_sel).
std::string printed_attribute(const std::string& text, const char* key) {
    const std::string pattern = std::string(key) + ":";
    const size_t at = text.find(pattern);
    if (at == std::string::npos) {
        return std::string();
    }
    size_t i = at + pattern.size();
    size_t j = i;
    while (j < text.size()) {
        const char c = text[j];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            break;
        }
        ++j;
    }
    return text.substr(i, j - i);
}

// SDWA data-format selectors. Measured over all 7,772 `_sdwa` rows: the printed
// BYTE_0..3 / WORD_0/1 / DWORD names are the 3-bit values 0..6, and the
// dst_unused names are UNUSED_PAD/SEXT/PRESERVE = 0/1/2.
bool sdwa_selector_value(const std::string& name, uint8_t& value) {
    if (name == "BYTE_0") { value = 0; return true; }
    if (name == "BYTE_1") { value = 1; return true; }
    if (name == "BYTE_2") { value = 2; return true; }
    if (name == "BYTE_3") { value = 3; return true; }
    if (name == "WORD_0") { value = 4; return true; }
    if (name == "WORD_1") { value = 5; return true; }
    if (name == "DWORD") { value = 6; return true; }
    return false;
}

bool word_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

// -1 when the word is absent, else the printed 0/1 (a bare word counts as 1).
int printed_flag(const std::string& text, const char* word) {
    const std::string w = word;
    for (size_t at = text.find(w); at != std::string::npos; at = text.find(w, at + 1)) {
        const bool left = at == 0 || !word_char(text[at - 1]);
        const size_t end = at + w.size();
        const bool right = end >= text.size() || !word_char(text[end]);
        if (!left || !right) {
            continue;
        }
        if (end + 1 < text.size() && text[end] == ':' && (text[end + 1] == '0' || text[end + 1] == '1')) {
            return text[end + 1] == '1' ? 1 : 0;
        }
        return 1;
    }
    return -1;
}

// Values of a `key:[a,b,...]` list (DPP quad_perm / dpp8).
bool printed_list(const std::string& text, const char* key, std::vector<unsigned>& out) {
    const std::string k = key;
    size_t at = std::string::npos;
    for (size_t pos = text.find(k); pos != std::string::npos; pos = text.find(k, pos + 1)) {
        if (pos > 0 && word_char(text[pos - 1])) {
            continue;
        }
        const size_t after = pos + k.size();
        if (after < text.size() && text[after] == ':') {
            at = after + 1;
        } else if (after < text.size() && text[after] == '[') {
            at = after;
        }
        if (at != std::string::npos) {
            break;
        }
    }
    if (at == std::string::npos || at >= text.size() || text[at] != '[') {
        return false;
    }
    out.clear();
    unsigned value = 0;
    bool digits = false;
    for (size_t i = at + 1; i < text.size(); ++i) {
        const char c = text[i];
        if (c >= '0' && c <= '9') {
            value = value * 10 + static_cast<unsigned>(c - '0');
            digits = true;
            continue;
        }
        if (digits) {
            out.push_back(value);
            value = 0;
            digits = false;
        }
        if (c == ']') {
            break;
        }
    }
    if (digits) {
        out.push_back(value);
    }
    return true;
}

// A `key:0xN` value, or -1 when absent.
int printed_hex(const std::string& text, const char* key) {
    const std::string s = printed_attribute(text, key);
    if (s.size() < 3 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) {
        return -1;
    }
    return static_cast<int>(std::strtoul(s.c_str() + 2, nullptr, 16));
}

bool sdwa_unused_value(const std::string& name, uint8_t& value) {
    if (name == "UNUSED_PAD") { value = 0; return true; }
    if (name == "UNUSED_SEXT") { value = 1; return true; }
    if (name == "UNUSED_PRESERVE") { value = 2; return true; }
    return false;
}

op_token parse_operand_token(const std::string& piece) {
    op_token out;
    size_t begin = piece.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return out;
    }
    size_t end = piece.find_first_of(" \t", begin);
    std::string token = piece.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    // A number is an inline constant or a literal dword, never a register.
    if (token_is_number(token, out.number_is_real, out.number_value)) {
        out.number = true;
        return out;
    }
    if (token_is_special(token, out.special_name)) {
        out.special = true;
        return out;
    }
    // hwreg(<id>, <offset>, <size>): the token span runs past the first whitespace,
    // so it is located in the whole piece. HW_REG_TBA_HI is the one named hardware
    // register the fixtures print, and hwreg(HW_REG_TBA_HI, 7, 25) encodes 17.
    const size_t hw_begin = piece.find("hwreg(");
    if (hw_begin != std::string::npos) {
        const size_t hw_end = piece.find(')', hw_begin);
        const std::string inner =
            piece.substr(hw_begin + 6, hw_end == std::string::npos ? std::string::npos
                                                                  : hw_end - (hw_begin + 6));
        std::vector<std::string> fields;
        size_t start = 0;
        while (start <= inner.size()) {
            const size_t comma = inner.find(',', start);
            std::string f = inner.substr(start, comma == std::string::npos ? std::string::npos
                                                                          : comma - start);
            const size_t b = f.find_first_not_of(" \t");
            const size_t e = f.find_last_not_of(" \t");
            fields.push_back(b == std::string::npos ? std::string() : f.substr(b, e - b + 1));
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
        if (fields.size() == 3) {
            long id = -1;
            if (!fields[0].empty() && fields[0][0] >= '0' && fields[0][0] <= '9') {
                id = std::strtol(fields[0].c_str(), nullptr, 10);
            } else if (fields[0] == "HW_REG_TBA_HI") {
                id = 17;
            }
            if (id >= 0) {
                out.hwreg = true;
                out.hwreg_id = static_cast<uint32_t>(id);
                out.hwreg_offset = static_cast<uint32_t>(std::strtoul(fields[1].c_str(), nullptr, 10));
                out.hwreg_size = static_cast<uint32_t>(std::strtoul(fields[2].c_str(), nullptr, 10));
                return out;
            }
        }
    }
    if (token.size() >= 2 && token[0] == 'p' && token.size() <= 3) {
        bool digits = true;
        for (size_t i = 1; i < token.size(); ++i) {
            if (token[i] < '0' || token[i] > '9') {
                digits = false;
            }
        }
        if (digits) {
            out.param = true;
            out.param_index = static_cast<uint32_t>(std::strtoul(token.c_str() + 1, nullptr, 10));
            return out;
        }
    }
    if (token.size() >= 2 && token[0] == '-') {
        out.neg = true;
        token.erase(0, 1);
    }
    if (token.rfind("sext(", 0) == 0) {
        out.sext = true;
        token = token.substr(5);
        if (!token.empty() && token.back() == ')') {
            token.pop_back();
        }
    }
    if (token.size() >= 3 && token.front() == '|' && token.back() == '|') {
        out.abs = true;
        token = token.substr(1, token.size() - 2);
        // |0.5|: abs applied to an inline constant, still a number.
        if (token_is_number(token, out.number_is_real, out.number_value)) {
            out.number = true;
            return out;
        }
    }
    if (token.size() < 2) {
        return out;
    }
    const char head = token[0];
    if (head != 'v' && head != 's') {
        return out;
    }
    out.vgpr = head == 'v';
    if (token.find('[') != std::string::npos) {
        // v[2:3] / s[4:5]
        const size_t open = token.find('[');
        const size_t colon = token.find(':', open);
        const size_t close = token.find(']', open);
        if (open != 1 || colon == std::string::npos || close == std::string::npos) {
            return out;
        }
        out.pair = true;
        out.index = static_cast<uint32_t>(std::stoul(token.substr(open + 1, colon - open - 1)));
        out.plain = true;
        return out;
    }
    for (size_t i = 1; i < token.size(); ++i) {
        if (token[i] < '0' || token[i] > '9') {
            return out; // vcc_lo, exec_hi, flat_scratch, ... : not named by this module
        }
    }
    out.index = static_cast<uint32_t>(std::stoul(token.substr(1)));
    out.plain = true;
    return out;
}

struct kind_stat {
    uint32_t fixtures = 0;
    uint32_t format_checked = 0;
    uint32_t format_ok = 0;
    uint32_t length_ok = 0;
    uint32_t opcode_checked = 0;
    uint32_t operand_checks = 0;
    uint32_t operand_ok = 0;
    uint32_t selector_checks = 0; // SDWA dst_sel/dst_unused/srcN_sel/clamp/omod
    uint32_t selector_ok = 0;
    uint32_t dpp_checks = 0; // DPP control lanes/op/masks/fi/bound_ctrl/modifiers
    uint32_t dpp_ok = 0;
    uint32_t passed = 0;
};

struct format_stat {
    uint32_t fixtures = 0;
    uint32_t format_checked = 0;
    uint32_t format_ok = 0;
    uint32_t length_ok = 0;
    uint32_t operand_checks = 0;
    uint32_t operand_ok = 0;
    uint32_t selector_checks = 0;
    uint32_t selector_ok = 0;
    uint32_t dpp_checks = 0;
    uint32_t dpp_ok = 0;
    uint32_t passed = 0;
};

struct known_opcode {
    sc_isa_format format;
    const char* mnemonic;
    uint16_t opcode;
};

// Opcodes already pinned by hand in earlier phases (and in the Phase 5 harness).
// These are the only external opcode assertions: everything else is
// self-consistency (one opcode per mnemonic), not a claim about AMD's numbering.
const known_opcode kKnownOpcodes[] = {
    {sc_isa_format::vop1, "v_mov_b32", 1},
    {sc_isa_format::vop1, "v_cvt_f32_i32", 5},
    {sc_isa_format::sop1, "s_mov_b64", 4},
};

} // namespace

int main(int argc, char** argv) {
    const std::string fixture_path = (argc > 1) ? argv[1] : "tests/shader/fixtures/gfx10_mc_fixtures.tsv";
    const std::string report_dir = (argc > 2) ? argv[2] : "out/shader_tests";
    std::printf("ScarlettCompat shader pipeline - GFX10 decode conformance (synthetic LLVM MC fixtures)\n");
    std::printf("fixtures: %s\n", fixture_path.c_str());

    std::vector<fixture> fixtures;
    std::string load_error;
    const bool loaded = load_fixtures(fixture_path.c_str(), fixtures, load_error);
    CHECK(loaded);
    if (!loaded) {
        std::printf("   %s\n", load_error.c_str());
        std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
        return 1;
    }
    std::printf("   %zu fixtures\n", fixtures.size());

    std::map<std::string, kind_stat> by_kind;
    std::map<std::string, format_stat> by_format;
    std::map<std::string, int> opcode_of;                 // "format/mnemonic" -> opcode
    std::map<std::pair<std::string, int>, std::string> mnemonic_of_opcode;
    std::map<std::string, uint32_t> queue;                // reason -> count
    std::vector<std::string> failures;
    uint32_t known_opcode_checks = 0;

    // Per-(format, field) counts, so every asserted total printed below is the sum
    // of printed per-field lines rather than a hand-kept aggregate.
    std::map<std::string, std::pair<uint32_t, uint32_t>> mem_field_stats; // (asserted, excluded)
    std::map<std::string, uint32_t> mem_fmt_rows;                          // format -> fixture rows
    std::map<std::string, std::map<std::string, uint32_t>> mem_excl_reasons; // field -> reason -> count
    // Fields decoded from the instruction words when the mnemonic text carries no token
    // to compare against: asserted on the row's own merits, not excluded.
    std::map<std::string, uint32_t> mem_word_decoded;

    for (const auto& f : fixtures) {
        kind_stat& ks = by_kind[f.kind];
        ++ks.fixtures;

        const sc_isa_instruction first = sc_isa_decode_word(f.words[0]);
        const std::string fmt = format_name(first.format);
        format_stat& fs = by_format[fmt];
        ++fs.fixtures;

        bool row_ok = true;

        // ---- length ---------------------------------------------------------
        // sc_isa_length() counts the extra dword a literal / SDWA / DPP / imm32
        // form carries. A VOP3 literal selector sits in the second dword, which
        // the single-word decode cannot see, so the fixture supplies it.
        const uint32_t consumed = sc_isa_length(first, f.words.size() > 1 ? f.words[1] : 0u);
        const uint32_t encoded = static_cast<uint32_t>(f.words.size());
        // The decoder reports "cannot tell from these words" (MIMG: the 64-bit
        // base is followed by one dword per non-sequential address component, and
        // neither the nsa bit position nor the per-opcode address count is
        // measured here). Counted as a gap, not as a pass and not as a failure.
        const bool length_undetermined = consumed == 0;
        if (length_undetermined) {
            ++queue["length not determinable from the words (MIMG NSA)"];
        }
        bool length_ok = !length_undetermined && consumed == encoded;
        if (length_undetermined) {
            // no length assertion for this row
        } else if (length_ok) {
            ++ks.length_ok;
            ++fs.length_ok;
        } else {
            row_ok = false;
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s:%u  %s  length: encoded %u dwords, consumed %u",
                          f.source.c_str(), f.line, f.text.c_str(), encoded, consumed);
            failures.push_back(buf);
        }

        // ---- format ---------------------------------------------------------
        const std::vector<sc_isa_format> allowed = expected_formats(f.kind, f.text);
        if (!allowed.empty()) {
            ++ks.format_checked;
            ++fs.format_checked;
            bool ok = false;
            for (sc_isa_format a : allowed) {
                if (a == first.format) {
                    ok = true;
                    break;
                }
            }
            if (ok) {
                ++ks.format_ok;
                ++fs.format_ok;
            } else {
                row_ok = false;
                char buf[256];
                std::string expect;
                for (sc_isa_format a : allowed) {
                    expect += (expect.empty() ? "" : "|");
                    expect += format_name(a);
                }
                std::snprintf(buf, sizeof(buf), "%s:%u  %s  format: expected %s, got %s", f.source.c_str(),
                              f.line, f.text.c_str(), expect.c_str(), fmt.c_str());
                failures.push_back(buf);
            }
        }

        // ---- opcode ---------------------------------------------------------
        if (first.opcode_known) {
            ++ks.opcode_checked;
            const std::string mnemonic = base_mnemonic(f.text);
            const std::string key = fmt + "/" + mnemonic;
            const auto hit = opcode_of.find(key);
            if (hit == opcode_of.end()) {
                opcode_of[key] = first.opcode;
                mnemonic_of_opcode[std::make_pair(fmt, static_cast<int>(first.opcode))] = mnemonic;
            } else if (hit->second != first.opcode) {
                row_ok = false;
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "%s:%u  %s  opcode: %s decoded as %u here but %d for the same mnemonic",
                              f.source.c_str(), f.line, f.text.c_str(), mnemonic.c_str(), first.opcode,
                              hit->second);
                failures.push_back(buf);
            }
            for (const auto& known : kKnownOpcodes) {
                if (known.format == first.format && mnemonic == known.mnemonic) {
                    ++known_opcode_checks;
                    if (first.opcode != known.opcode) {
                        row_ok = false;
                        char buf[256];
                        std::snprintf(buf, sizeof(buf), "%s:%u  %s  opcode: expected %u, got %u",
                                      f.source.c_str(), f.line, f.text.c_str(), known.opcode, first.opcode);
                        failures.push_back(buf);
                    }
                }
            }
        } else {
            ++queue["opcode field not extracted for format " + fmt];
        }

        // ---- operands -------------------------------------------------------
        bool operands_decoded = first.format == sc_isa_format::vop1 || first.format == sc_isa_format::vop2 ||
                                first.format == sc_isa_format::vopc || first.format == sc_isa_format::sop1 ||
                                first.format == sc_isa_format::sop2 || first.format == sc_isa_format::sopc ||
                                first.format == sc_isa_format::sopk || first.format == sc_isa_format::sopp ||
                                first.format == sc_isa_format::vop3 ||
                                first.format == sc_isa_format::mubuf || first.format == sc_isa_format::mimg ||
                                first.format == sc_isa_format::smem || first.format == sc_isa_format::ds;
        if (!operands_decoded) {
            ++queue["operands not decoded for format " + fmt];
        } else if (first.words == 0) {
            ++queue["length unknown, operands not decoded for format " + fmt];
        } else {
            std::vector<uint8_t> bytes;
            bytes.reserve(f.words.size() * 4);
            for (uint32_t w : f.words) {
                for (int shift = 0; shift < 32; shift += 8) {
                    bytes.push_back(static_cast<uint8_t>((w >> shift) & 0xffu));
                }
            }
            const sc_isa_walk walk = sc_isa_walk_stream(bytes.data(), bytes.size());
            if (walk.instructions.empty()) {
                ++queue["walk produced no instruction for format " + fmt];
            } else {
                const sc_wave_decode decoded = sc_decode_wave(walk);
                if (decoded.instructions.empty()) {
                    ++queue["wave decode produced no operands for format " + fmt];
                } else {
                    const sc_instruction_operands& ops = decoded.instructions[0];

                    // Split the mnemonic text into operand tokens, keeping only the
                    // first whitespace-delimited token of each comma-separated piece
                    // (modifiers such as quad_perm:[...] follow the operand).
                    std::vector<std::string> pieces;
                    std::string rest = f.text;
                    const size_t first_space = rest.find_first_of(" \t");
                    if (first_space == std::string::npos) {
                        rest.clear();
                    } else {
                        rest = rest.substr(first_space + 1);
                    }
                    size_t start = 0;
                    int depth = 0;
                    for (size_t i = 0; i < rest.size(); ++i) {
                        const char c = rest[i];
                        if (c == '[' || c == '(') ++depth;
                        if (c == ']' || c == ')') --depth;
                        if (c == ',' && depth == 0) {
                            pieces.push_back(rest.substr(start, i - start));
                            start = i + 1;
                        }
                    }
                    if (start < rest.size()) {
                        pieces.push_back(rest.substr(start));
                    }

                    // Token positions depend on the printed operand list:
                    //   VOP2 carry forms print an sdst first
                    //     (v_add_co_ci_u32 v5, vcc_lo, v1, v2, vcc_lo)
                    //   VOPC cmpx forms print no destination at all (implicit vcc)
                    //     (v_cmpx_class_f16_e32 -1, v2)
                    // Both are visible in the fixture text, so they are derived
                    // from it rather than from a mnemonic allowlist.
                    const std::string mnemonic = base_mnemonic(f.text);
                    const bool cmpx = mnemonic.rfind("v_cmpx", 0) == 0;
                    bool check_dst = true;
                    bool check_src0 = true;
                    size_t dst_index = 0;
                    size_t src0_index = 1;
                    size_t src1_index = 2;
                    // VOP3 is decoded field-by-field, so its printed operand list
                    // is consumed in order: a decoded role with no token left is
                    // simply not checked (64-bit compares print two sources).
                    if (first.format == sc_isa_format::vop3) {
                        struct vop3_role {
                            bool present;
                            const sc_operand* operand;
                            const char* name;
                            int source_index; // 0..2 for sources, -1 otherwise
                        };
                        std::vector<vop3_role> ordered;
                        if (ops.has_vdst) {
                            ordered.push_back({true, &ops.vdst, "dst", -1});
                        }
                        if (ops.has_sdst && !(cmpx && pieces.size() <= 2)) {
                            ordered.push_back({true, &ops.sdst, "sdst", -1});
                        }
                        ordered.push_back({ops.has_src0, &ops.src0, "src0", 0});
                        ordered.push_back({ops.has_src1, &ops.src1, "src1", 1});
                        ordered.push_back({ops.has_src2, &ops.src2, "src2", 2});

                        auto is_attr_piece = [](const std::string& piece) {
                            const size_t b = piece.find_first_not_of(" \t");
                            return b != std::string::npos && piece.compare(b, 4, "attr") == 0;
                        };
                        auto attr_index = [](const std::string& piece) -> long {
                            const size_t b = piece.find("attr");
                            return b == std::string::npos ? -1 : std::strtol(piece.c_str() + b + 4, nullptr, 10);
                        };
                        auto report = [&](const char* name, const sc_operand& decoded, const std::string& expected) {
                            row_ok = false;
                            char buf[320];
                            std::snprintf(buf, sizeof(buf), "%s:%u  %s  operand %s: expected %s, got %s (field 0x%x)",
                                          f.source.c_str(), f.line, f.text.c_str(), name, expected.c_str(),
                                          sc_operand_name(decoded).c_str(), decoded.field);
                            failures.push_back(buf);
                        };
                        std::vector<bool> used(pieces.size(), false);

                        // Destinations are printed first, in order.
                        size_t dst_cursor = 0;
                        for (const auto& r : ordered) {
                            if (!r.present || r.source_index >= 0) {
                                continue;
                            }
                            if (dst_cursor >= pieces.size()) {
                                break;
                            }
                            const op_token token = parse_operand_token(pieces[dst_cursor]);
                            used[dst_cursor] = true;
                            ++dst_cursor;
                            if (token_unrecognized(token)) {
                                ++queue["operand token not a plain register (vcc/exec/literal/modifiers)"];
                                continue;
                            }
                            ++ks.operand_checks;
                            ++fs.operand_checks;
                            if (operand_matches(*r.operand, token)) {
                                ++ks.operand_ok;
                                ++fs.operand_ok;
                            } else {
                                report(r.name, *r.operand, pieces[dst_cursor - 1]);
                            }
                        }

                        // v_interp_* encode the attribute in the src0 slot but print it
                        // after the vector sources, so it is located by its text.
                        for (const auto& r : ordered) {
                            if (!r.present || r.source_index < 0) {
                                continue;
                            }
                            if (r.operand->file != sc_operand_file::attribute) {
                                continue;
                            }
                            size_t found = pieces.size();
                            for (size_t i = 0; i < pieces.size(); ++i) {
                                if (!used[i] && is_attr_piece(pieces[i])) {
                                    found = i;
                                    break;
                                }
                            }
                            if (found == pieces.size()) {
                                ++queue["interp form prints no attribute token"];
                                continue;
                            }
                            used[found] = true;
                            const long printed = attr_index(pieces[found]);
                            ++ks.operand_checks;
                            ++fs.operand_checks;
                            if (printed >= 0 && static_cast<uint32_t>(printed) == r.operand->index) {
                                ++ks.operand_ok;
                                ++fs.operand_ok;
                            } else {
                                report(r.name, *r.operand, "attr" + std::to_string(printed));
                            }
                        }

                        // Register sources consume the remaining pieces in order, with
                        // the per-source neg/abs bits checked against the printed token.
                        for (const auto& r : ordered) {
                            if (!r.present || r.source_index < 0) {
                                continue;
                            }
                            if (r.operand->file == sc_operand_file::attribute) {
                                continue;
                            }
                            size_t found = pieces.size();
                            for (size_t i = 0; i < pieces.size(); ++i) {
                                if (!used[i]) {
                                    found = i;
                                    break;
                                }
                            }
                            if (found == pieces.size()) {
                                break;
                            }
                            used[found] = true;
                            const op_token token = parse_operand_token(pieces[found]);
                            if (token_unrecognized(token)) {
                                ++queue["operand token not a plain register (vcc/exec/literal/modifiers)"];
                                continue;
                            }
                            ++ks.operand_checks;
                            ++fs.operand_checks;
                            bool ok = operand_matches(*r.operand, token);
                            const bool is_register = r.operand->file == sc_operand_file::sgpr ||
                                                     r.operand->file == sc_operand_file::vgpr;
                            if (ok && is_register && ops.modifiers.known) {
                                ok = token.neg == ops.modifiers.neg[r.source_index] &&
                                     token.abs == ops.modifiers.abs[r.source_index];
                            }
                            if (ok) {
                                ++ks.operand_ok;
                                ++fs.operand_ok;
                            } else {
                                report(r.name, *r.operand,
                                       (token.vgpr ? "v" : "s") + std::to_string(token.index) +
                                           (token.abs ? " abs" : "") + (token.neg ? " neg" : ""));
                            }
                        }

                        // Modifier bits that are not per-source.
                        if (ops.modifiers.known) {
                            const bool text_clamp = f.text.find(" clamp") != std::string::npos;
                            uint8_t text_omod = 0;
                            if (f.text.find("mul:2") != std::string::npos) text_omod = 1;
                            if (f.text.find("mul:4") != std::string::npos) text_omod = 2;
                            if (f.text.find("div:2") != std::string::npos) text_omod = 3;
                            if (text_clamp != ops.modifiers.clamp) {
                                row_ok = false;
                                char buf[256];
                                std::snprintf(buf, sizeof(buf), "%s:%u  %s  clamp: text=%d decoded=%d",
                                              f.source.c_str(), f.line, f.text.c_str(), text_clamp ? 1 : 0,
                                              ops.modifiers.clamp ? 1 : 0);
                                failures.push_back(buf);
                            }
                            if (text_omod != ops.modifiers.omod) {
                                row_ok = false;
                                char buf[256];
                                std::snprintf(buf, sizeof(buf), "%s:%u  %s  omod: text=%u decoded=%u",
                                              f.source.c_str(), f.line, f.text.c_str(), text_omod,
                                              ops.modifiers.omod);
                                failures.push_back(buf);
                            }
                            // The printed op_sel list names the slots that exist, in
                            // the order src0, src1, src2, dst (decoded bits 11,12,13,14).
                            // Shapes measured over all 105 op_sel fixtures: a full list
                            // is sources-then-dst (4 values for a 3-source form, 3 for a
                            // 2-source form, where bit 13 is skipped), and a short list
                            // is a plain prefix (v_permlane16_b32 prints 2 of 3 sources).
                            const size_t opsel = f.text.find("op_sel:[");
                            if (opsel != std::string::npos && ops.modifiers.op_sel_known) {
                                uint8_t printed[4] = {0, 0, 0, 0};
                                size_t printed_count = 0;
                                size_t cursor = opsel + 8;
                                while (cursor < f.text.size() && f.text[cursor] != ']' &&
                                       printed_count < 4) {
                                    while (cursor < f.text.size() &&
                                           (f.text[cursor] == ' ' || f.text[cursor] == ',')) {
                                        ++cursor;
                                    }
                                    if (cursor >= f.text.size() || f.text[cursor] == ']') {
                                        break;
                                    }
                                    printed[printed_count++] = static_cast<uint8_t>(f.text[cursor] - '0');
                                    ++cursor;
                                    while (cursor < f.text.size() && f.text[cursor] >= '0' &&
                                           f.text[cursor] <= '9') {
                                        ++cursor;
                                    }
                                }
                                const size_t n_src = pieces.size() > 1 ? pieces.size() - 1 : 0;
                                int slot[4] = {0, 1, 2, 3};
                                if (printed_count == n_src + 1 && n_src < 3) {
                                    slot[n_src] = 3; // sources, then the destination bit
                                }
                                for (size_t i = 0; i < printed_count; ++i) {
                                    if (printed[i] != ops.modifiers.op_sel[slot[i]]) {
                                        row_ok = false;
                                        char buf[256];
                                        std::snprintf(buf, sizeof(buf),
                                                      "%s:%u  %s  op_sel[%zu]: text=%u decoded=%u",
                                                      f.source.c_str(), f.line, f.text.c_str(), i, printed[i],
                                                      ops.modifiers.op_sel[slot[i]]);
                                        failures.push_back(buf);
                                    }
                                }
                            }
                        }
                                            }

                    if (first.format == sc_isa_format::sopk || first.format == sc_isa_format::sopp) {
                        // SOPP prints only its immediate. SOPK prints [dst, imm]
                        // except s_setreg_b32 (hwreg first) and s_setreg_imm32_b32
                        // (hwreg, imm32), which the text itself distinguishes; the
                        // hwreg word is a packed field this module does not decode.
                        struct pairing {
                            const sc_operand* operand;
                            size_t token;
                            const char* name;
                        };
                        std::vector<pairing> pairs;
                        if (first.format == sc_isa_format::sopp) {
                            if (ops.has_src0 && !pieces.empty()) {
                                pairs.push_back({&ops.src0, 0, "imm16"});
                            }
                        } else {
                            const bool hwreg_first =
                                !pieces.empty() && pieces[0].find("hwreg") != std::string::npos;
                            if (hwreg_first) {
                                if (ops.has_src0) pairs.push_back({&ops.src0, 0, "hwreg"});
                                if (ops.has_sdst && pieces.size() > 1) pairs.push_back({&ops.sdst, 1, "dst"});
                                if (ops.has_src1 && pieces.size() > 1) pairs.push_back({&ops.src1, 1, "imm32"});
                            } else if (pieces.size() == 1) {
                                if (ops.has_src0) pairs.push_back({&ops.src0, 0, "imm16"});
                            } else {
                                if (ops.has_sdst && !pieces.empty()) pairs.push_back({&ops.sdst, 0, "dst"});
                                if (ops.has_src0 && pieces.size() > 1) pairs.push_back({&ops.src0, 1, "imm16"});
                            }
                        }
                        for (const auto& r : pairs) {
                            const op_token token = parse_operand_token(pieces[r.token]);
                            if (token_unrecognized(token)) {
                                ++queue["operand token not a plain register (hwreg/packed fields)"];
                                continue;
                            }
                            ++ks.operand_checks;
                            ++fs.operand_checks;
                            if (operand_matches(*r.operand, token)) {
                                ++ks.operand_ok;
                                ++fs.operand_ok;
                            } else {
                                row_ok = false;
                                char buf[320];
                                std::snprintf(buf, sizeof(buf),
                                              "%s:%u  %s  operand %s: expected %s, got %s (field 0x%x)",
                                              f.source.c_str(), f.line, f.text.c_str(), r.name,
                                              pieces[r.token].c_str(),
                                              sc_operand_name(*r.operand).c_str(), r.operand->field);
                                failures.push_back(buf);
                            }
                        }
                    }
                    if (ops.dpp.known) {
                        // Every printed DPP attribute is compared with the decoded word.
                        auto check = [&](bool ok, const char* what, const std::string& printed,
                                         const std::string& decoded) {
                            ++ks.dpp_checks;
                            ++fs.dpp_checks;
                            if (ok) {
                                ++ks.dpp_ok;
                                ++fs.dpp_ok;
                                return;
                            }
                            row_ok = false;
                            char buf[320];
                            std::snprintf(buf, sizeof(buf), "%s:%u  %s  dpp %s: printed %s, decoded %s",
                                          f.source.c_str(), f.line, f.text.c_str(), what, printed.c_str(),
                                          decoded.c_str());
                            failures.push_back(buf);
                        };
                        const int fi = printed_flag(f.text, "fi");
                        if (ops.dpp.is_dpp8) {
                            std::vector<unsigned> lanes;
                            bool ok = printed_list(f.text, "dpp8", lanes) && lanes.size() == 8;
                            for (size_t i = 0; ok && i < 8; ++i) {
                                ok = lanes[i] == ops.dpp.dpp8_lane[i];
                            }
                            std::string lane_text, lane_decoded;
                            for (unsigned v : lanes) lane_text += (lane_text.empty() ? "" : ",") + std::to_string(v);
                            for (int i = 0; i < 8; ++i) {
                                lane_decoded += (i == 0 ? "" : ",") + std::to_string(ops.dpp.dpp8_lane[i]);
                            }
                            check(ok, "dpp8 lanes", lane_text, lane_decoded);
                            // An unprinted fi is the clear bit, not a missing check.
                            check((fi == 1) == ops.dpp.fi, "fi", std::to_string(fi),
                                  ops.dpp.fi ? "1" : "0");
                        } else {
                            std::vector<unsigned> lanes;
                            if (printed_list(f.text, "quad_perm", lanes)) {
                                bool ok = ops.dpp.quad && lanes.size() == 4;
                                for (size_t i = 0; ok && i < 4; ++i) {
                                    ok = lanes[i] == ops.dpp.quad_lane[i];
                                }
                                std::string lane_text, lane_decoded;
                                for (unsigned v : lanes) lane_text += (lane_text.empty() ? "" : ",") + std::to_string(v);
                                for (int i = 0; i < 4; ++i) {
                                    lane_decoded += (i == 0 ? "" : ",") + std::to_string(ops.dpp.quad_lane[i]);
                                }
                                check(ok, "quad_perm", lane_text, lane_decoded);
                            } else {
                                struct row_form {
                                    const char* key;
                                    uint8_t op;
                                    int forced_amount;
                                };
                                const row_form forms[] = {
                                    {"row_shl", 0, -1},   {"row_shr", 1, -1},  {"row_ror", 2, -1},
                                    {"row_share", 5, -1}, {"row_xmask", 6, -1}, {"row_mirror", 4, 0},
                                    {"row_half_mirror", 4, 1},
                                };
                                bool matched = false;
                                for (const auto& rf : forms) {
                                    bool present = false;
                                    unsigned amount = 0;
                                    if (rf.forced_amount >= 0) {
                                        present = printed_flag(f.text, rf.key) == 1;
                                        amount = static_cast<unsigned>(rf.forced_amount);
                                    } else {
                                        const std::string digits = printed_attribute(f.text, rf.key);
                                        present = !digits.empty();
                                        if (present) {
                                            amount = static_cast<unsigned>(std::strtoul(digits.c_str(), nullptr, 10));
                                        }
                                    }
                                    if (!present) {
                                        continue;
                                    }
                                    matched = true;
                                    const std::string row_decoded =
                                        (ops.dpp.quad ? std::string("quad") : std::to_string(ops.dpp.row_op)) +
                                        "/" + std::to_string(ops.dpp.row_amount);
                                    check(!ops.dpp.quad && ops.dpp.row_op == rf.op && ops.dpp.row_amount == amount,
                                          rf.key, std::to_string(amount), row_decoded);
                                }
                                if (!matched) {
                                    check(false, "control", "(none printed)", "row/quad");
                                }
                            }
                            const int row_mask = printed_hex(f.text, "row_mask");
                            const int bank_mask = printed_hex(f.text, "bank_mask");
                            // An omitted mask encodes 0xF in these fixtures: the
                            // disassembler prints a mask only when it differs from
                            // all-enabled (`quad_perm:[3,2,1,0] bank_mask:0x0` carries
                            // row_mask = 15, and the mirror fixture carries bank 15).
                            check(ops.dpp.masks_known && (row_mask < 0 ? 0xF : row_mask) == ops.dpp.row_mask,
                                  "row_mask", row_mask < 0 ? std::string("(default 15)") : std::to_string(row_mask),
                                  std::to_string(ops.dpp.row_mask));
                            check(ops.dpp.masks_known && (bank_mask < 0 ? 0xF : bank_mask) == ops.dpp.bank_mask,
                                  "bank_mask", bank_mask < 0 ? std::string("(default 15)") : std::to_string(bank_mask),
                                  std::to_string(ops.dpp.bank_mask));
                            check((fi == 1) == ops.dpp.fi, "fi", std::to_string(fi),
                                  ops.dpp.fi ? "1" : "0");
                            // The fixtures set this bit for both `bound_ctrl:0` and
                            // `bound_ctrl:1`, so only its presence is asserted.
                            const int bound = printed_flag(f.text, "bound_ctrl");
                            check((bound >= 0) == ops.dpp.bound_ctrl, "bound_ctrl present",
                                  std::to_string(bound), ops.dpp.bound_ctrl ? "1" : "0");
                        }
                    }
                    if (ops.sdwa.known) {
                        // dst_sel/dst_unused/clamp/omod only exist where the form has a
                        // vector destination; a compare form puts its scalar destination
                        // in those bits instead, so a printed selector there is a defect.
                        struct selector_case {
                            const char* key;
                            uint8_t decoded;
                            bool is_unused;
                            bool applicable;
                        };
                        const bool two_sources = first.format == sc_isa_format::vop2 ||
                                                 first.format == sc_isa_format::vopc;
                        const bool dst_selectors = ops.sdwa.dst_selector_known;
                        const selector_case selectors[] = {
                            {"dst_sel", ops.sdwa.dst_sel, false, dst_selectors},
                            {"dst_unused", ops.sdwa.dst_unused, true, dst_selectors},
                            {"src0_sel", ops.sdwa.src0_sel, false, true},
                            {"src1_sel", ops.sdwa.src1_sel, false, two_sources},
                        };
                        for (const auto& c : selectors) {
                            const std::string printed = printed_attribute(f.text, c.key);
                            if (!c.applicable) {
                                if (!printed.empty()) {
                                    row_ok = false;
                                    char buf[256];
                                    std::snprintf(buf, sizeof(buf),
                                                  "%s:%u  %s  prints %s on a form with no vector destination",
                                                  f.source.c_str(), f.line, f.text.c_str(), c.key);
                                    failures.push_back(buf);
                                }
                                continue;
                            }
                            ++ks.selector_checks;
                            ++fs.selector_checks;
                            uint8_t value = 0;
                            const bool mapped = c.is_unused ? sdwa_unused_value(printed, value)
                                                           : sdwa_selector_value(printed, value);
                            if (!printed.empty() && mapped && value == c.decoded) {
                                ++ks.selector_ok;
                                ++fs.selector_ok;
                            } else {
                                row_ok = false;
                                char buf[320];
                                std::snprintf(buf, sizeof(buf),
                                              "%s:%u  %s  %s: printed %s, decoded %u",
                                              f.source.c_str(), f.line, f.text.c_str(), c.key,
                                              printed.empty() ? "-" : printed.c_str(), c.decoded);
                                failures.push_back(buf);
                            }
                        }
                        if (dst_selectors) {
                            const bool text_clamp = f.text.find(" clamp") != std::string::npos;
                            uint8_t text_omod = 0;
                            if (f.text.find("mul:2") != std::string::npos) text_omod = 1;
                            if (f.text.find("mul:4") != std::string::npos) text_omod = 2;
                            if (f.text.find("div:2") != std::string::npos) text_omod = 3;
                            ++ks.selector_checks;
                            ++fs.selector_checks;
                            if (text_clamp == ops.sdwa.clamp && text_omod == ops.sdwa.omod) {
                                ++ks.selector_ok;
                                ++fs.selector_ok;
                            } else {
                                row_ok = false;
                                char buf[256];
                                std::snprintf(buf, sizeof(buf),
                                              "%s:%u  %s  sdwa clamp/omod: printed %d/%u, decoded %d/%u",
                                              f.source.c_str(), f.line, f.text.c_str(), text_clamp ? 1 : 0,
                                              text_omod, ops.sdwa.clamp ? 1 : 0, ops.sdwa.omod);
                                failures.push_back(buf);
                            }
                        }
                    }
                    if (first.format != sc_isa_format::vop3 && first.format != sc_isa_format::sopk &&
                        first.format != sc_isa_format::sopp) {
                    // A VOP2 carry form prints an sdst second (v_add_co_ci_u32 v5,
                    // vcc_lo, v1, v2, vcc_lo) while v_cndmask_b32 prints only its
                    // implicit vcc at the end (v_cndmask_b32_e32 v5, 0, v2, vcc).
                    // The sdst token is a named scalar (vcc/vcc_lo) or a plain s
                    // register; a literal vector source such as `0` is not.
                    auto is_scalar_token = [](const std::string& piece) {
                        const op_token token = parse_operand_token(piece);
                        if (token.plain && !token.vgpr) {
                            return true;
                        }
                        const size_t begin = piece.find_first_not_of(" \t");
                        if (begin == std::string::npos) {
                            return false;
                        }
                        const size_t end = piece.find_first_of(" \t", begin);
                        const std::string tok = piece.substr(begin, end == std::string::npos
                                                                       ? std::string::npos
                                                                       : end - begin);
                        return tok == "vcc" || tok == "vcc_lo" || tok == "vcc_hi";
                    };
                    // A literal form also has four pieces with a scalar in
                    // second place (v_madak_f32 v0, s0, v1, 0x4229ae14: src0 is
                    // s0, the literal is last), but no carry form ends in an
                    // immediate, so the last piece decides.
                    bool last_is_number = false;
                    if (pieces.size() > 3) {
                        const size_t last_begin = pieces[3].find_first_not_of(" \t");
                        last_is_number = last_begin != std::string::npos &&
                                         (pieces[3][last_begin] >= '0' && pieces[3][last_begin] <= '9');
                    }
                    const bool sdst_inserted =
                        pieces.size() >= 5 ||
                        (pieces.size() == 4 && is_scalar_token(pieces[1]) && !last_is_number);
                    if (first.format == sc_isa_format::vop2 && sdst_inserted) {
                        src0_index = 2;
                        src1_index = 3;
                    } else if (first.format == sc_isa_format::vopc) {
                        // VOPC has no encoded destination: the result goes to vcc.
                        // cmp prints it, cmpx does not, so the sources shift.
                        src0_index = (cmpx && pieces.size() == 2) ? 0 : 1;
                        src1_index = src0_index + 1;
                    } else if (first.format == sc_isa_format::sopc) {
                        // SOPC prints no destination: it writes SCC.
                        check_dst = false;
                        src0_index = 0;
                        src1_index = 1;
                    } else if (first.format == sc_isa_format::sop1 && pieces.size() == 1) {
                        // The single-operand SOP1 forms disagree on the role: s_getpc_b64
                        // writes sdst, while s_setpc_b64 / s_rfe_b64 / s_swappc_b64
                        // read ssrc0. The fixtures print no role, so this small set is
                        // named explicitly instead of accepting whichever operand matches.
                        if (mnemonic == "s_getpc_b64") {
                            check_src0 = false; // writes sdst: piece[0] is the destination
                        } else {
                            check_dst = false;
                            src0_index = 0;
                        }
                    }

                    struct role {
                        bool present;
                        const sc_operand* operand;
                        size_t token_index;
                        const char* name;
                        int source_index = -1; // 0/1 for SDWA source modifiers
                    };
                    std::vector<role> roles;
                    // Memory formats: measured fields vs the printed role.
                    if (first.format == sc_isa_format::mubuf || first.format == sc_isa_format::mimg) {
                        roles.push_back({ops.has_mem_vdst, &ops.mem_vdst, 0, "mvdst"});
                        // MIMG prints a ttmp[] vector as srsrc on 3 fixture rows (TTMP
                        // resource forms); the field's TTMP encoding is not measured, so
                        // the srsrc assertion covers the plain s[] tokens only.
                        if (f.text.find("ttmp[") == std::string::npos) {
                            roles.push_back({ops.has_mem_srsrc, &ops.mem_srsrc, 2, "msrsrc"});
                        } else {
                            ++mem_field_stats["mimg msrsrc"].second; // TTMP resource form
            ++mem_excl_reasons["mimg msrsrc"]["ttmp[] resource form: the 52:48 field names no s[] base"];
                        }
                    }
                    if (first.format == sc_isa_format::mimg) {
                        if (pieces.size() > 1 && parse_operand_token(pieces[1]).plain) {
                            roles.push_back({ops.has_mem_vaddr, &ops.mem_vaddr, 1, "mvaddr"});
                        } else {
                            ++mem_field_stats["mimg mvaddr"].second; // NSA list or no token
                            ++mem_excl_reasons["mimg mvaddr"]["NSA address list: components live in extra dwords"];
                        }
                    }
                    // sdata exists only in the three-operand forms: s_dcache_discard /
                    // s_dcache_discard_x2 print (sbase, soffset) and have no data slot,
                    // so their first register is not sdata.
                    if (first.format == sc_isa_format::smem) {
                        if (pieces.size() > 2 && parse_operand_token(pieces[0]).plain) {
                            roles.push_back({ops.has_mem_sdata, &ops.mem_sdata, 0, "msdata"});
                        } else {
                            ++mem_field_stats["smem msdata"].second; // s_dcache_discard: no data slot
                            ++mem_excl_reasons["smem msdata"]["two-operand s_dcache_discard: no data slot"];
                        }
                    }
                    // The plain `offset:N` form only: ds_read2_*/ds_write2_* print
                    // `offset0:`/`offset1:` and pack two 8-bit offsets into the same
                    // bits 15:0, a different role that is not claimed.
                    const bool ds_offset_shape = first.format == sc_isa_format::ds &&
                                                 f.text.find("offset0:") == std::string::npos &&
                                                 f.text.find("offset1:") == std::string::npos &&
                                                 f.text.find("offset:swizzle(") == std::string::npos;
                    if (first.format == sc_isa_format::ds && !ds_offset_shape) {
                        ++mem_field_stats["ds moffset"].second; // dual 8-bit packing or swizzle word
                        ++mem_excl_reasons["ds moffset"]["offset0:/offset1: or offset:swizzle() word"];
                    }
                    if (ds_offset_shape) {
                        uint32_t printed_offset = 0;
                        const size_t at = f.text.find("offset:");
                        if (at != std::string::npos) {
                            size_t cursor = at + 7;
                            int base = 10;
                            if (cursor + 1 < f.text.size() && f.text[cursor] == 0x30 &&
                                (f.text[cursor + 1] == 0x78 || f.text[cursor + 1] == 0x58)) {
                                cursor += 2;
                                base = 16;
                            }
                            while (cursor < f.text.size()) {
                                const char c = f.text[cursor];
                                uint32_t digit = 0;
                                if (c >= 0x30 && c <= 0x39) digit = static_cast<uint32_t>(c - 0x30);
                                else if (base == 16 && c >= 0x61 && c <= 0x66) digit = static_cast<uint32_t>(c - 0x61 + 10);
                                else if (base == 16 && c >= 0x41 && c <= 0x46) digit = static_cast<uint32_t>(c - 0x41 + 10);
                                else break;
                                printed_offset = printed_offset * static_cast<uint32_t>(base) + digit;
                                ++cursor;
                            }
                        }
                        ++mem_field_stats["ds moffset"].first;
                        ++ks.operand_checks;
                        ++fs.operand_checks;
                        if (ops.has_mem_offset && ops.mem_offset == printed_offset) {
                            ++ks.operand_ok;
                            ++fs.operand_ok;
                        } else {
                            row_ok = false;
                            char buf[256];
                            std::snprintf(buf, sizeof(buf), "%s:%u  %s  operand moffset: expected %u, field %u",
                                          f.source.c_str(), f.line, f.text.c_str(), printed_offset,
                                          ops.mem_offset);
                            failures.push_back(buf);
                        }
                    }
                    // v_madmk/v_madak/v_fmamk/v_fmaak print [dst, src0, imm32, src1]:
                    // the literal is the second source and the 8-bit field is the
                    // third, so the two must not share a token index.
                    size_t vsrc1_index = src1_index;
                    if (first.format == sc_isa_format::vop2 && ops.has_src1 &&
                        first.extra == sc_isa_extra::literal) {
                        // v_madmk/v_fmamk print [dst, src0, imm32, src1] while
                        // v_madak/v_fmaak print [dst, src0, src1, imm32], so the
                        // literal's position is read from the text.
                        // Start at index 2: src0 (index 1) may itself be an inline
                        // constant (`v_madmk_f32 v5, 0, 0x11213141, v3`).
                        for (size_t i = 2; i < pieces.size(); ++i) {
                            const op_token probe = parse_operand_token(pieces[i]);
                            if (probe.number) {
                                src1_index = i;
                                vsrc1_index = (i == 2) ? 3 : 2;
                                break;
                            }
                        }
                    }
                    // A compare SDWA form does encode its scalar destination (dword1
                    // bits 15:8) and prints it first, while the plain e32 compares
                    // always write vcc and encode no destination at all.
                    const sc_operand* dst_operand = nullptr;
                    if (ops.has_vdst && check_dst) {
                        dst_operand = &ops.vdst;
                    } else if (first.format == sc_isa_format::vopc && ops.has_sdst && !cmpx) {
                        dst_operand = &ops.sdst;
                    }
                    roles.push_back({dst_operand != nullptr, dst_operand, dst_index, "dst", -1});
                    roles.push_back({ops.has_src0 && check_src0, &ops.src0, src0_index, "src0", 0});
                    roles.push_back({ops.has_vsrc1, &ops.vsrc1, vsrc1_index, "src1", 1});
                    // SOP2/SOPC and VOP3 report their second source in `src1`
                    // (the scalar 8-bit field for SOP2/SOPC, the 9-bit field for
                    // VOP3); the VOP3 branch above handles its own ordering.
                    roles.push_back({ops.has_src1, &ops.src1, src1_index, "src1", 1});

                    const bool mem_format = first.format == sc_isa_format::mubuf ||
                                            first.format == sc_isa_format::mimg ||
                                            first.format == sc_isa_format::smem ||
                                            first.format == sc_isa_format::ds;
                    // Only the memory fields are accounted per field here; the generic
                    // dst/src0/src1 roles are vector-ALU roles that memory rows push but
                    // never assert.
                    const auto is_mem_role = [](const char* name) {
                        return std::string(name) == "mvdst" || std::string(name) == "msrsrc" ||
                               std::string(name) == "mvaddr" || std::string(name) == "msdata" ||
                               std::string(name) == "moffset";
                    };
                    for (const auto& r : roles) {
                        if (!r.present) {
                            if (mem_format && is_mem_role(r.name)) {
                                const std::string reason =
                                    "stream truncated before the operand word at " + f.source + ":" +
                                    std::to_string(f.line);
                                ++mem_field_stats[fmt + " " + r.name].second;
                                ++mem_excl_reasons[fmt + " " + r.name][reason];
                            }
                            continue;
                        }
                        if (r.token_index >= pieces.size()) {
                            if (mem_format && is_mem_role(r.name)) {
                                // The decoder produced the field from the words; the printed text
                                // just has no token to compare. For MIMG the operand word is always
                                // present, so the field is asserted on its word value. MUBUF's
                                // buffer_gl0_inv/buffer_gl1_inv take no operands at all and their
                                // operand word is 0x00000000 (reserved), so those rows cannot be
                                // asserted and are excluded with that word evidence.
                                if (first.format == sc_isa_format::mimg) {
                                    ++mem_field_stats[fmt + " " + r.name].first;
                                    ++mem_word_decoded[fmt + " " + r.name];
                                } else {
                                    const std::string reason =
                                        "no operands on this mnemonic; operand word is 0x00000000 "
                                        "(reserved), so the field names no register";
                                    ++mem_field_stats[fmt + " " + r.name].second;
                                    ++mem_excl_reasons[fmt + " " + r.name][reason];
                                }
                            } else {
                                ++queue[std::string("mnemonic text has no ") + r.name +
                                        " token for format " + fmt];
                            }
                            continue;
                        }
                        const op_token token = parse_operand_token(pieces[r.token_index]);
                        if (token_unrecognized(token)) {
                            ++queue["operand token not decoded (modifier/selector/packed field)"];
                            continue;
                        }
                        if (first.format == sc_isa_format::mubuf || first.format == sc_isa_format::mimg ||
                            first.format == sc_isa_format::smem || first.format == sc_isa_format::ds) {
                            ++mem_field_stats[fmt + " " + r.name].first;
                        }
                        ++ks.operand_checks;
                        ++fs.operand_checks;
                        bool ok = operand_matches(*r.operand, token);
                        if (ok && ops.sdwa.known && r.source_index >= 0) {
                            const int s = r.source_index;
                            ok = token.sext == ops.sdwa.sext[s] && token.neg == ops.sdwa.neg[s] &&
                                 token.abs == ops.sdwa.abs[s];
                        }
                        if (ops.dpp.modifiers_known && r.source_index >= 0 && r.source_index < 2) {
                            // DPP16 keeps the per-source neg/abs bits in dword1 20..23.
                            const int s = r.source_index;
                            ++ks.dpp_checks;
                            ++fs.dpp_checks;
                            if (token.neg == ops.dpp.neg[s] && token.abs == ops.dpp.abs[s]) {
                                ++ks.dpp_ok;
                                ++fs.dpp_ok;
                            } else {
                                row_ok = false;
                                char buf[256];
                                std::snprintf(buf, sizeof(buf), "%s:%u  %s  dpp %s modifiers: printed neg=%d abs=%d, decoded neg=%d abs=%d",
                                              f.source.c_str(), f.line, f.text.c_str(), r.name, token.neg ? 1 : 0,
                                              token.abs ? 1 : 0, ops.dpp.neg[s] ? 1 : 0, ops.dpp.abs[s] ? 1 : 0);
                                failures.push_back(buf);
                            }
                        }
                        if (ok) {
                            ++ks.operand_ok;
                            ++fs.operand_ok;
                        } else {
                            row_ok = false;
                            char buf[320];
                            std::snprintf(buf, sizeof(buf),
                                          "%s:%u  %s  operand %s: expected %s, got %s (field 0x%x)",
                                          f.source.c_str(), f.line, f.text.c_str(), r.name,
                                          pieces[r.token_index].c_str(),
                                          sc_operand_name(*r.operand).c_str(), r.operand->field);
                            failures.push_back(buf);
                        }
                    }
                    }
                }
            }
        }

        mem_fmt_rows[fmt] = fs.fixtures;
        if (row_ok) {
            ++ks.passed;
            ++fs.passed;
        }
    }

    // ------------------------------------------------------- resolved opcodes
    banner("opcode table resolved from the fixtures (observed, not AMD numbering except the pinned three)");
    std::printf("   %zu (format, mnemonic) pairs resolved\n", opcode_of.size());
    std::printf("   %zu known-opcode assertions applied\n", static_cast<size_t>(known_opcode_checks));

    // ------------------------------------- MIMG dmask and NSA address components
    // Both are decoded from the instruction words through sc_decode_instruction
    // (explicit words and count, no walker), then compared with the fixture text.
    // Measured on every MIMG row: dmask 11:8 of dword 0 matches 247/247 printed
    // dmask values, and the NSA list matches 100/100 rows (component 0 in dword 1
    // bits 7:0, then four 8-bit vgprs per extra dword), with 0 violations.
    {
        uint32_t mimg_rows = 0;
        for (const auto& f : fixtures) {
            if (f.words.size() < 2 || ((f.words[0] >> 26) & 0x3fu) != 0x3cu) {
                continue;
            }
            const sc_instruction_operands io =
                sc_decode_instruction(f.words.data(), static_cast<uint32_t>(f.words.size()));
            if (io.format != sc_isa_format::mimg) {
                continue;
            }
            ++mimg_rows;

            // dmask
            const std::size_t dp = f.text.find("dmask:0x");
            if (dp != std::string::npos) {
                const uint32_t printed =
                    static_cast<uint32_t>(std::strtoul(f.text.c_str() + dp + 8, nullptr, 16));
                if (printed != io.mem_dmask) {
                    ++mem_field_stats["mimg mdmask"].second;
                    char buf[320];
                    std::snprintf(buf, sizeof buf,
                                  "MISMATCH %s:%u dmask: text says 0x%x, word0 bits 11:8 say 0x%x",
                                  f.source.c_str(), f.line, printed, io.mem_dmask);
                    failures.push_back(buf);
                } else {
                    ++mem_field_stats["mimg mdmask"].first;
                }
            } else if (f.text.empty()) {
                ++mem_field_stats["mimg mdmask"].second;
                ++mem_excl_reasons["mimg mdmask"]
                                   ["no printed text: the row has words only, so the 11:8 "
                                    "slot cannot be checked against a dmask:0x token"];
            } else {
                ++mem_field_stats["mimg mdmask"].second;
                ++mem_excl_reasons["mimg mdmask"]
                                   ["mnemonic prints no dmask operand "
                                    "(image_bvh*_intersect_ray); the 11:8 slot is not a dmask there"];
            }

            // NSA address list: "[v.., v.., ...]" is printed only when dim 2:1 != 0.
            const std::size_t lp = f.text.find("[v");
            if (lp != std::string::npos) {
                const std::size_t rp = f.text.find(']', lp);
                std::vector<uint32_t> comps;
                for (std::size_t i = lp; i < rp && i + 1 < f.text.size(); ++i) {
                    if (f.text[i] == 'v' && f.text[i + 1] >= '0' && f.text[i + 1] <= '9') {
                        comps.push_back(static_cast<uint32_t>(
                            std::strtoul(f.text.c_str() + i + 1, nullptr, 10)));
                    }
                }
                bool ok = io.has_mem_nsa && comps.size() <= io.mem_nsa_slots;
                for (std::size_t j = 0; ok && j < comps.size(); ++j) {
                    ok = comps[j] == io.mem_nsa_vgpr[j];
                }
                if (ok) {
                    ++mem_field_stats["mimg mnsa"].first;
                } else {
                    ++mem_field_stats["mimg mnsa"].second;
                    char buf[400];
                    std::snprintf(buf, sizeof buf,
                                  "MISMATCH %s:%u mnsa: printed %u components from %s, words give "
                                  "%u slots (dim bits 2:1)",
                                  f.source.c_str(), f.line, static_cast<uint32_t>(comps.size()),
                                  f.text.c_str(), io.mem_nsa_slots);
                    failures.push_back(buf);
                }
            } else if (f.text.empty()) {
                ++mem_field_stats["mimg mnsa"].second;
                ++mem_excl_reasons["mimg mnsa"]
                                   ["no printed text: dim 2:1 gives the extra-dword count but the "
                                    "component count exists only in the printed list"];
            } else {
                ++mem_field_stats["mimg mnsa"].second;
                ++mem_excl_reasons["mimg mnsa"]
                                   ["contiguous address: dim 2:1 is 0, so no NSA list is encoded "
                                    "and the one v[dword] is the whole address"];
            }
        }
        // ssamp: the sampler is the second s[] token (resource first). Measured over
        // every row that prints one: dword1 25:21 in 4-SGPR units, 103/103 exact;
        // 20:24 and 22:27 disagree on all 103 rows, so both boundaries are pinned.
        // Rows whose mnemonic takes no sampler (loads, stores, atomics, bvh) have no
        // second s[] token and are excluded with that reason.
        {
            uint32_t ssamp_rows = 0;
            for (const auto& f : fixtures) {
                if (f.words.size() < 2 || ((f.words[0] >> 26) & 0x3fu) != 0x3cu) {
                    continue;
                }
                const sc_instruction_operands io =
                    sc_decode_instruction(f.words.data(), static_cast<uint32_t>(f.words.size()));
                if (io.format != sc_isa_format::mimg) {
                    continue;
                }
                ++ssamp_rows;
                const std::size_t s1 = f.text.find("s[");
                const std::size_t s2 = s1 == std::string::npos ? std::string::npos
                                                              : f.text.find("s[", s1 + 2);
                if (s2 != std::string::npos) {
                    const uint32_t printed =
                        static_cast<uint32_t>(std::strtoul(f.text.c_str() + s2 + 2, nullptr, 10));
                    if (printed != io.mem_ssamp) {
                        ++mem_field_stats["mimg mssamp"].second;
                        char buf[320];
                        std::snprintf(buf, sizeof buf,
                                      "MISMATCH %s:%u mssamp: text says s%u, word1 bits 25:21 * 4 "
                                      "says s%u",
                                      f.source.c_str(), f.line, printed, io.mem_ssamp);
                        failures.push_back(buf);
                    } else {
                        ++mem_field_stats["mimg mssamp"].first;
                    }
                } else {
                    ++mem_field_stats["mimg mssamp"].second;
                    ++mem_excl_reasons["mimg mssamp"]
                                       ["mnemonic prints one s[] resource and no sampler operand"];
                }
            }
            if (ssamp_rows != mimg_rows) {
                char buf[256];
                std::snprintf(buf, sizeof buf,
                              "MISMATCH mimg ssamp pass saw %u MIMG rows, dmask/NSA pass saw %u",
                              ssamp_rows, mimg_rows);
                failures.push_back(buf);
            }
        }

        if (mimg_rows != mem_fmt_rows["mimg"]) {
            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "MISMATCH mimg dmask/NSA pass saw %u MIMG rows, per-field row count is %u",
                          mimg_rows, mem_fmt_rows["mimg"]);
            failures.push_back(buf);
        }
    }

    // ------------------------------------- FLAT/GLOBAL/SCRATCH operands (dword 1 bytes)
    // Measured over the 153 fixture rows whose word0 format field is 0x37. Dword 1
    // holds four 8-bit register slots: vaddr 39:32 (byte 0), vdata 47:40 (byte 1),
    // saddr 55:48 (byte 2), vdst 63:56 (byte 3). Shapes with a serviceable print:
    //   64 rows (v,v) not a load       -> vaddr, vdata
    //    8 rows (v,v) load            -> vdst, vaddr
    //   42 rows (v,v,off) not a load  -> vaddr, vdata, saddr off (byte 2 = 0x7D)
    //    4 rows (v,v,v,off)           -> vdst, vaddr, vdata, saddr off
    //    7 rows (v,s) load            -> saddr
    //    5 rows (off,off)             -> saddr off (byte 2 = 0x7F)
    // The four-operand rows pin all four bytes at once: `global_atomic_fcmpswap v5,
    // v[1:2], v[2:3], off` is dword1 0x057D0201 (byte3 = 5 = v5, byte0 = 1, byte1 = 2,
    // byte2 = 0x7D = off). Remaining shapes (`off` where the omitted register is not
    // identifiable, a lone v with saddr, or shapes whose printed values coincide) are
    // excluded per field with a word-level reason rather than guessed.
    {
        auto split_ops = [](const std::string& text) {
            std::vector<std::string> ops;
            std::size_t start = 0;
            int depth = 0;
            for (std::size_t i = 0; i <= text.size(); ++i) {
                const char c = i < text.size() ? text[i] : ',';
                if (c == '[' || c == '(') ++depth;
                else if (c == ']' || c == ')') --depth;
                else if (c == ',' && depth == 0) {
                    std::size_t b = start;
                    while (b < i && (text[b] == ' ' || text[b] == '\t')) ++b;
                    std::size_t e = i;
                    while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\t')) --e;
                    if (e > b) {
                        std::string op = text.substr(b, e - b);
                        const std::size_t wind = op.find_first_of(" \t");
                        if (wind != std::string::npos) op.erase(wind); // drop offset:/glc/slc
                        ops.push_back(op);
                    }
                    start = i + 1;
                }
            }
            return ops;
        };
        auto reg = [](const std::string& op, char kind, uint32_t& out) {
            if (op.size() < 2 || op[0] != kind) return false;
            const char* digits = nullptr;
            if (op[1] == '[') digits = op.c_str() + 2;
            else if (op[1] >= '0' && op[1] <= '9') digits = op.c_str() + 1;
            if (digits == nullptr) return false;
            out = static_cast<uint32_t>(std::strtoul(digits, nullptr, 10));
            return true;
        };
        uint32_t flat_rows = 0;
        for (const auto& f : fixtures) {
            if (f.words.size() < 2 || ((f.words[0] >> 26) & 0x3fu) != 0x37u) {
                continue;
            }
            const sc_instruction_operands io =
                sc_decode_instruction(f.words.data(), static_cast<uint32_t>(f.words.size()));
            if (io.format != sc_isa_format::flat || !io.has_mem_flat) {
                continue;
            }
            ++flat_rows;
            bool done[4] = {false, false, false, false}; // mvdst, mvaddr, mvdata, msaddr
            auto check = [&](int slot, const char* field, uint32_t printed, uint32_t decoded,
                             const char* where) {
                done[slot] = true;
                const std::string key = std::string("flat ") + field;
                if (printed == decoded) {
                    ++mem_field_stats[key].first;
                    return;
                }
                ++mem_field_stats[key].second;
                char buf[320];
                std::snprintf(buf, sizeof buf,
                              "MISMATCH %s:%u %s: text says %u, dword1 %s says %u",
                              f.source.c_str(), f.line, field, printed, where, decoded);
                failures.push_back(buf);
            };
            auto check_off = [&]() {
                done[3] = true;
                if (io.mem_flat_saddr_off) {
                    ++mem_field_stats["flat msaddr"].first;
                    return;
                }
                ++mem_field_stats["flat msaddr"].second;
                char buf[320];
                std::snprintf(buf, sizeof buf,
                              "MISMATCH %s:%u msaddr: text says off, dword1 byte 2 says 0x%02x",
                              f.source.c_str(), f.line, io.mem_flat_saddr);
                failures.push_back(buf);
            };
            const std::size_t sp = f.text.find(' ');
            const std::string body = sp == std::string::npos ? std::string() : f.text.substr(sp + 1);
            const std::vector<std::string> ops = split_ops(body);
            const bool is_load = f.text.find("_load") != std::string::npos;
            uint32_t a = 0, b = 0, c = 0;
            const bool v0 = ops.size() > 0 && reg(ops[0], 'v', a);
            const bool v1 = ops.size() > 1 && reg(ops[1], 'v', b);
            const bool v2 = ops.size() > 2 && reg(ops[2], 'v', c);
            const bool op0_off = ops.size() > 0 && ops[0] == "off";
            const bool op1_off = ops.size() > 1 && ops[1] == "off";
            const bool op_last_off = !ops.empty() && ops.back() == "off";
            if (ops.size() == 2 && v0 && v1) {
                if (is_load) {
                    check(0, "mvdst", a, io.mem_flat_vdst, "byte 3 (63:56)");
                    check(1, "mvaddr", b, io.mem_flat_vaddr, "byte 0 (39:32)");
                } else {
                    check(1, "mvaddr", a, io.mem_flat_vaddr, "byte 0 (39:32)");
                    check(2, "mvdata", b, io.mem_flat_vdata, "byte 1 (47:40)");
                }
            } else if (ops.size() == 3 && v0 && v1 && op_last_off && !is_load) {
                check(1, "mvaddr", a, io.mem_flat_vaddr, "byte 0 (39:32)");
                check(2, "mvdata", b, io.mem_flat_vdata, "byte 1 (47:40)");
                check_off();
            } else if (ops.size() == 4 && v0 && v1 && v2 && op_last_off) {
                check(0, "mvdst", a, io.mem_flat_vdst, "byte 3 (63:56)");
                check(1, "mvaddr", b, io.mem_flat_vaddr, "byte 0 (39:32)");
                check(2, "mvdata", c, io.mem_flat_vdata, "byte 1 (47:40)");
                check_off();
            } else if (ops.size() == 2 && v0 && is_load && op1_off) {
                // (v, off): the printed register is the vaddr on the loads that keep the
                // default vdst and the vdst on the addtid form, so only the saddr slot
                // (byte 2 = 0x7D) is identifiable here.
                check_off();
            } else if (ops.size() == 2 && v0 && is_load) {
                uint32_t s = 0;
                const bool is_sreg = reg(ops[1], 's', s);
                if (is_sreg) {
                    check(3, "msaddr", s, io.mem_flat_saddr, "byte 2 (55:48)");
                }
            } else if (ops.size() == 2 && op0_off && op1_off) {
                check_off();
            }
            const char* names[4] = {"mvdst", "mvaddr", "mvdata", "msaddr"};
            for (int i = 0; i < 4; ++i) {
                if (done[i]) continue;
                ++mem_field_stats[std::string("flat ") + names[i]].second;
                ++mem_excl_reasons[std::string("flat ") + names[i]]
                                  ["printed shape does not name this slot (off where the omitted "
                                   "register is unidentifiable, a lone v with saddr, or values "
                                   "that coincide)"];
            }
        }
        if (flat_rows != mem_fmt_rows["flat"]) {
            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "MISMATCH flat operand pass saw %u FLAT rows, per-field row count is %u",
                          flat_rows, mem_fmt_rows["flat"]);
            failures.push_back(buf);
        }
    }

    // ---------------------------------------------- DS register roles (dword 1 bytes)
    // Measured over all 1,865 DS rows: every printed register equals one of the four
    // bytes of dword 1 (7:0, 15:8, 23:16, 31:24). Word0's bytes match no printed
    // register in any position, and word0's 31:16 is the printed offset. The printed
    // position -> dword1 byte permutation is a per-mnemonic property, not a word
    // property: 150 (mnemonic, token count) keys, each with exactly one candidate
    // permutation. Pass 1 derives each key's permutation from a row whose vgpr values
    // are distinct enough to pin the positions; pass 2 asserts every row against it.
    // 0 disagreements over the 1,855 rows that print a register.
    {
        auto vgpr_tokens = [](const std::string& text) {
            std::vector<uint32_t> out;
            for (std::size_t i = 0; i + 1 < text.size(); ++i) {
                const bool boundary =
                    i == 0 || text[i - 1] == ' ' || text[i - 1] == ',' || text[i - 1] == '[';
                if (boundary && text[i] == 'v' && text[i + 1] >= '0' && text[i + 1] <= '9') {
                    out.push_back(static_cast<uint32_t>(std::strtoul(text.c_str() + i + 1, nullptr, 10)));
                }
            }
            return out;
        };
        auto mnemonic_of = [](const std::string& text) {
            return text.empty() ? std::string() : text.substr(0, text.find(' '));
        };
        std::map<std::string, std::array<int, 4>> ds_perm; // "mnemonic/count" -> byte per position
        uint32_t ds_rows = 0, ds_derived = 0, ds_asserted = 0, ds_bad = 0, ds_notoken = 0;
        for (const auto& f : fixtures) { // pass 1: derive the permutation per key
            if (f.words.size() < 2 || ((f.words[0] >> 26) & 0x3fu) != 0x36u) {
                continue;
            }
            const sc_instruction_operands io =
                sc_decode_instruction(f.words.data(), static_cast<uint32_t>(f.words.size()));
            if (io.format != sc_isa_format::ds) {
                continue;
            }
            const std::vector<uint32_t> toks = vgpr_tokens(f.text);
            if (toks.empty()) {
                continue;
            }
            const std::string key = mnemonic_of(f.text) + "/" + std::to_string(toks.size());
            if (ds_perm.find(key) != ds_perm.end()) {
                continue;
            }
            std::array<int, 4> perm = {{-1, -1, -1, -1}};
            bool unique = true;
            for (std::size_t k = 0; k < toks.size() && unique; ++k) {
                int hit = -1;
                for (int b = 0; b < 4; ++b) {
                    if (io.mem_ds_role[b] == toks[k]) {
                        if (hit >= 0) { unique = false; break; }
                        hit = b;
                    }
                }
                if (hit < 0) { unique = false; break; }
                perm[k] = hit;
            }
            if (unique) {
                ds_perm[key] = perm;
                ++ds_derived;
            }
        }
        for (const auto& f : fixtures) { // pass 2: assert every row
            if (f.words.size() < 2 || ((f.words[0] >> 26) & 0x3fu) != 0x36u) {
                continue;
            }
            const sc_instruction_operands io =
                sc_decode_instruction(f.words.data(), static_cast<uint32_t>(f.words.size()));
            if (io.format != sc_isa_format::ds) {
                continue;
            }
            ++ds_rows;
            const std::vector<uint32_t> toks = vgpr_tokens(f.text);
            if (toks.empty()) {
                ++ds_notoken;
                ++mem_field_stats["ds roles"].second;
                char why[240];
                std::snprintf(why, sizeof why,
                              "prints no register operand: operand word 0x%08x carries four 8-bit "
                              "vgpr fields but the row names no v[] position to compare",
                              f.words[1]);
                ++mem_excl_reasons["ds roles"][why];
                continue;
            }
            const std::string key = mnemonic_of(f.text) + "/" + std::to_string(toks.size());
            const auto it = ds_perm.find(key);
            bool ok = it != ds_perm.end();
            for (std::size_t k = 0; ok && k < toks.size(); ++k) {
                ok = it->second[k] >= 0 && io.mem_ds_role[it->second[k]] == toks[k];
            }
            if (ok) {
                ++mem_field_stats["ds roles"].first;
                ++ds_asserted;
            } else {
                ++mem_field_stats["ds roles"].second;
                ++ds_bad;
                char buf[400];
                std::snprintf(buf, sizeof buf,
                              "MISMATCH %s:%u ds roles: %s prints %u registers but dword1 bytes "
                              "%u/%u/%u/%u give no consistent position",
                              f.source.c_str(), f.line, mnemonic_of(f.text).c_str(),
                              static_cast<uint32_t>(toks.size()), io.mem_ds_role[0],
                              io.mem_ds_role[1], io.mem_ds_role[2], io.mem_ds_role[3]);
                failures.push_back(buf);
            }
        }
        std::printf("   ds roles: %u rows, %u permutation keys derived, %u asserted, %u "
                    "disagreements, %u without a printed register%c",
                    ds_rows, ds_derived, ds_asserted, ds_bad, ds_notoken, 10);
        if (ds_rows != mem_fmt_rows["ds"]) {
            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "MISMATCH ds roles pass saw %u DS rows, per-field row count is %u",
                          ds_rows, mem_fmt_rows["ds"]);
            failures.push_back(buf);
        }
    }

    // ------------------- MUBUF vaddr / offset / soffset (dword1 7:0, word0 11:0, 63:56)
    // Measured on all 781 MUBUF rows: vaddr 39:32 matches 100/100 rows that print a
    // register and is 0 on 679/679 rows that print `off`; offset matches 717/717
    // printed offset:N on word0 bits 11:0 (bits 15:0 disagree on 193 rows, because
    // 12..15 are flag bits), and the 64 rows that omit offset:0 all encode 11:0 = 0;
    // soffset 63:56 equals the printed sN index on 590/590 sN rows, while constant
    // soffsets use an 8-bit encoding table (0x80 = 0, 0xC1 = -1, 0xF0 = 0.5,
    // 0xF7 = -4.0, 0x7C = m0), which names no s[] index.
    {
        auto pieces_of = [](const std::string& text) {
            std::vector<std::string> out;
            int depth = 0;
            std::string cur;
            for (const char ch : text) {
                if (ch == '[' || ch == '(') ++depth;
                if ((ch == ']' || ch == ')') && depth > 0) --depth;
                if (ch == ',' && depth == 0) { out.push_back(cur); cur.clear(); }
                else cur.push_back(ch);
            }
            if (!cur.empty()) out.push_back(cur);
            for (auto& p : out) {
                const std::size_t a = p.find_first_not_of(" \t");
                const std::size_t b = p.find_last_not_of(" \t");
                p = (a == std::string::npos) ? std::string() : p.substr(a, b - a + 1);
            }
            return out;
        };
        uint32_t mubuf_rows = 0;
        for (const auto& f : fixtures) {
            if (f.words.size() < 2 || ((f.words[0] >> 26) & 0x3fu) != 0x38u) {
                continue;
            }
            const sc_instruction_operands io =
                sc_decode_instruction(f.words.data(), static_cast<uint32_t>(f.words.size()));
            if (io.format != sc_isa_format::mubuf) {
                continue;
            }
            ++mubuf_rows;
            const std::vector<std::string> pcs = pieces_of(f.text);
            const std::string a2 = pcs.size() > 1 ? pcs[1] : std::string();
            const std::string a4 = pcs.size() > 3 ? pcs[3].substr(0, pcs[3].find(' ')) : std::string();

            if (pcs.size() <= 1) { // buffer_gl0_inv / buffer_gl1_inv: no operands at all
                ++mem_field_stats["mubuf mvaddr"].second;
                ++mem_field_stats["mubuf moffset"].second;
                ++mem_field_stats["mubuf msoffset"].second;
                char why[200];
                std::snprintf(why, sizeof why,
                              "no operands on this mnemonic; operand word 0x%08x is reserved, so "
                              "no role position exists",
                              f.words.size() > 1 ? f.words[1] : 0u);
                ++mem_excl_reasons["mubuf mvaddr"][why];
                ++mem_excl_reasons["mubuf moffset"][why];
                ++mem_excl_reasons["mubuf msoffset"][why];
                continue;
            }

            // vaddr: a printed register must equal 39:32; `off` must be 0 there.
            {
                int want = -1;
                if (a2 == "off") {
                    want = 0;
                } else if (a2.size() > 1 && a2[0] == 'v' && a2[1] >= '0' && a2[1] <= '9') {
                    want = static_cast<int>(std::strtoul(a2.c_str() + 1, nullptr, 10));
                }
                if (want < 0) {
                    ++mem_field_stats["mubuf mvaddr"].second;
                    ++mem_excl_reasons["mubuf mvaddr"]["address operand is neither a v[] register nor "
                                                         "`off` in this row"];
                } else if (static_cast<int>(io.mem_mubuf_vaddr) != want) {
                    ++mem_field_stats["mubuf mvaddr"].second;
                    char buf[320];
                    std::snprintf(buf, sizeof buf,
                                  "MISMATCH %s:%u mubuf mvaddr: text says %s, dword1 7:0 says v%u",
                                  f.source.c_str(), f.line, a2.c_str(), io.mem_mubuf_vaddr);
                    failures.push_back(buf);
                } else {
                    ++mem_field_stats["mubuf mvaddr"].first;
                }
            }

            // offset: printed offset:N must equal word0 11:0; no token means 0 there.
            {
                const std::size_t op = f.text.find("offset:");
                uint32_t want = 0;
                if (op != std::string::npos) {
                    want = static_cast<uint32_t>(std::strtoul(f.text.c_str() + op + 7, nullptr, 10));
                }
                if (io.mem_mubuf_offset != want) {
                    ++mem_field_stats["mubuf moffset"].second;
                    char buf[320];
                    std::snprintf(buf, sizeof buf,
                                  "MISMATCH %s:%u mubuf moffset: text says %u, word0 11:0 says %u",
                                  f.source.c_str(), f.line, want, io.mem_mubuf_offset);
                    failures.push_back(buf);
                } else {
                    ++mem_field_stats["mubuf moffset"].first;
                }
            }

            // soffset: sN is the raw index; constants are an 8-bit encoding table.
            {
                const bool is_reg = a4.size() > 1 && a4[0] == 's' && a4[1] >= '0' && a4[1] <= '9';
                if (!is_reg) {
                    ++mem_field_stats["mubuf msoffset"].second;
                    char why[300];
                    std::snprintf(why, sizeof why,
                                  "constant soffset `%s` is encoded as 0x%02x in 63:56, an operand "
                                  "value table entry rather than an s[] index",
                                  a4.c_str(), io.mem_soffset);
                    ++mem_excl_reasons["mubuf msoffset"][why];
                } else {
                    const uint32_t want =
                        static_cast<uint32_t>(std::strtoul(a4.c_str() + 1, nullptr, 10));
                    if (io.mem_soffset != want) {
                        ++mem_field_stats["mubuf msoffset"].second;
                        char buf[320];
                        std::snprintf(buf, sizeof buf,
                                      "MISMATCH %s:%u mubuf msoffset: text says s%u, dword1 31:24 "
                                      "says 0x%02x",
                                      f.source.c_str(), f.line, want, io.mem_soffset);
                        failures.push_back(buf);
                    } else {
                        ++mem_field_stats["mubuf msoffset"].first;
                    }
                }
            }
        }
        std::printf("   mubuf roles: %u rows (vaddr/offset/soffset measured per row)%c", mubuf_rows, 10);
        if (mubuf_rows != mem_fmt_rows["mubuf"]) {
            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "MISMATCH mubuf role pass saw %u MUBUF rows, per-field row count is %u",
                          mubuf_rows, mem_fmt_rows["mubuf"]);
            failures.push_back(buf);
        }
    }

    // ---------------------- SMEM sbase / soffset / offset (word0 5:0, dword1 31:25, 20:0)
    // Measured on all 459 SMEM rows: sbase matches (word0 5:0) * 2 on 440/440 rows that
    // print it (the 6 two-operand s_dcache_discard rows print sdata + soffset/offset and
    // no sbase); soffset matches dword1 31:25 on 308/308 `sN` rows, with m0/null/vcc_lo/
    // vcc_hi being operand values rather than indices; offset matches dword1 **20:0** on
    // 106/106 printed immediates, while 7:0, 12:0 and 15:0 disagree on 51, 34 and 34 rows.
    {
        auto pieces_of = [](const std::string& text) {
            std::vector<std::string> out;
            int depth = 0;
            std::string cur;
            for (const char ch : text) {
                if (ch == '[' || ch == '(') ++depth;
                if ((ch == ']' || ch == ')') && depth > 0) --depth;
                if (ch == ',' && depth == 0) { out.push_back(cur); cur.clear(); }
                else cur.push_back(ch);
            }
            if (!cur.empty()) out.push_back(cur);
            for (auto& q : out) {
                const std::size_t a = q.find_first_not_of(" \t");
                const std::size_t b = q.find_last_not_of(" \t");
                q = (a == std::string::npos) ? std::string() : q.substr(a, b - a + 1);
            }
            return out;
        };
        uint32_t smem_rows = 0;
        for (const auto& f : fixtures) {
            if (f.words.size() < 2 || ((f.words[0] >> 26) & 0x3fu) != 0x3du) continue;
            const sc_instruction_operands io =
                sc_decode_instruction(f.words.data(), static_cast<uint32_t>(f.words.size()));
            if (io.format != sc_isa_format::smem) continue;
            ++smem_rows;
            const std::vector<std::string> pcs = pieces_of(f.text);

            const bool sbase_is_index = pcs.size() > 2 && pcs[1].size() > 1 &&
                                        pcs[1][0] == 's' &&
                                        (pcs[1][1] == '[' || (pcs[1][1] >= '0' && pcs[1][1] <= '9'));
            if (sbase_is_index) {
                const std::size_t m = pcs[1].find_first_of("0123456789");
                const uint32_t want = (m == std::string::npos) ? 0xffffffffu
                    : static_cast<uint32_t>(std::strtoul(pcs[1].c_str() + m, nullptr, 10));
                if (want != io.mem_sbase) {
                    ++mem_field_stats["smem msbase"].second;
                    char buf[320];
                    std::snprintf(buf, sizeof buf,
                                  "MISMATCH %s:%u smem msbase: text says %s, (word0 5:0) * 2 says %u",
                                  f.source.c_str(), f.line, pcs[1].c_str(), io.mem_sbase);
                    failures.push_back(buf);
                } else {
                    ++mem_field_stats["smem msbase"].first;
                }
            } else {
                ++mem_field_stats["smem msbase"].second;
                if (pcs.size() > 2) {
                    const std::string why =
                        "second operand `" + pcs[1] + "` is a named pair; (word0 5:0) * 2 = " +
                        std::to_string(io.mem_sbase) +
                        " is that pair's index rather than an s[] token";
                    ++mem_excl_reasons["smem msbase"][why];
                } else {
                    ++mem_excl_reasons["smem msbase"][pcs.size() == 2
                        ? "two-operand form prints sdata + soffset/offset and no sbase"
                        : "mnemonic prints no sbase operand in this row"];
                }
            }

            const std::string third =
                pcs.size() > 2 ? pcs[2] : (pcs.size() == 2 ? pcs[1] : std::string());
            const std::size_t sp = third.empty() ? 0 : third.find_first_of(" \t");
            const std::string tok = third.empty() ? std::string() : third.substr(0, sp);
            const bool is_reg = tok.size() > 1 && tok[0] == 's' && tok[1] >= '0' && tok[1] <= '9';

            if (is_reg) {
                const uint32_t want = static_cast<uint32_t>(std::strtoul(tok.c_str() + 1, nullptr, 10));
                if (want != io.mem_smem_soffset) {
                    ++mem_field_stats["smem msoffset"].second;
                    char buf[320];
                    std::snprintf(buf, sizeof buf,
                                  "MISMATCH %s:%u smem msoffset: text says %s, dword1 31:25 says %u",
                                  f.source.c_str(), f.line, tok.c_str(), io.mem_smem_soffset);
                    failures.push_back(buf);
                } else {
                    ++mem_field_stats["smem msoffset"].first;
                }
            } else {
                ++mem_field_stats["smem msoffset"].second;
                char why[300];
                if (tok.empty() || tok[0] == '0') {
                    std::snprintf(why, sizeof why,
                                  "third operand `%s`: dword1 31:25 = 0x%02x is the offset form "
                                  "marker, not an s[] index",
                                  tok.empty() ? "(none)" : tok.c_str(), io.mem_smem_soffset);
                } else {
                    std::snprintf(why, sizeof why,
                                  "third operand `%s` is an operand value encoded 0x%02x in "
                                  "dword1 31:25, not an s[] index",
                                  tok.c_str(), io.mem_smem_soffset);
                }
                ++mem_excl_reasons["smem msoffset"][why];
            }

            uint32_t printed_offset = 0xffffffffu;
            const std::size_t op = f.text.find("0x");
            if (!is_reg && op != std::string::npos) {
                printed_offset = static_cast<uint32_t>(std::strtoul(f.text.c_str() + op + 2, nullptr, 16));
            }
            if (printed_offset != 0xffffffffu) {
                if (printed_offset != io.mem_smem_offset) {
                    ++mem_field_stats["smem moffset"].second;
                    char buf[320];
                    std::snprintf(buf, sizeof buf,
                                  "MISMATCH %s:%u smem moffset: text says 0x%x, dword1 20:0 says 0x%x",
                                  f.source.c_str(), f.line, printed_offset, io.mem_smem_offset);
                    failures.push_back(buf);
                } else {
                    ++mem_field_stats["smem moffset"].first;
                }
            } else {
                ++mem_field_stats["smem moffset"].second;
                ++mem_excl_reasons["smem moffset"]
                    ["row prints no immediate offset (a register or operand value occupies dword1 31:25)"];
            }
        }
        std::printf("   smem roles: %u rows (sbase/soffset/offset measured per row)%c", smem_rows, 10);
        if (smem_rows != mem_fmt_rows["smem"]) {
            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "MISMATCH smem role pass saw %u SMEM rows, per-field row count is %u",
                          smem_rows, mem_fmt_rows["smem"]);
            failures.push_back(buf);
        }
    }

    // --------------------------------------------------------------- failures
    banner("asserted operand tokens per field");
    {
        uint32_t asserted_total = 0;
        uint32_t word_only_sum = 0;   // asserted from words, no printed token to compare
        uint32_t excluded_total = 0;
        for (const auto& kv : mem_field_stats) {
            const std::string& key = kv.first;
            const std::string shape = key.substr(0, key.find(0x20));
            const uint32_t asserted = kv.second.first;
            const uint32_t excluded = kv.second.second;
            const uint32_t rows = mem_fmt_rows[shape];
            asserted_total += asserted;
            word_only_sum += mem_word_decoded[key];
            excluded_total += excluded;
            std::printf("   %-16s asserted %6u  word-only %4u  excluded %4u  rows %6u", key.c_str(),
                        asserted, mem_word_decoded[key], excluded, rows);
            if (asserted + excluded != rows) {
                std::printf("  MISMATCH");
                failures.push_back(key + ": asserted " + std::to_string(asserted) + " + excluded " +
                                  std::to_string(excluded) + " != fixture rows " + std::to_string(rows));
            }
            std::printf("%c", 10);
            for (const auto& why : mem_excl_reasons[key]) {
                std::printf("       %4u excluded: %s", why.second, why.first.c_str());
                std::printf("%c", 10);
            }
        }
        std::printf("   %-16s asserted %6u  word-only %4u  excluded %4u%c", "all memory fields",
                    asserted_total, word_only_sum, excluded_total, 10);
    }

    banner("conformance failures (work queue, top of list)");
    std::printf("   %zu failure(s)\n", failures.size());
    for (size_t i = 0; i < failures.size() && i < 25; ++i) {
        std::printf("   ! %s\n", failures[i].c_str());
    }

    banner("not decoded (work queue, by reason)");
    for (const auto& q : queue) {
        std::printf("   - %-70s %u\n", q.first.c_str(), q.second);
    }

    // ------------------------------------------------------------ pass rates
    banner("measured pass rate per decoded format");
    std::printf("   %-10s %8s %8s %8s %8s %10s %10s %8s\n", "format", "fixtures", "fmt ok", "len ok", "passed",
                "op checks", "op ok", "rate");
    std::string table;
    {
        std::ostringstream head;
        head << "| format | fixtures | format ok | length ok | operands checked | operands ok | passed | pass rate |\n";
        head << "|---|---|---|---|---|---|---|---|\n";
        table += head.str();
    }
    for (const auto& entry : by_format) {
        const format_stat& s = entry.second;
        const double rate = s.fixtures == 0 ? 0.0 : (100.0 * static_cast<double>(s.passed) / s.fixtures);
        std::printf("   %-10s %8u %8u %8u %8u %10u %10u %7.1f%%\n", entry.first.c_str(), s.fixtures, s.format_ok,
                    s.length_ok, s.passed, s.operand_checks, s.operand_ok, rate);
        std::ostringstream row;
        row << "| " << entry.first << " | " << s.fixtures << " | " << s.format_ok << " | " << s.length_ok << " | "
            << s.operand_checks << " | " << s.operand_ok << " | " << s.passed << " | " << rate << "% |\n";
        table += row.str();
    }

    banner("per source family");
    std::printf("   %-12s %8s %8s %8s %10s %10s %8s\n", "family", "fixtures", "fmt ok", "len ok", "op checks",
                "op ok", "passed");
    for (const auto& entry : by_kind) {
        const kind_stat& s = entry.second;
        std::printf("   %-12s %8u %8u %8u %10u %10u %8u\n", entry.first.c_str(), s.fixtures, s.format_ok,
                    s.length_ok, s.operand_checks, s.operand_ok, s.passed);
    }

    // ------------------------------------------------------------- expectations
    banner("conformance gate");
    uint32_t total = 0;
    uint32_t passed = 0;
    for (const auto& entry : by_format) {
        total += entry.second.fixtures;
        passed += entry.second.passed;
    }
    uint32_t selector_checks = 0;
    uint32_t selector_ok = 0;
    for (const auto& entry : by_format) {
        selector_checks += entry.second.selector_checks;
        selector_ok += entry.second.selector_ok;
    }
    std::printf("   %u sdwa attribute checks (dst_sel/dst_unused/srcN_sel/clamp/omod), %u matched\n",
                selector_checks, selector_ok);
    uint32_t dpp_checks = 0;
    uint32_t dpp_ok = 0;
    for (const auto& entry : by_format) {
        dpp_checks += entry.second.dpp_checks;
        dpp_ok += entry.second.dpp_ok;
    }
    std::printf("   %u dpp control checks (lanes/row op/masks/fi/bound_ctrl/modifiers), %u matched\n",
                dpp_checks, dpp_ok);
    std::printf("   %u of %u fixtures passed every applicable check\n", passed, total);
    CHECK(failures.empty());
    CHECK(total == fixtures.size());
    // The formats we claim to decode must all be represented in the fixtures --
    // otherwise "100% pass" could mean "nothing was tested".
    for (const char* fmt : {"vop1", "vop2", "vopc", "sop1", "sop2", "sopc", "sopp", "sopk"}) {
        CHECK(by_format.count(fmt) == 1);
    }
    CHECK(known_opcode_checks >= 3);
    CHECK(!opcode_of.empty());

    // ------------------------------------------------------------------ report
    std::string report = "# GFX10 decode conformance (synthetic fixtures)\n\n";
    report += "Generated by `tests/shader/sc_conformance_test.exe` from\n";
    report += "`tests/shader/fixtures/gfx10_mc_fixtures.tsv` (harvested from LLVM's\n";
    report += "`llvm/test/MC/AMDGPU/gfx10_asm_*.s`; **synthetic** assembler output, not captured game data).\n\n";
    report += "Each fixture asserts what the file family says it is (format), the dword count it\n";
    report += "encodes (length), one opcode value per (format, mnemonic), and the classification of\n";
    report += "every plain-register operand in the mnemonic against our decoded register file.\n\n";
    report += "Fixtures: " + std::to_string(fixtures.size()) + ", passed every check: " + std::to_string(passed) +
              ", failures: " + std::to_string(failures.size()) + "\n\n";
    report += "## Pass rate per decoded format\n\n" + table;
    report += "\n## Not decoded (work queue)\n\n| reason | fixtures |\n|---|---|\n";
    for (const auto& q : queue) {
        report += "| " + q.first + " | " + std::to_string(q.second) + " |\n";
    }
    report += "\n## Failures (work queue)\n\n";
    if (failures.empty()) {
        report += "None.\n";
    } else {
        for (const auto& failure : failures) {
            report += "- " + failure + "\n";
        }
    }

    const std::string report_path = report_dir + "/gfx10_conformance.md";
    std::ofstream out(report_path.c_str(), std::ios::binary | std::ios::trunc);
    CHECK(static_cast<bool>(out));
    if (out) {
        out.write(report.data(), static_cast<std::streamsize>(report.size()));
        out.flush();
        CHECK(out.good());
        std::printf("\n   report: %s (%zu bytes)\n", report_path.c_str(), report.size());
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
