// ScarlettCompat shader pipeline - Phase 2: GPU render check.
// MIT. New file; not wired into the boot path.
//
// WHAT THIS IS
//   A real D3D12 offscreen render of the synthetic triangle, used to prove the
//   emitted DXIL actually creates a pipeline state and rasterizes on this
//   machine's GPU (RX 5700 XT, RDNA1, FL 12_1). It also reports the caps the
//   backend keys off: highest shader model, wave lane count, mesh tier,
//   raytracing tier.
//
//   It creates its own device and its own 64x64 target. It is not the layer,
//   does not touch the guest, and never presents.
#ifndef SC_SHADER_GPU_RENDER_TEST_H
#define SC_SHADER_GPU_RENDER_TEST_H

#include <cstdint>
#include <string>
#include <vector>

namespace sc {

struct sc_gpu_caps {
    bool valid = false;
    std::string adapter_name;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t feature_level = 0; // D3D_FEATURE_LEVEL bit pattern as reported
    uint32_t shader_model_major = 0;
    uint32_t shader_model_minor = 0;
    uint32_t mesh_shader_tier = 0; // D3D12_MESH_SHADER_TIER
    uint32_t raytracing_tier = 0;  // D3D12_RAYTRACING_TIER
    uint32_t wave_lane_min = 0;
    uint32_t wave_lane_max = 0;
    std::string note;
    std::string to_string() const;
};

struct sc_triangle_render_result {
    bool ok = false;
    std::string log;
    std::vector<std::string> errors;
    sc_gpu_caps caps;
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t clear_rgba[4] = {0, 0, 0, 255};
    uint8_t center_rgba[4] = {0, 0, 0, 0};
    uint8_t corner_rgba[4] = {0, 0, 0, 0};
    bool pso_created = false;
    bool drew = false;
};

// Device + adapter discovery only (no rendering).
sc_gpu_caps sc_query_gpu_caps();

// Creates a device, a PSO from the given DXIL, renders a 3-vertex triangle into
// an offscreen R8G8B8A8_UNORM target, and reads two pixels back:
//   center = middle of the target (must be the triangle color)
//   corner = (0,0) (must be the clear color)
// `clear` is RGBA8. On non-Windows builds this returns false with a note.
bool sc_render_synthetic_triangle(const std::vector<uint8_t>& vs_dxil, const std::vector<uint8_t>& ps_dxil,
                                  uint32_t width, uint32_t height, const uint8_t clear[4],
                                  sc_triangle_render_result& out);

// 64x64 convenience wrapper with a black clear color.
bool sc_run_synthetic_triangle_test(const std::vector<uint8_t>& vs_dxil, const std::vector<uint8_t>& ps_dxil,
                                    sc_triangle_render_result& out);

} // namespace sc

#endif // SC_SHADER_GPU_RENDER_TEST_H
