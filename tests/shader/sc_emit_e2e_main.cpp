// ScarlettCompat shader pipeline - end-to-end emit test (MIT).
// Standalone: no game, no guest, no layer, no presentation.
//
//   sc_emit_e2e_test.exe
//
// WHY THIS EXISTS
//   The build gate compiles sc_dxil_emit.cpp but never runs it, and the Phase 2
//   triangle test drives the *backend*, which does its own container validation
//   inside compile(). Nothing proved that the bytes the emitter actually returns
//   are the bytes a real device accepts. This test drives the emitter seam
//   directly and walks the whole chain:
//
//     1. probe dxc.exe and assert the real target device (RX 5700 XT, 0x731f)
//     2. emit VS/PS from the pipeline's own HLSL through sc_dxc_cli_emitter
//     3. parse those exact bytes back with the container reader and assert the
//        format, DXIL chunk, shader-model token, program version and stage
//     4. create a real D3D12 PSO from those bytes and read pixels back
//     5. prove the readback is live (a constant PS and a position-dependent PS
//        must disagree at the same pixel) so a stale target cannot pass
//     6. prove the seam fails loudly: broken HLSL and a wrong profile must
//        return ok=false with a log and no bytecode
//
//   Every shader here is our own synthetic HLSL; no captured bytes are used.
#include "shader/sc_container.h"
#include "shader/sc_dxil_emit.h"
#include "shader/sc_gpu_render_test.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace sc;

namespace {

constexpr uint32_t kTargetVendor = 0x1002; // AMD
constexpr uint32_t kTargetDevice = 0x731f; // Navi 10 / RX 5700 XT

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

std::string pixel_string(const uint8_t px[4]) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "(%u,%u,%u,%u)", px[0], px[1], px[2], px[3]);
    return std::string(buf);
}

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

// The pipeline's own smoke shaders, written out here rather than pulled from the
// backend so this test never exercises sc_dxil_backend's validation or cache.
const char* kTriangleVs =
    "// synthetic triangle: position from SV_VertexID, no vertex buffer\n"
    "struct VSOut { float4 pos : SV_Position; };\n"
    "VSOut main(uint id : SV_VertexID) {\n"
    "    float2 corners[3] = { float2(-0.85, -0.85), float2(0.0, 0.9), float2(0.85, -0.85) };\n"
    "    VSOut o;\n"
    "    o.pos = float4(corners[id], 0.0, 1.0);\n"
    "    return o;\n"
    "}\n";

const char* kSolidGreenPs =
    "// solid green so the readback is unambiguous\n"
    "float4 main() : SV_Target { return float4(0.0, 1.0, 0.0, 1.0); }\n";

// Exactly 7 in UNORM8, i.e. a value the clear color (0) and green (255) cannot
// produce. Used as the readback-liveness probe.
const char* kConstantSevenPs =
    "float4 main() : SV_Target { return float4(7.0 / 255.0, 0.0, 0.0, 1.0); }\n";

// Depends on the interpolated SV_Position: at the centre pixel (32,32) of a 64x64
// target, sv.xy is (32.5, 32.5), so this must read (255,255,0,255). A constant
// shader cannot produce that, and a stale render target cannot either.
const char* kPositionPs =
    "float4 main(float4 sv : SV_Position) : SV_Target {\n"
    "    float r = sv.x > 32.0 ? 1.0 : 0.0;\n"
    "    float g = sv.y > 32.0 ? 1.0 : 0.0;\n"
    "    return float4(r, g, 0.0, 1.0);\n"
    "}\n";

const char* kBrokenPs = "float4 main() : SV_Target { return float4(1, 0, 0, 1) }\n";

} // namespace

int main() {
    std::printf("ScarlettCompat shader pipeline - end-to-end emit test (dxc -> container -> PSO -> readback)\n");

    // ------------------------------------------------------------ 1. hardware
    banner("target device");
    const sc_gpu_caps caps = sc_query_gpu_caps();
    std::printf("   %s\n", caps.to_string().c_str());
    CHECK(caps.valid);
    if (!caps.valid) {
        std::printf("\n%d checks, %d failed (no D3D12 device)\n", g_checks, g_fail);
        return 1;
    }
    // Documented target hardware: RDNA1, FL 12_1, SM 6.7, no mesh shaders, no DXR.
    // Asserted so this test cannot silently pass on a different GPU.
    CHECK(caps.vendor_id == kTargetVendor);
    CHECK(caps.device_id == kTargetDevice);
    CHECK(caps.shader_model_major == 6);
    CHECK(caps.shader_model_minor >= 7);
    CHECK(caps.mesh_shader_tier == 0);
    CHECK(caps.raytracing_tier == 0);
    // The device reports a wave32/wave64 capability range (32..64), not "64 only":
    // RDNA1 can run both, and the compiler chooses. Asserted so the range is not
    // mistaken for a pinned shader property.
    CHECK(caps.wave_lane_min == 32);
    CHECK(caps.wave_lane_max == 64);
    if (caps.device_id != kTargetDevice) {
        std::printf("   note: this test targets device 0x%04x; found 0x%04x\n", kTargetDevice,
                    caps.device_id);
    }

    // -------------------------------------------------------------- 2. emitter
    banner("emitter probe (sc_dxil_emit.cpp, real subprocess)");
    const sc_dxil_backend_info probe = sc_dxil_probe();
    std::printf("   backend: %s available=%d\n", probe.name.c_str(), probe.available ? 1 : 0);
    std::printf("   path   : %s\n", probe.path.c_str());
    std::printf("   version: %s\n", probe.version.c_str());
    CHECK(probe.available);
    if (!probe.available) {
        std::printf("   %s\n", probe.note.c_str());
        std::printf("\n%d checks, %d failed (no dxc.exe)\n", g_checks, g_fail);
        return 1;
    }

    sc_dxc_cli_emitter emitter(probe.path);
    char profile_vs[16];
    char profile_ps[16];
    std::snprintf(profile_vs, sizeof(profile_vs), "vs_%u_%u", caps.shader_model_major,
                  caps.shader_model_minor);
    std::snprintf(profile_ps, sizeof(profile_ps), "ps_%u_%u", caps.shader_model_major,
                  caps.shader_model_minor);

    // ------------------------------------------- 3. emit + parse (vertex stage)
    banner("emit VS through the seam, parse it back with the container reader");
    sc_dxil_request vs_request;
    vs_request.hlsl = kTriangleVs;
    vs_request.entry = "main";
    vs_request.profile = profile_vs;

    sc_dxil_result vs;
    const bool vs_ok = emitter.compile(vs_request, vs);
    CHECK(vs_ok);
    CHECK(vs.ok);
    CHECK(!vs.dxil.empty());
    CHECK(vs.backend == "dxc-cli");
    if (!vs_ok) {
        std::printf("   emit log:\n%s\n", vs.log.c_str());
        std::printf("\n%d checks, %d failed (VS emission)\n", g_checks, g_fail);
        return 1;
    }
    std::printf("   %s: %zu bytes (log %zu bytes)\n", profile_vs, vs.dxil.size(), vs.log.size());

    sc_container_info vs_info;
    const bool vs_parsed = sc_parse_container(vs.dxil.data(), vs.dxil.size(), vs_info);
    CHECK(vs_parsed);
    CHECK(vs_info.ok);
    CHECK(vs_info.format == sc_container_format::dxbc);
    CHECK(vs_info.has_dxil);
    CHECK(vs_info.shader_chunk >= 0);
    CHECK(vs_info.stage == sc_stage::vertex);
    CHECK(vs_info.stage_from == sc_stage_source::shader_token);
    CHECK(vs_info.has_program_version);
    CHECK(vs_info.program_major == caps.shader_model_major);
    CHECK(vs_info.program_minor == caps.shader_model_minor);
    // DXIL chunk token: low 16 bits = shader model nibbles (0x67 == 6.7).
    CHECK((vs_info.shader_token & 0xffffu) ==
          ((caps.shader_model_major << 4) | caps.shader_model_minor));
    CHECK(vs_info.total_size == vs.dxil.size());
    CHECK(contains(vs_info.chunk_list(), "DXIL"));
    CHECK(contains(vs_info.chunk_list(), "PSV0"));
    CHECK(contains(vs_info.chunk_list(), "ISG1"));
    CHECK(vs_info.error.empty()); // no error text on a good container
    std::printf("   chunks : %s\n", vs_info.chunk_list().c_str());
    std::printf("   token  : 0x%08x -> %s %u.%u, declared size %u\n", vs_info.shader_token,
                sc_stage_name(vs_info.stage), vs_info.program_major, vs_info.program_minor,
                vs_info.total_size);

    for (const auto& chunk : vs_info.chunks) {
        std::printf("      %-6s offset=%-6u size=%u\n", chunk.name.c_str(), chunk.offset, chunk.size);
        CHECK(chunk.offset + chunk.size <= vs.dxil.size());
    }

    // Same source twice: the seam itself must be deterministic, or a cache keyed
    // on source text would be lying to the layer.
    sc_dxil_result vs_again;
    CHECK(emitter.compile(vs_request, vs_again));
    CHECK(vs_again.dxil.size() == vs.dxil.size());
    CHECK(vs_again.dxil == vs.dxil);
    std::printf("   re-emit: %zu bytes, byte-identical=%d\n", vs_again.dxil.size(),
                vs_again.dxil == vs.dxil ? 1 : 0);

    // -------------------------------------------- 4. emit + parse (pixel stage)
    banner("emit PS variants through the seam");
    struct ps_case {
        const char* label;
        const char* hlsl;
    };
    const ps_case ps_cases[] = {
        {"solid green", kSolidGreenPs},
        {"constant 7", kConstantSevenPs},
        {"sv_position", kPositionPs},
    };

    std::vector<sc_dxil_result> ps_results;
    for (const auto& c : ps_cases) {
        sc_dxil_request request;
        request.hlsl = c.hlsl;
        request.entry = "main";
        request.profile = profile_ps;
        sc_dxil_result result;
        const bool ok = emitter.compile(request, result);
        CHECK(ok);
        if (!ok) {
            std::printf("   %s: emit failed\n%s\n", c.label, result.log.c_str());
        }
        sc_container_info info;
        const bool parsed = ok && sc_parse_container(result.dxil.data(), result.dxil.size(), info);
        CHECK(parsed);
        CHECK(info.stage == sc_stage::pixel);
        CHECK(info.has_dxil);
        CHECK(contains(info.chunk_list(), "PSV0"));
        CHECK(contains(info.chunk_list(), "OSG1"));
        std::printf("   %-12s %5zu bytes  stage=%s  chunks=%s\n", c.label, result.dxil.size(),
                    sc_stage_name(info.stage), info.chunk_list().c_str());
        ps_results.push_back(result);
    }
    if (ps_results.size() != 3) {
        std::printf("\n%d checks, %d failed (PS emission)\n", g_checks, g_fail);
        return 1;
    }

    // ------------------------------------------------ 5. PSO + readback on GPU
    banner("D3D12 PSO from the seam's own bytes + pixel readback");
    sc_triangle_render_result solid;
    const bool solid_rendered = sc_run_synthetic_triangle_test(vs.dxil, ps_results[0].dxil, solid);
    CHECK(solid_rendered);
    CHECK(solid.pso_created);
    CHECK(solid.drew);
    for (const auto& error : solid.errors) {
        std::printf("   error: %s\n", error.c_str());
    }
    std::printf("   solid green : center=%s corner=%s\n", pixel_string(solid.center_rgba).c_str(),
                pixel_string(solid.corner_rgba).c_str());
    const uint8_t expected_green[4] = {0, 255, 0, 255};
    CHECK(std::memcmp(solid.center_rgba, expected_green, 4) == 0);
    CHECK(std::memcmp(solid.corner_rgba, solid.clear_rgba, 4) == 0);

    // ---- readback liveness: the constant and the position-dependent PS must
    // disagree at the same pixel, or the target was stale and every pixel check
    // above means nothing.
    banner("readback liveness (same pixel, two different shaders)");
    sc_triangle_render_result seven;
    const bool seven_rendered = sc_run_synthetic_triangle_test(vs.dxil, ps_results[1].dxil, seven);
    CHECK(seven_rendered);
    sc_triangle_render_result position;
    const bool position_rendered = sc_run_synthetic_triangle_test(vs.dxil, ps_results[2].dxil, position);
    CHECK(position_rendered);
    std::printf("   constant 7   : center=%s\n", pixel_string(seven.center_rgba).c_str());
    std::printf("   sv_position  : center=%s\n", pixel_string(position.center_rgba).c_str());

    const uint8_t expected_seven[4] = {7, 0, 0, 255};
    CHECK(std::memcmp(seven.center_rgba, expected_seven, 4) == 0);
    const uint8_t expected_position[4] = {255, 255, 0, 255};
    CHECK(std::memcmp(position.center_rgba, expected_position, 4) == 0);
    CHECK(std::memcmp(seven.center_rgba, position.center_rgba, 4) != 0);
    CHECK(std::memcmp(position.corner_rgba, position.clear_rgba, 4) == 0);

    // ------------------------------------------------ 6. the seam fails loudly
    banner("negative cases: the seam refuses instead of handing on junk");
    sc_dxil_request broken_request;
    broken_request.hlsl = kBrokenPs;
    broken_request.entry = "main";
    broken_request.profile = profile_ps;
    sc_dxil_result broken;
    const bool broken_ok = emitter.compile(broken_request, broken);
    CHECK(!broken_ok);
    CHECK(!broken.ok);
    CHECK(broken.dxil.empty());
    CHECK(!broken.log.empty());
    std::printf("   broken HLSL  : ok=%d bytes=%zu log=%zu bytes\n", broken.ok ? 1 : 0, broken.dxil.size(),
                broken.log.size());

    // A pixel shader compiled as a vertex profile must be refused by dxc, so a
    // wrong profile can never reach a PSO unnoticed.
    sc_dxil_request wrong_profile;
    wrong_profile.hlsl = kSolidGreenPs;
    wrong_profile.entry = "main";
    wrong_profile.profile = profile_vs;
    sc_dxil_result wrong;
    const bool wrong_ok = emitter.compile(wrong_profile, wrong);
    CHECK(!wrong_ok);
    CHECK(wrong.dxil.empty());
    std::printf("   PS as %-6s : ok=%d bytes=%zu\n", profile_vs, wrong.ok ? 1 : 0, wrong.dxil.size());

    // An entry point that does not exist must also fail rather than produce an
    // empty-but-successful container.
    sc_dxil_request missing_entry;
    missing_entry.hlsl = kSolidGreenPs;
    missing_entry.entry = "main2";
    missing_entry.profile = profile_ps;
    sc_dxil_result missing;
    const bool missing_ok = emitter.compile(missing_entry, missing);
    CHECK(!missing_ok);
    CHECK(missing.dxil.empty());
    std::printf("   entry main2  : ok=%d bytes=%zu\n", missing.ok ? 1 : 0, missing.dxil.size());

    std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
