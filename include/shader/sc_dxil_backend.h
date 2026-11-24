// ScarlettCompat shader pipeline - Phase 2: standalone DXIL backend.
// MIT. New file; not wired into the boot path.
//
// WHAT THIS IS
//   The device-aware layer around the raw DXIL emitter seam (sc_dxil_emit.h):
//     * picks the DXIL profile from the device's real highest shader model,
//       instead of guessing 6.6/6.7;
//     * compiles HLSL for a stage and *validates the result* (container parses,
//       contains DXIL, and the container's own stage opinion must agree);
//     * caches by content so a repeated shader costs one hash;
//     * ships the pipeline's known-good smoke shader (a synthetic triangle)
//       used by the GPU test.
//
//   It never touches a device itself: caps come in as a value. That keeps the
//   backend usable from the layer (once slots 86/16 land) and testable without
//   a GPU.
#ifndef SC_SHADER_DXIL_BACKEND_H
#define SC_SHADER_DXIL_BACKEND_H

#include "shader/sc_container.h"
#include "shader/sc_dxil_emit.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sc {

struct sc_shader_model {
    uint32_t major = 6;
    uint32_t minor = 6;
    std::string to_string() const;
};

// "vs_6_7" for vertex at SM 6.7; empty when the stage is not reachable at that
// shader model (mesh/amplification need 6.5+). The empty string is the signal
// the caller must skip/fallback, never a silent downgrade.
std::string sc_stage_profile(sc_stage stage, sc_shader_model model);

struct sc_backend_shader {
    bool ok = false;
    sc_stage stage = sc_stage::unknown;
    std::string profile;               // "vs_6_7"
    std::vector<uint8_t> dxil;         // container bytes ready for a PSO
    std::string sha256;                // over the container bytes
    std::string chunks;                // "SFI0,ISG1,OSG1,PSV0,STAT,HASH,DXIL"
    std::string stage_from_container;  // stage name or "unknown"
    bool container_ok = false;
    std::string log;                   // compiler output on failure
};

// HLSL source for the pipeline's synthetic triangle. `stage` must be vertex or
// pixel; returns false for anything else. The vertex shader emits a full
// triangle from SV_VertexID (no input layout) and the pixel shader is solid
// green, so the GPU test can pixel-verify without any vertex buffer.
bool sc_synthetic_triangle_source(sc_stage stage, std::string& out_hlsl);

// Same smoke shaders with an explicit `[WaveSize(n)]` attribute (SM 6.6+), plus a
// trivial compute variant. Used to settle whether wave size can be pinned per
// stage on this hardware/driver before the RDNA2 frontend lifts console shaders
// that assume wave64. Returns false for stages without a probe shader.
bool sc_wave_probe_source(sc_stage stage, uint32_t wave_size, std::string& out_hlsl);

class sc_dxil_backend {
public:
    sc_dxil_backend(std::unique_ptr<sc_dxil_emitter> emitter, sc_shader_model model);

    // Picks the best available emitter (dxc when present) for `model`.
    static std::unique_ptr<sc_dxil_backend> create(sc_shader_model model);

    bool compile(sc_stage stage, const std::string& hlsl, const std::string& entry, sc_backend_shader& out);

    sc_shader_model model() const { return model_; }
    sc_dxil_backend_info info() const { return emitter_ ? emitter_->info() : sc_dxil_backend_info{}; }
    size_t cache_size() const { return cache_.size(); }
    uint64_t cache_hits() const { return cache_hits_; }

private:
    std::unique_ptr<sc_dxil_emitter> emitter_;
    sc_shader_model model_;
    std::unordered_map<std::string, sc_backend_shader> cache_;
    uint64_t cache_hits_ = 0;
};

} // namespace sc

#endif // SC_SHADER_DXIL_BACKEND_H
