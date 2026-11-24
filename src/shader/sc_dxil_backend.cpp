// ScarlettCompat shader pipeline - Phase 2: standalone DXIL backend (MIT).
#include "shader/sc_dxil_backend.h"

#include "shader/sc_sha256.h"

#include <cstdio>

namespace sc {
namespace {

const char* kTriangleVs =
    "// ScarlettCompat synthetic triangle - known-good pipeline smoke shader.\n"
    "// Full-screen-ish triangle from SV_VertexID; no vertex buffer, no input layout.\n"
    "struct VSOut { float4 pos : SV_Position; };\n"
    "VSOut main(uint id : SV_VertexID) {\n"
    "    float2 corners[3] = { float2(-0.85, -0.85), float2(0.0, 0.9), float2(0.85, -0.85) };\n"
    "    VSOut o;\n"
    "    o.pos = float4(corners[id], 0.0, 1.0);\n"
    "    return o;\n"
    "}\n";

const char* kTrianglePs =
    "// ScarlettCompat synthetic triangle - solid green so readback is unambiguous.\n"
    "float4 main() : SV_Target {\n"
    "    return float4(0.0, 1.0, 0.0, 1.0);\n"
    "}\n";

const char* stage_prefix(sc_stage stage) {
    switch (stage) {
    case sc_stage::vertex: return "vs";
    case sc_stage::hull: return "hs";
    case sc_stage::domain: return "ds";
    case sc_stage::geometry: return "gs";
    case sc_stage::pixel: return "ps";
    case sc_stage::compute: return "cs";
    case sc_stage::mesh: return "ms";
    case sc_stage::amplification: return "as";
    case sc_stage::unknown:
    default: return nullptr;
    }
}

} // namespace

std::string sc_shader_model::to_string() const {
    return std::to_string(major) + "." + std::to_string(minor);
}

std::string sc_stage_profile(sc_stage stage, sc_shader_model model) {
    const char* prefix = stage_prefix(stage);
    if (prefix == nullptr) {
        return std::string();
    }
    if (model.major < 6) {
        return std::string(); // DXIL starts at SM 6.0
    }
    // Mesh and amplification shaders only exist from SM 6.5.
    if ((stage == sc_stage::mesh || stage == sc_stage::amplification) &&
        (model.major == 6 && model.minor < 5)) {
        return std::string();
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s_%u_%u", prefix, model.major, model.minor);
    return std::string(buf);
}

bool sc_synthetic_triangle_source(sc_stage stage, std::string& out_hlsl) {
    switch (stage) {
    case sc_stage::vertex:
        out_hlsl = kTriangleVs;
        return true;
    case sc_stage::pixel:
        out_hlsl = kTrianglePs;
        return true;
    default:
        out_hlsl.clear();
        return false;
    }
}

bool sc_wave_probe_source(sc_stage stage, uint32_t wave_size, std::string& out_hlsl) {
    if (wave_size == 0) {
        out_hlsl.clear();
        return false;
    }
    char buf[1024];
    switch (stage) {
    case sc_stage::vertex:
        std::snprintf(buf, sizeof(buf),
                      "// synthetic triangle, wave size pinned for the probe\n"
                      "struct VSOut { float4 pos : SV_Position; };\n"
                      "[WaveSize(%u)]\n"
                      "VSOut main(uint id : SV_VertexID) {\n"
                      "    float2 corners[3] = { float2(-0.85, -0.85), float2(0.0, 0.9), float2(0.85, -0.85) };\n"
                      "    VSOut o;\n"
                      "    o.pos = float4(corners[id], 0.0, 1.0);\n"
                      "    return o;\n"
                      "}\n",
                      wave_size);
        out_hlsl = buf;
        return true;
    case sc_stage::pixel:
        std::snprintf(buf, sizeof(buf),
                      "// solid green, wave size pinned for the probe\n"
                      "[WaveSize(%u)]\n"
                      "float4 main() : SV_Target { return float4(0.0, 1.0, 0.0, 1.0); }\n",
                      wave_size);
        out_hlsl = buf;
        return true;
    case sc_stage::compute:
        std::snprintf(buf, sizeof(buf),
                      "// trivial compute shader, wave size pinned for the probe\n"
                      "RWStructuredBuffer<uint> g_probe : register(u0);\n"
                      "[WaveSize(%u)]\n"
                      "[numthreads(64, 1, 1)]\n"
                      "void main(uint3 id : SV_DispatchThreadID) {\n"
                      "    if (id.x == 0xffffffffu) g_probe[0] = 1u;\n"
                      "}\n",
                      wave_size);
        out_hlsl = buf;
        return true;
    default:
        out_hlsl.clear();
        return false;
    }
}

sc_dxil_backend::sc_dxil_backend(std::unique_ptr<sc_dxil_emitter> emitter, sc_shader_model model)
    : emitter_(std::move(emitter)), model_(model) {}

std::unique_ptr<sc_dxil_backend> sc_dxil_backend::create(sc_shader_model model) {
    return std::unique_ptr<sc_dxil_backend>(new sc_dxil_backend(sc_make_dxil_emitter(), model));
}

bool sc_dxil_backend::compile(sc_stage stage, const std::string& hlsl, const std::string& entry,
                              sc_backend_shader& out) {
    out = sc_backend_shader{};
    out.stage = stage;

    const std::string profile = sc_stage_profile(stage, model_);
    if (profile.empty()) {
        out.log = std::string("no DXIL profile for stage '") + sc_stage_name(stage) + "' at shader model " +
                  model_.to_string();
        return false;
    }
    out.profile = profile;

    // Cache key: everything that can change the emitted bytes.
    std::string key_material = profile;
    key_material += '\n';
    key_material += entry;
    key_material += '\n';
    key_material += hlsl;
    const std::string key = sc_sha256_hex(key_material.data(), key_material.size());
    const auto hit = cache_.find(key);
    if (hit != cache_.end()) {
        out = hit->second;
        ++cache_hits_;
        return out.ok;
    }

    sc_dxil_request request;
    request.hlsl = hlsl;
    request.entry = entry;
    request.profile = profile;
    // RDNA1 has no 16-bit float acceleration worth targeting and no mesh tier;
    // no extra flags are forced here. Per-shader flags (e.g. wave-size control
    // for a shader that assumes wave64) belong in request.extra_args.
    sc_dxil_result result;
    const bool emitted = emitter_ && emitter_->compile(request, result);
    out.log = result.log;
    if (!emitted) {
        cache_[key] = out;
        return false;
    }

    out.dxil = result.dxil;
    out.sha256 = sc_sha256_hex(out.dxil.data(), out.dxil.size());

    sc_container_info info;
    if (!sc_parse_container(out.dxil.data(), out.dxil.size(), info)) {
        out.log += "\ncontainer parse failed after emission: " + info.error;
        cache_[key] = out;
        return false;
    }
    out.container_ok = true;
    out.chunks = info.chunk_list();
    out.stage_from_container = sc_stage_name(info.stage);

    if (!info.has_dxil) {
        out.log += "\nemitted container has no DXIL chunk (chunks: " + out.chunks + ")";
        cache_[key] = out;
        return false;
    }
    if (info.stage != sc_stage::unknown && info.stage != stage) {
        // Wrong profile would land here; never hand a mismatched shader on.
        out.log += std::string("\ncontainer stage '") + sc_stage_name(info.stage) + "' does not match requested '" +
                   sc_stage_name(stage) + "'";
        cache_[key] = out;
        return false;
    }

    out.ok = true;
    cache_[key] = out;
    return true;
}

} // namespace sc
