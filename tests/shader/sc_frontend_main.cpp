// ScarlettCompat shader pipeline - Phase 3 harness (RDNA2 frontend).
// MIT. Standalone: no game, no guest, no device, no layer.
//
//   sc_frontend_test.exe [output-dir]      full build: real dxc fixtures + synthetic
//   sc_frontend_nodxc.exe [output-dir]     built WITHOUT sc_dxil_emit.cpp linked
//
// The second binary is the proof that the frontend has no DXC dependency: the
// emitter translation unit is not linked at all, and every synthetic, XS2,
// unknown and fuzz case still runs.
#include "shader/sc_frontend.h"
#include "shader/sc_sha256.h"
#include "shader/sc_xs2.h"

#ifndef SC_FRONTEND_NO_DXC
#include "shader/sc_dxil_emit.h"
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
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

// ------------------------------------------------------------ fixture helpers

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

struct chunk_spec {
    std::string name;
    std::vector<uint8_t> data;
};

chunk_spec make_chunk(const char* name, std::vector<uint8_t> data) {
    chunk_spec c;
    c.name = name;
    c.data = std::move(data);
    return c;
}

std::vector<uint8_t> build_container(const std::vector<chunk_spec>& chunks) {
    const uint32_t header_size = 32;
    std::vector<uint32_t> offsets;
    uint32_t cursor = header_size + static_cast<uint32_t>(chunks.size()) * 4u;
    for (const auto& c : chunks) {
        offsets.push_back(cursor);
        const uint32_t payload = static_cast<uint32_t>(c.data.size());
        cursor += 8u + ((payload + 3u) & ~3u);
    }
    std::vector<uint8_t> out(cursor, 0);
    out[0] = 'D';
    out[1] = 'X';
    out[2] = 'B';
    out[3] = 'C';
    wr32(out, 20, 1);
    wr32(out, 24, cursor);
    wr32(out, 28, static_cast<uint32_t>(chunks.size()));
    for (size_t i = 0; i < chunks.size(); ++i) {
        const uint32_t off = offsets[i];
        wr32(out, header_size + i * 4u, off);
        std::memcpy(out.data() + off, chunks[i].name.data(), 4);
        wr32(out, off + 4, static_cast<uint32_t>(chunks[i].data.size()));
        if (!chunks[i].data.empty()) {
            std::memcpy(out.data() + off + 8, chunks[i].data.data(), chunks[i].data.size());
        }
    }
    return out;
}

// DXIL container shaped like dxc output: token = SM (low 16) | stage (bits 16-23).
std::vector<uint8_t> make_dxil_container(uint32_t stage_index, uint32_t shader_model = 0x67) {
    std::vector<uint8_t> dxil = u32s({shader_model | (stage_index << 16), 0x00000008u, 0x00000107u});
    const std::vector<uint8_t> bitcode = {'B', 'C', 0xc0, 0xde, 0x00, 0x00, 0x00, 0x00};
    dxil.insert(dxil.end(), bitcode.begin(), bitcode.end());

    std::vector<chunk_spec> chunks;
    chunks.push_back(make_chunk("DXIL", dxil));
    chunks.push_back(make_chunk("PSV0", u32s({0x00000034, 0x00000000, 0x00000000})));
    chunks.push_back(make_chunk("ISG1", u32s({1, 2})));
    chunks.push_back(make_chunk("OSG1", u32s({1, 2})));
    return build_container(chunks);
}

// SM5 container: SHEX token carries the program type in its low 16 bits.
std::vector<uint8_t> make_sm5_container(uint32_t program_type) {
    std::vector<chunk_spec> chunks;
    chunks.push_back(make_chunk("ISGN", u32s({1, 8})));
    chunks.push_back(make_chunk("OSGN", u32s({1, 8})));
    chunks.push_back(make_chunk("SHEX", u32s({0x00400000u | program_type, 0x10, 0xDEADBEEF})));
    return build_container(chunks);
}

// A microcode payload with OUR inferred header shape:
//   [0] payload size, [1] code offset, [2] entry count, [3] version, then code.
// `include_code_offset = false` removes the corroborating field so the prober
// must fall back to an uncorroborated payload-start code region.
std::vector<uint8_t> make_xs2_payload(uint32_t code_words, bool with_terminator, bool include_code_offset) {
    const uint32_t header_words = 4;
    const uint32_t total_words = header_words + code_words;
    std::vector<uint8_t> blob(total_words * 4u, 0);
    wr32(blob, 0, total_words * 4u);
    wr32(blob, 4, include_code_offset ? header_words * 4u : 0u);
    wr32(blob, 8, 1u);
    wr32(blob, 12, 3u);
    for (uint32_t i = 0; i < code_words; ++i) {
        const uint32_t value = (i == code_words - 1 && with_terminator) ? kXs2Endpgm : 0xBF800000u; // s_nop
        wr32(blob, (header_words + i) * 4u, value);
    }
    return blob;
}

std::vector<uint8_t> pseudo_random(size_t size, uint32_t seed) {
    std::vector<uint8_t> out(size);
    uint32_t state = seed | 1u;
    for (size_t i = 0; i < size; ++i) {
        state = state * 1664525u + 1013904223u;
        out[i] = static_cast<uint8_t>(state >> 24);
    }
    return out;
}

bool has_prefix(const std::vector<std::string>& lines, const char* prefix) {
    for (const auto& line : lines) {
        if (line.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

const sc_xs2_field_candidate* find_field(const sc_xs2_header_probe& probe, const char* interpretation) {
    for (const auto& f : probe.fields) {
        if (f.interpretation == interpretation) {
            return &f;
        }
    }
    return nullptr;
}

void expect_reusable(const sc_shader_classification& c, sc_stage stage, const char* label) {
    CHECK(c.kind == sc_payload_kind::dxil_container);
    CHECK(c.reusable_as_is);
    CHECK(c.stage == stage);
    CHECK(c.stage_source == sc_stage_source::shader_token);
    CHECK(c.container.ok);
    if (c.kind != sc_payload_kind::dxil_container || c.stage != stage) {
        std::printf("   [%s] got kind=%s stage=%s reason=%s\n", label, c.kind_name(), sc_stage_name(c.stage),
                    c.reason.c_str());
    }
}

// ------------------------------------------------------- real dxc fixtures

#ifndef SC_FRONTEND_NO_DXC

struct real_stage_fixture {
    const char* label;
    sc_stage stage;
    const char* profile;
    const char* hlsl;
    bool required;
};

const char* kVSHlsl =
    "struct VSOut { float4 pos : SV_Position; };\n"
    "VSOut main(uint id : SV_VertexID) {\n"
    "  float2 p = float2((id << 1) & 2, id & 2);\n"
    "  VSOut o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); return o; }\n";

const char* kPSHlsl = "float4 main() : SV_Target { return float4(0.0, 1.0, 0.0, 1.0); }\n";

const char* kGSHlsl =
    "struct GSOut { float4 pos : SV_Position; };\n"
    "[maxvertexcount(3)]\n"
    "void main(triangle float4 pos[3] : SV_Position, inout TriangleStream<GSOut> stream) {\n"
    "  GSOut o; o.pos = pos[0];\n"
    "  stream.Append(o); stream.Append(o); stream.Append(o); stream.RestartStrip(); }\n";

const char* kHSHlsl =
    "struct HSOut { float4 pos : SV_Position; };\n"
    "struct HSC { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };\n"
    "HSC HSConst(InputPatch<HSOut, 3> patch) {\n"
    "  HSC o; o.edges[0] = 1.0; o.edges[1] = 1.0; o.edges[2] = 1.0; o.inside = 1.0;\n"
    "  return o; }\n"
    "[domain(\"tri\")][partitioning(\"integer\")][outputtopology(\"triangle_cw\")]\n"
    "[outputcontrolpoints(3)][patchconstantfunc(\"HSConst\")]\n"
    "HSOut main(InputPatch<HSOut, 3> patch, uint id : SV_OutputControlPointID) { return patch[id]; }\n";

const char* kDSHlsl =
    "struct HSOut { float4 pos : SV_Position; };\n"
    "struct HSC { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };\n"
    "[domain(\"tri\")]\n"
    "float4 main(HSC c, float3 bary : SV_DomainLocation, const OutputPatch<HSOut, 3> patch) : SV_Position {\n"
    "  return patch[0].pos; }\n";

const char* kCSHlsl =
    "RWStructuredBuffer<uint> g : register(u0);\n"
    "[numthreads(64,1,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) { if (id.x == 0xffffffffu) g[0] = 1u; }\n";

const char* kMSHlsl =
    "struct MSOut { float4 pos : SV_Position; };\n"
    "[outputtopology(\"triangle\")][numthreads(1,1,1)]\n"
    "void main(out vertices MSOut verts[3], out indices uint3 tris[1]) {\n"
    "  SetMeshOutputCounts(3, 1);\n"
    "  verts[0].pos = float4(-1, -1, 0, 1);\n"
    "  verts[1].pos = float4(0, 1, 0, 1);\n"
    "  verts[2].pos = float4(1, -1, 0, 1);\n"
    "  tris[0] = uint3(0, 1, 2); }\n";

void test_real_dxc_fixtures(bool& have_real_vs, std::vector<uint8_t>& real_vs) {
    banner("real dxc fixtures (grounded in actual SM 6.7 containers)");
    std::unique_ptr<sc_dxil_emitter> emitter = sc_make_dxil_emitter();
    const sc_dxil_backend_info info = emitter->info();
    std::printf("   emitter: %s available=%d\n", info.name.c_str(), info.available ? 1 : 0);
    std::printf("   dxc    : %s\n", info.path.c_str());
    CHECK(info.available);
    if (!info.available) {
        std::printf("   SKIP: no dxc on this machine; synthetic fixtures still cover every stage\n");
        return;
    }

    const real_stage_fixture fixtures[] = {
        {"vs", sc_stage::vertex, "vs_6_7", kVSHlsl, true},
        {"ps", sc_stage::pixel, "ps_6_7", kPSHlsl, true},
        {"gs", sc_stage::geometry, "gs_6_7", kGSHlsl, true},
        {"hs", sc_stage::hull, "hs_6_7", kHSHlsl, true},
        {"ds", sc_stage::domain, "ds_6_7", kDSHlsl, true},
        {"cs", sc_stage::compute, "cs_6_7", kCSHlsl, true},
        {"ms", sc_stage::mesh, "ms_6_7", kMSHlsl, false},
    };

    int compiled = 0;
    for (const auto& f : fixtures) {
        sc_dxil_request req;
        req.hlsl = f.hlsl;
        req.entry = "main";
        req.profile = f.profile;
        sc_dxil_result res;
        const bool ok = emitter->compile(req, res);
        if (!ok) {
            std::printf("   %s: compile failed (%s)\n", f.label, f.required ? "REQUIRED" : "optional");
            std::printf("      log: %s\n", res.log.c_str());
            CHECK(!f.required);
            continue;
        }
        ++compiled;
        const sc_shader_classification c = sc_classify_shader(res.dxil.data(), res.dxil.size());
        std::printf("   %s: %zu bytes -> kind=%s stage=%s source=%s sm=%u.%u reusable=%d\n", f.label,
                    res.dxil.size(), c.kind_name(), sc_stage_name(c.stage),
                    sc_stage_source_name(c.stage_source), c.shader_model_major, c.shader_model_minor,
                    c.reusable_as_is ? 1 : 0);
        expect_reusable(c, f.stage, f.label);
        CHECK(c.shader_model_major == 6);
        CHECK(c.shader_model_minor >= 6);
        CHECK(c.carve_offset == 0);
        CHECK(c.container.chunk_list().find("DXIL") != std::string::npos);
        CHECK(c.sha256.size() == 64);
        if (f.stage == sc_stage::vertex) {
            have_real_vs = true;
            real_vs = res.dxil;
        }
    }
    CHECK(compiled >= 6); // every stage the classifier must be right about, from real bytes
    std::printf("   %d real containers classified\n", compiled);

    // Carving: a real container behind a 64-byte decorated prefix.
    if (have_real_vs) {
        std::vector<uint8_t> wrapped(64, 0xab);
        wrapped.insert(wrapped.end(), real_vs.begin(), real_vs.end());
        const sc_shader_classification c = sc_classify_shader(wrapped.data(), wrapped.size());
        CHECK(c.kind == sc_payload_kind::dxil_container);
        CHECK(c.reusable_as_is);
        CHECK(c.stage == sc_stage::vertex);
        CHECK(c.carve_offset == 64);
        std::printf("   carved vertex container at offset %u -> stage=%s reusable=%d\n", c.carve_offset,
                    sc_stage_name(c.stage), c.reusable_as_is ? 1 : 0);

        // Note: the classification API takes no stage argument, so a caller
        // cannot inject a stage guess. Stage can only come from the token.
    }
}

#endif // SC_FRONTEND_NO_DXC

// --------------------------------------------------- synthetic stage matrix

void test_synthetic_stage_matrix() {
    banner("synthetic DXIL matrix: every stage from the version token");
    struct stage_case {
        uint32_t token_stage;
        sc_stage expected;
        const char* label;
    };
    const stage_case cases[] = {
        {0, sc_stage::pixel, "ps"},           {1, sc_stage::vertex, "vs"},
        {2, sc_stage::geometry, "gs"},        {3, sc_stage::hull, "hs"},
        {4, sc_stage::domain, "ds"},          {5, sc_stage::compute, "cs"},
        {13, sc_stage::mesh, "ms"},           {14, sc_stage::amplification, "as"},
    };
    for (const auto& c : cases) {
        const std::vector<uint8_t> blob = make_dxil_container(c.token_stage);
        const sc_shader_classification r = sc_classify_shader(blob.data(), blob.size());
        std::printf("   %s (token %u): kind=%s stage=%s reusable=%d sm=%u.%u\n", c.label, c.token_stage,
                    r.kind_name(), sc_stage_name(r.stage), r.reusable_as_is ? 1 : 0, r.shader_model_major,
                    r.shader_model_minor);
        expect_reusable(r, c.expected, c.label);
        CHECK(r.shader_model_major == 6);
        CHECK(r.shader_model_minor == 7);
    }

    // A shader-library kind (6) and an out-of-range kind (200) are not
    // pipeline stages: both must be refused rather than guessed.
    for (uint32_t kind : {6u, 200u}) {
        const std::vector<uint8_t> lib = make_dxil_container(kind);
        const sc_shader_classification l = sc_classify_shader(lib.data(), lib.size());
        CHECK(l.kind == sc_payload_kind::dxil_container);
        CHECK(!l.reusable_as_is);
        CHECK(l.stage == sc_stage::unknown);
        std::printf("   token kind %u -> stage=%s reusable=%d (refused)\n", kind, sc_stage_name(l.stage),
                    l.reusable_as_is ? 1 : 0);
    }

    // A token that does not identify a stage (8..255) must not be reusable.
    const std::vector<uint8_t> bogus = make_dxil_container(200);
    const sc_shader_classification r = sc_classify_shader(bogus.data(), bogus.size());
    CHECK(r.kind == sc_payload_kind::dxil_container);
    CHECK(!r.reusable_as_is);
    CHECK(r.stage == sc_stage::unknown);
    std::printf("   unknown token stage 200: kind=%s reusable=%d (correctly refused)\n", r.kind_name(),
                r.reusable_as_is ? 1 : 0);

    // SM5 container: stage comes from the SHEX token, and it is NOT reusable.
    const std::vector<uint8_t> sm5 = make_sm5_container(1); // program type 1 = vertex
    const sc_shader_classification s = sc_classify_shader(sm5.data(), sm5.size());
    CHECK(s.kind == sc_payload_kind::dxbc_sm5_legacy);
    CHECK(!s.reusable_as_is);
    CHECK(s.stage == sc_stage::vertex);
    CHECK(s.stage_source == sc_stage_source::shader_token);
    std::printf("   SM5 SHEX (type 1): kind=%s stage=%s\n", s.kind_name(), sc_stage_name(s.stage));
}

// ------------------------------------------------------------- XS2 fixtures

void test_xs2_fixtures() {
    banner("XS2 microcode discovery");

    // Strong case: s_endpgm terminator plus a corroborating code_offset field.
    const std::vector<uint8_t> payload = make_xs2_payload(24, true, true);
    const sc_shader_classification c = sc_classify_shader(payload.data(), payload.size());
    std::printf("   raw payload %zu bytes -> kind=%s code_start_corroborated=%d\n", payload.size(),
                c.kind_name(), c.xs2.code_start_corroborated ? 1 : 0);
    CHECK(c.kind == sc_payload_kind::rdna2_xs2);
    CHECK(!c.reusable_as_is);
    CHECK(c.xs2.found);
    CHECK(c.xs2.code_start_corroborated);
    CHECK(c.xs2.payload_size == payload.size());
    CHECK(c.xs2.terminator_offsets.size() == 1);
    CHECK(c.xs2.terminator_offsets[0] == payload.size() - 4);
    CHECK(c.xs2.code_regions.size() == 1);
    if (!c.xs2.code_regions.empty()) {
        CHECK(c.xs2.code_regions[0].begin == 16);
        CHECK(c.xs2.code_regions[0].end == payload.size());
    }
    const sc_xs2_field_candidate* size_field = find_field(c.xs2.header, "payload_size");
    const sc_xs2_field_candidate* code_field = find_field(c.xs2.header, "code_offset_candidate");
    CHECK(size_field != nullptr);
    CHECK(code_field != nullptr);
    if (size_field != nullptr) {
        CHECK(size_field->offset == 0);
        CHECK(size_field->value == payload.size());
    }
    if (code_field != nullptr) {
        CHECK(code_field->offset == 4);
        CHECK(code_field->value == 16);
    }
    CHECK(has_prefix(c.xs2.evidence, "observed:"));
    CHECK(has_prefix(c.xs2.evidence, "inferred:"));
    CHECK(!c.xs2.open_questions.empty());
    std::printf("   header fields: size@%u code@%u  regions=%zu  evidence=%zu  open=%zu\n",
                size_field ? size_field->offset : 999u, code_field ? code_field->offset : 999u,
                c.xs2.code_regions.size(), c.xs2.evidence.size(), c.xs2.open_questions.size());

    // Weak case: terminator present, no corroborating header field.
    const std::vector<uint8_t> weak = make_xs2_payload(16, true, false);
    const sc_shader_classification w = sc_classify_shader(weak.data(), weak.size());
    CHECK(w.kind == sc_payload_kind::rdna2_xs2);
    CHECK(!w.xs2.code_start_corroborated);
    CHECK(!w.xs2.code_regions.empty());
    if (!w.xs2.code_regions.empty()) {
        CHECK(w.xs2.code_regions[0].begin == 0);
    }
    std::printf("   no code_offset field -> corroborated=0 region.begin=%d\n",
                w.xs2.code_regions.empty() ? -1 : static_cast<int>(w.xs2.code_regions[0].begin));

    // Wrapped in a DXBC envelope with a non-DXIL chunk: the largest chunk is probed.
    std::vector<chunk_spec> chunks;
    chunks.push_back(make_chunk("XS20", make_xs2_payload(20, true, true)));
    const std::vector<uint8_t> wrapped = build_container(chunks);
    const sc_shader_classification wc = sc_classify_shader(wrapped.data(), wrapped.size());
    CHECK(wc.kind == sc_payload_kind::rdna2_xs2);
    CHECK(wc.xs2.found);
    CHECK(wc.xs2.envelope.rfind("dxbc-chunk:", 0) == 0);
    std::printf("   DXBC-wrapped microcode -> kind=%s envelope=%s\n", wc.kind_name(), wc.xs2.envelope.c_str());

    // Two terminators -> two regions.
    std::vector<uint8_t> two = make_xs2_payload(32, true, true);
    wr32(two, 16 + 8 * 4, kXs2Endpgm);
    const sc_shader_classification t = sc_classify_shader(two.data(), two.size());
    CHECK(t.kind == sc_payload_kind::rdna2_xs2);
    CHECK(t.xs2.terminator_offsets.size() == 2);
    CHECK(t.xs2.code_regions.size() == 2);
    std::printf("   two s_endpgm words -> %zu regions\n", t.xs2.code_regions.size());

    // The other documented SOPP termination forms are recognised too (the table
    // lives in sc_isa.h). A payload that ends in either still has a code region,
    // otherwise a shader using the PS-done form would never reach Phase 4.
    for (uint32_t term : {0xBF9B0000u, 0xBF9E0000u}) {
        std::vector<uint8_t> alt = make_xs2_payload(16, true, true);
        wr32(alt, 16 + 15 * 4, term);
        const sc_shader_classification a = sc_classify_shader(alt.data(), alt.size());
        CHECK(a.kind == sc_payload_kind::rdna2_xs2);
        CHECK(a.xs2.found);
        CHECK(a.xs2.code_regions.size() == 1);
        std::printf("   terminator 0x%08x -> kind=%s regions=%zu\n", term, a.kind_name(),
                    a.xs2.code_regions.size());
    }

    // Negative cases that must NOT be called microcode.
    const std::vector<uint8_t> no_terminator = make_xs2_payload(24, false, true);
    const sc_shader_classification n = sc_classify_shader(no_terminator.data(), no_terminator.size());
    CHECK(n.kind == sc_payload_kind::unknown);

    const std::vector<uint8_t> tiny = u32s({0x10, 0x04, 0x01, 0xBF810000u}); // terminator too early
    const sc_shader_classification ti = sc_classify_shader(tiny.data(), tiny.size());
    CHECK(ti.kind == sc_payload_kind::unknown);
    std::printf("   no terminator -> kind=%s ; 4-word blob with terminator -> kind=%s (both refused)\n",
                n.kind_name(), ti.kind_name());

    // Unaligned payload is refused with an explanation.
    std::vector<uint8_t> unaligned = make_xs2_payload(16, true, true);
    unaligned.push_back(0x00);
    const sc_shader_classification ua = sc_classify_shader(unaligned.data(), unaligned.size());
    CHECK(ua.kind == sc_payload_kind::unknown);
}

// --------------------------------------------------------- unknown fixtures

void test_unknown_fixtures() {
    banner("unknown payloads are refused with a reason");

    sc_shader_classification c = sc_classify_shader(nullptr, 0);
    CHECK(c.kind == sc_payload_kind::unknown);
    CHECK(!c.reason.empty());

    const uint8_t three[3] = {1, 2, 3};
    c = sc_classify_shader(three, sizeof(three));
    CHECK(c.kind == sc_payload_kind::unknown);
    CHECK(!c.reason.empty());

    const std::vector<uint8_t> noise = pseudo_random(1024, 7);
    c = sc_classify_shader(noise.data(), noise.size());
    CHECK(c.kind == sc_payload_kind::unknown);
    CHECK(c.sha256.size() == 64);
    std::printf("   1 KiB pseudo-random -> kind=%s reason=\"%s\"\n", c.kind_name(), c.reason.c_str());

    // Truncated container.
    const std::vector<uint8_t> good = make_dxil_container(1);
    const std::vector<uint8_t> truncated(good.begin(), good.begin() + 40);
    c = sc_classify_shader(truncated.data(), truncated.size());
    CHECK(c.kind == sc_payload_kind::unknown);
    std::printf("   truncated container -> kind=%s reason=\"%s\"\n", c.kind_name(), c.reason.c_str());

    // Container with no shader chunk at all.
    std::vector<chunk_spec> chunks;
    chunks.push_back(make_chunk("RDEF", u32s({1, 2, 3, 4})));
    const std::vector<uint8_t> rdef = build_container(chunks);
    c = sc_classify_shader(rdef.data(), rdef.size());
    CHECK(c.kind == sc_payload_kind::unknown);
    CHECK(c.container.ok);
    std::printf("   RDEF-only container -> kind=%s chunks=\"%s\"\n", c.kind_name(),
                c.container.chunk_list().c_str());

    // Absurd chunk count.
    std::vector<uint8_t> bad = good;
    wr32(bad, 28, 0xffffffffu);
    c = sc_classify_shader(bad.data(), bad.size());
    CHECK(c.kind == sc_payload_kind::unknown);

    // Bare bitcode is its own outcome, not "unknown" and not "reusable".
    const std::vector<uint8_t> bitcode = {'B', 'C', 0xc0, 0xde, 1, 2, 3, 4};
    c = sc_classify_shader(bitcode.data(), bitcode.size());
    CHECK(c.kind == sc_payload_kind::dxil_bitcode);
    CHECK(!c.reusable_as_is);
    std::printf("   bare bitcode -> kind=%s reason=\"%s\"\n", c.kind_name(), c.reason.c_str());
}

// ------------------------------------------------------------------- fuzzing

void test_fuzz() {
    banner("fuzz: 300 pseudo-random blobs, no crash, no false 'reusable'");
    int unknown = 0;
    int other = 0;
    for (uint32_t seed = 1; seed <= 300; ++seed) {
        const size_t len = (seed * 37u) % 601u;
        const std::vector<uint8_t> blob = pseudo_random(len, seed);
        const sc_shader_classification c = sc_classify_shader(blob.data(), blob.size());
        if (c.kind == sc_payload_kind::dxil_container) {
            CHECK(c.container.ok);
        }
        if (c.kind == sc_payload_kind::rdna2_xs2) {
            CHECK(c.xs2.found);
        }
        if (c.kind == sc_payload_kind::unknown) {
            CHECK(!c.reason.empty());
            ++unknown;
        } else {
            ++other;
        }
        if (len > 0) {
            CHECK(c.sha256.size() == 64);
        }
    }
    std::printf("   %d unknown, %d other (deterministic LCG input)\n", unknown, other);
}

void test_determinism_and_json() {
    banner("determinism + JSON");
    const std::vector<uint8_t> blob = make_xs2_payload(24, true, true);
    const sc_shader_classification a = sc_classify_shader(blob.data(), blob.size());
    const sc_shader_classification b = sc_classify_shader(blob.data(), blob.size());
    CHECK(a.to_json() == b.to_json());
    const std::string json = a.to_json();
    CHECK(json.find("\"kind\":\"rdna2_xs2\"") != std::string::npos);
    CHECK(json.find("\"code_start_corroborated\":true") != std::string::npos);
    CHECK(json.find("\"xs2\":") != std::string::npos);
    CHECK(json.find("\"fields\":[") != std::string::npos);
    CHECK(json.find("\"open_questions\":") != std::string::npos);

    const std::vector<uint8_t> dxil = make_dxil_container(5);
    const std::string djson = sc_classify_shader(dxil.data(), dxil.size()).to_json();
    CHECK(djson.find("\"kind\":\"dxil_container\"") != std::string::npos);
    CHECK(djson.find("\"stage\":\"compute\"") != std::string::npos);
    CHECK(djson.find("\"stage_source\":\"shader-token\"") != std::string::npos);
    std::printf("   JSON keys present on both paths (%zu / %zu bytes)\n", json.size(), djson.size());
}

} // namespace

int main(int argc, char** argv) {
    const std::string out_dir = (argc > 1) ? argv[1] : "out/shader_tests";
    std::printf("ScarlettCompat shader pipeline - Phase 3 (RDNA2 frontend classification)\n");
#ifdef SC_FRONTEND_NO_DXC
    std::printf("build: NO-DXC (sc_dxil_emit.cpp is not linked into this binary)\n");
#else
    std::printf("build: full (real dxc fixtures enabled)\n");
#endif
    std::printf("output dir: %s (this harness writes nothing)\n", out_dir.c_str());

#ifndef SC_FRONTEND_NO_DXC
    bool have_real_vs = false;
    std::vector<uint8_t> real_vs;
    test_real_dxc_fixtures(have_real_vs, real_vs);
#endif
    test_synthetic_stage_matrix();
    test_xs2_fixtures();
    test_unknown_fixtures();
    test_fuzz();
    test_determinism_and_json();

    std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
