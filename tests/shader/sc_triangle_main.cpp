// ScarlettCompat shader pipeline - Phase 2 runner.
// MIT. Standalone: no game, no guest, no layer, no presentation.
//
//   sc_triangle_test.exe [output-dir]
//
// Proves the DXIL backend end to end on this machine's GPU:
//   1. query the real device caps (highest shader model, wave lanes, mesh/RT tiers)
//   2. emit DXIL for the synthetic triangle through sc_dxil_backend
//   3. create a real D3D12 PSO from that DXIL and render it offscreen
//   4. read the pixels back: center must be the triangle color, corner the clear color
//   5. record the same shaders through the Phase 0 capture path and dump the corpus
#include "shader/sc_capture.h"
#include "shader/sc_container.h"
#include "shader/sc_dxil_backend.h"
#include "shader/sc_gpu_render_test.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
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

std::string pixel_string(const uint8_t px[4]) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "(%u,%u,%u,%u)", px[0], px[1], px[2], px[3]);
    return std::string(buf);
}

} // namespace

int main(int argc, char** argv) {
    const std::string out_dir = (argc > 1) ? argv[1] : "out/shader_tests";
    std::printf("ScarlettCompat shader pipeline - Phase 2 (DXIL backend -> synthetic triangle)\n");
    std::printf("output dir: %s\n", out_dir.c_str());
    sc_make_dirs(out_dir);

    // ---------------------------------------------------------------- 1. caps
    banner("device caps");
    const sc_gpu_caps caps = sc_query_gpu_caps();
    std::printf("   %s\n", caps.to_string().c_str());
    CHECK(caps.valid);
    if (!caps.valid) {
        std::printf("\n%d checks, %d failed (no D3D12 device)\n", g_checks, g_fail);
        return 1;
    }
    CHECK(caps.shader_model_major >= 6);
    // RDNA1 is a native wave64 part: the compiler must be able to pick 64-wide.
    CHECK(caps.wave_lane_max >= 64);
    // Documented target-hardware facts, asserted so a different GPU cannot pass silently.
    CHECK(caps.mesh_shader_tier == 0);
    CHECK(caps.raytracing_tier == 0);

    // ------------------------------------------------------------- 2. backend
    banner("dxil backend");
    const sc_shader_model model{caps.shader_model_major, caps.shader_model_minor};
    std::unique_ptr<sc_dxil_backend> backend = sc_dxil_backend::create(model);
    const sc_dxil_backend_info emitter_info = backend->info();
    std::printf("   emitter: %s available=%d\n", emitter_info.name.c_str(), emitter_info.available ? 1 : 0);
    std::printf("   dxc    : %s\n", emitter_info.path.c_str());
    std::printf("   version: %s\n", emitter_info.version.c_str());
    std::printf("   model  : SM %s  -> profiles %s / %s\n", model.to_string().c_str(),
                sc_stage_profile(sc_stage::vertex, model).c_str(),
                sc_stage_profile(sc_stage::pixel, model).c_str());
    CHECK(emitter_info.available);
    CHECK(sc_stage_profile(sc_stage::vertex, model) == "vs_" + std::to_string(model.major) + "_" +
                                                          std::to_string(model.minor));

    std::string vs_hlsl;
    std::string ps_hlsl;
    CHECK(sc_synthetic_triangle_source(sc_stage::vertex, vs_hlsl));
    CHECK(sc_synthetic_triangle_source(sc_stage::pixel, ps_hlsl));

    sc_backend_shader vs;
    sc_backend_shader ps;
    const bool vs_ok = backend->compile(sc_stage::vertex, vs_hlsl, "main", vs);
    CHECK(vs_ok);
    if (!vs_ok) {
        std::printf("   VS compile log:\n%s\n", vs.log.c_str());
    }
    const bool ps_ok = backend->compile(sc_stage::pixel, ps_hlsl, "main", ps);
    CHECK(ps_ok);
    if (!ps_ok) {
        std::printf("   PS compile log:\n%s\n", ps.log.c_str());
    }
    if (!vs_ok || !ps_ok) {
        std::printf("\n%d checks, %d failed (DXIL emission)\n", g_checks, g_fail);
        return 1;
    }

    CHECK(vs.container_ok);
    CHECK(ps.container_ok);
    CHECK(vs.stage == sc_stage::vertex);
    CHECK(ps.stage == sc_stage::pixel);
    CHECK(vs.stage_from_container == "vertex");
    CHECK(ps.stage_from_container == "pixel");
    std::printf("   VS: %zu bytes  profile=%s  chunks=\"%s\"\n", vs.dxil.size(), vs.profile.c_str(),
                vs.chunks.c_str());
    std::printf("       sha256=%s\n", vs.sha256.c_str());
    std::printf("   PS: %zu bytes  profile=%s  chunks=\"%s\"\n", ps.dxil.size(), ps.profile.c_str(),
                ps.chunks.c_str());
    std::printf("       sha256=%s\n", ps.sha256.c_str());

    // Same source twice must hit the backend cache, not the compiler.
    sc_backend_shader vs_again;
    CHECK(backend->compile(sc_stage::vertex, vs_hlsl, "main", vs_again));
    CHECK(vs_again.sha256 == vs.sha256);
    CHECK(backend->cache_hits() == 1);
    std::printf("   cache: %zu entries, %llu hits\n", backend->cache_size(),
                static_cast<unsigned long long>(backend->cache_hits()));

    // --------------------------------------------------------------- 3. render
    banner("gpu: PSO + offscreen triangle + readback");
    sc_triangle_render_result render;
    const bool rendered = sc_run_synthetic_triangle_test(vs.dxil, ps.dxil, render);
    std::printf("   target: %ux%u R8G8B8A8_UNORM, clear %s\n", render.width, render.height,
                pixel_string(render.clear_rgba).c_str());
    std::printf("   pso_created=%d drew=%d\n", render.pso_created ? 1 : 0, render.drew ? 1 : 0);
    std::printf("   center=%s  corner=%s\n", pixel_string(render.center_rgba).c_str(),
                pixel_string(render.corner_rgba).c_str());
    CHECK(rendered);
    for (const auto& error : render.errors) {
        std::printf("   error: %s\n", error.c_str());
    }
    CHECK(render.pso_created);
    CHECK(render.drew);

    const uint8_t expected_center[4] = {0, 255, 0, 255};
    CHECK(std::memcmp(render.center_rgba, expected_center, 4) == 0);
    CHECK(std::memcmp(render.corner_rgba, render.clear_rgba, 4) == 0);

    const bool triangle_visible = std::memcmp(render.center_rgba, expected_center, 4) == 0;
    const bool corner_clear = std::memcmp(render.corner_rgba, render.clear_rgba, 4) == 0;
    std::printf("   verdict: triangle visible=%d, corner untouched=%d\n", triangle_visible ? 1 : 0,
                corner_clear ? 1 : 0);

    // ------------------------------------------------- 3b. wave-size policy
    banner("wave size policy (console shaders assume wave64)");
    std::printf("   device wave lanes: %u..%u -> an unannotated shader may compile to wave32\n",
                caps.wave_lane_min, caps.wave_lane_max);
    const bool sm66_or_newer =
        (model.major > 6) || (model.major == 6 && model.minor >= 6);
    CHECK(sm66_or_newer);

    std::string probe_hlsl;
    sc_backend_shader cs_wave64;
    CHECK(sc_wave_probe_source(sc_stage::compute, 64, probe_hlsl));
    const bool cs_wave_ok = backend->compile(sc_stage::compute, probe_hlsl, "main", cs_wave64);
    std::printf("   compute [WaveSize(64)]: %s\n", cs_wave_ok ? "accepted" : "rejected");
    if (!cs_wave_ok) {
        std::printf("   log: %s\n", cs_wave64.log.c_str());
    }
    CHECK(cs_wave_ok);

    // Graphics stages: dxc rejects the attribute ("only allowed on compute or
    // node shaders"), so wave size for VS/PS is chosen by the driver. Recorded
    // as a finding rather than a failure -- it is a hardware/compiler fact, not
    // our bug. If a future dxc accepts it, the render below proves it works.
    sc_backend_shader vs_wave64;
    CHECK(sc_wave_probe_source(sc_stage::vertex, 64, probe_hlsl));
    const bool vs_wave_ok = backend->compile(sc_stage::vertex, probe_hlsl, "main", vs_wave64);
    std::printf("   vertex  [WaveSize(64)]: %s\n", vs_wave_ok ? "accepted" : "rejected (driver-chosen wave size)");
    if (!vs_wave_ok) {
        std::printf("   dxc says: %s\n", vs_wave64.log.c_str());
    } else {
        // A pinned wave64 graphics PSO must still rasterize identically.
        sc_triangle_render_result wave_render;
        const bool wave_rendered = sc_run_synthetic_triangle_test(vs_wave64.dxil, ps.dxil, wave_render);
        CHECK(wave_rendered);
        CHECK(std::memcmp(wave_render.center_rgba, expected_center, 4) == 0);
        std::printf("   wave64 PSO rendered: center=%s\n", pixel_string(wave_render.center_rgba).c_str());
    }

    // ------------------------------------ 3c. measure the graphics wave size
    banner("measured graphics wave size (driver-selected, cannot be pinned)");
    const std::string lane_hlsl =
        "// Encodes WaveGetLaneCount()/255 into red so UNORM8 readback recovers it exactly.\n"
        "float4 main() : SV_Target {\n"
        "    uint lanes = WaveGetLaneCount();\n"
        "    return float4((float)lanes / 255.0, 0.0, 0.0, 1.0);\n"
        "}\n";
    sc_backend_shader lane_ps;
    const bool lane_ps_ok = backend->compile(sc_stage::pixel, lane_hlsl, "main", lane_ps);
    CHECK(lane_ps_ok);
    if (lane_ps_ok) {
        sc_triangle_render_result lane_render;
        const bool lane_rendered = sc_run_synthetic_triangle_test(vs.dxil, lane_ps.dxil, lane_render);
        CHECK(lane_rendered);
        const uint32_t observed_lanes = lane_render.center_rgba[0];
        std::printf("   pixel shader observed wave lanes = %u (center=%s)\n", observed_lanes,
                    pixel_string(lane_render.center_rgba).c_str());
        CHECK(observed_lanes == 32 || observed_lanes == 64);
    }

    // -------------------------------------------------------------- 4. capture
    banner("capture: record the same PSO through the Phase 0 path");
    sc_capture_pso_desc desc;
    desc.source_slot = 86;
    desc.debug_name = "synthetic-triangle";
    desc.primitive_topology_type = 3; // D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE
    desc.sample_count = 1;
    desc.rtv_formats.push_back(28); // R8G8B8A8_UNORM
    desc.render_targets.push_back(sc_capture_render_target_state{});

    sc_capture_stage_blob vs_blob;
    vs_blob.stage = sc_stage::vertex;
    vs_blob.bytes = vs.dxil;
    desc.shaders.push_back(vs_blob);

    sc_capture_stage_blob ps_blob;
    ps_blob.stage = sc_stage::pixel;
    ps_blob.bytes = ps.dxil;
    desc.shaders.push_back(ps_blob);

    sc_capture_store store;
    const sc_capture_add_result add = store.add(desc);
    CHECK(add.added);
    const std::string corpus_dir = out_dir + "/triangle_corpus";
    const sc_capture_dump_report dump = sc_capture_dump(store, corpus_dir);
    CHECK(dump.ok);
    CHECK(dump.records == 1);
    std::printf("   signature=%s  stages=\"%s\"\n", store.records()[0].signature_hex().c_str(),
                store.records()[0].stages.c_str());
    std::printf("   wrote %u files, %llu bytes under %s\n", dump.files,
                static_cast<unsigned long long>(dump.bytes), corpus_dir.c_str());
    for (const auto& error : dump.errors) {
        std::printf("   dump error: %s\n", error.c_str());
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
