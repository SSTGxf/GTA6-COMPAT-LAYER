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
#include <cstring>
#include <fstream>
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
    return {};
}

struct op_token {
    bool plain = false; // a bare vN/sN or v[a:b]/s[a:b] (modifiers stripped)
    bool vgpr = false;
    bool pair = false;
    bool neg = false;   // printed -vN / -|vN|
    bool abs = false;   // printed |vN|
    uint32_t index = 0;
};

op_token parse_operand_token(const std::string& piece) {
    op_token out;
    size_t begin = piece.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return out;
    }
    size_t end = piece.find_first_of(" \t", begin);
    std::string token = piece.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    if (token.size() >= 2 && token[0] == '-') {
        out.neg = true;
        token.erase(0, 1);
    }
    if (token.size() >= 3 && token.front() == '|' && token.back() == '|') {
        out.abs = true;
        token = token.substr(1, token.size() - 2);
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
    uint32_t passed = 0;
};

struct format_stat {
    uint32_t fixtures = 0;
    uint32_t format_checked = 0;
    uint32_t format_ok = 0;
    uint32_t length_ok = 0;
    uint32_t operand_checks = 0;
    uint32_t operand_ok = 0;
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
        bool length_ok = consumed == encoded;
        if (length_ok) {
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
                                first.format == sc_isa_format::vop3;
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
                            if (!token.plain) {
                                ++queue["operand token not a plain register (vcc/exec/literal/modifiers)"];
                                continue;
                            }
                            ++ks.operand_checks;
                            ++fs.operand_checks;
                            const sc_operand_file expected_file =
                                token.vgpr ? sc_operand_file::vgpr : sc_operand_file::sgpr;
                            if (r.operand->file == expected_file && r.operand->index == token.index) {
                                ++ks.operand_ok;
                                ++fs.operand_ok;
                            } else {
                                report(r.name, *r.operand, (token.vgpr ? "v" : "s") + std::to_string(token.index));
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
                            if (!token.plain) {
                                ++queue["operand token not a plain register (vcc/exec/literal/modifiers)"];
                                continue;
                            }
                            ++ks.operand_checks;
                            ++fs.operand_checks;
                            const sc_operand_file expected_file =
                                token.vgpr ? sc_operand_file::vgpr : sc_operand_file::sgpr;
                            bool ok = r.operand->file == expected_file && r.operand->index == token.index;
                            if (ok && ops.modifiers.known) {
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

                    if (first.format != sc_isa_format::vop3) {
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
                    const bool sdst_inserted =
                        pieces.size() >= 5 || (pieces.size() == 4 && is_scalar_token(pieces[1]));
                    if (first.format == sc_isa_format::vop2 && sdst_inserted) {
                        src0_index = 2;
                        src1_index = 3;
                    } else if (first.format == sc_isa_format::vopc) {
                        // VOPC has no encoded destination: the result goes to vcc.
                        // cmp prints it, cmpx does not, so the sources shift.
                        src0_index = (cmpx && pieces.size() == 2) ? 0 : 1;
                        src1_index = src0_index + 1;
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
                    };
                    std::vector<role> roles;
                    roles.push_back({ops.has_vdst && check_dst, &ops.vdst, dst_index, "dst"});
                    roles.push_back({ops.has_src0 && check_src0, &ops.src0, src0_index, "src0"});
                    roles.push_back({ops.has_vsrc1, &ops.vsrc1, src1_index, "src1"});

                    for (const auto& r : roles) {
                        if (!r.present) {
                            continue;
                        }
                        if (r.token_index >= pieces.size()) {
                            ++queue[std::string("mnemonic text has no ") + r.name + " token for format " + fmt];
                            continue;
                        }
                        const op_token token = parse_operand_token(pieces[r.token_index]);
                        if (!token.plain) {
                            ++queue["operand token not a plain register (vcc/exec/literal/modifiers)"];
                            continue;
                        }
                        ++ks.operand_checks;
                        ++fs.operand_checks;
                        const sc_operand_file expected_file =
                            token.vgpr ? sc_operand_file::vgpr : sc_operand_file::sgpr;
                        const bool ok = r.operand->file == expected_file && r.operand->index == token.index;
                        if (ok) {
                            ++ks.operand_ok;
                            ++fs.operand_ok;
                        } else {
                            row_ok = false;
                            char buf[320];
                            std::snprintf(buf, sizeof(buf),
                                          "%s:%u  %s  operand %s: expected %s%u, got %s (field 0x%x)",
                                          f.source.c_str(), f.line, f.text.c_str(), r.name,
                                          token.vgpr ? "v" : "s", token.index,
                                          sc_operand_name(*r.operand).c_str(), r.operand->field);
                            failures.push_back(buf);
                        }
                    }
                    }
                }
            }
        }

        if (row_ok) {
            ++ks.passed;
            ++fs.passed;
        }
    }

    // ------------------------------------------------------- resolved opcodes
    banner("opcode table resolved from the fixtures (observed, not AMD numbering except the pinned three)");
    std::printf("   %zu (format, mnemonic) pairs resolved\n", opcode_of.size());
    std::printf("   %zu known-opcode assertions applied\n", static_cast<size_t>(known_opcode_checks));

    // --------------------------------------------------------------- failures
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
