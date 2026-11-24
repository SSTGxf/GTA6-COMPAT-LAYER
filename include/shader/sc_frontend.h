// ScarlettCompat shader pipeline - Phase 3: RDNA2 shader frontend.
// MIT. New file; not wired into the boot path.
//
// WHAT THIS IS
//   The single entry point that answers "what did the guest just hand us?" for a
//   shader blob, before anything tries to build a PSO:
//
//     dxil_container   DXBC envelope + DXIL chunk, stage identified from the
//                      DXIL version token -> reusable as-is
//     rdna2_xs2        precompiled RDNA2 microcode -> must be unpacked (Phase 4)
//     unknown          neither of the above (with the reason recorded)
//
//   Two adjacent outcomes are reported distinctly rather than folded into
//   "unknown", because they need different work:
//     dxil_bitcode     bare DXIL bitcode with no container envelope
//     dxbc_sm5_legacy  SM5 DXBC (SHEX/SHDR) that cannot be reused as DXIL
//
// HARD RULES THIS MODULE KEEPS
//   * Stage identity comes from the shader-chunk version token (DXIL token for
//     SM6, SHEX/SHDR token for SM5). Never from ISGN/OSGN presence and never
//     from a caller hint -- there is no stage parameter to pass.
//   * No dependency on DXC. This header and sc_frontend.cpp reference only the
//     container reader, the XS2 prober and SHA-256. HLSL->DXIL emission stays in
//     sc_dxil_backend / sc_dxil_emit, replaceable and optional; the Phase 3
//     harness builds and passes a binary that does not link the emitter at all.
#ifndef SC_SHADER_FRONTEND_H
#define SC_SHADER_FRONTEND_H

#include "shader/sc_container.h"
#include "shader/sc_xs2.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sc {

enum class sc_payload_kind : uint32_t {
    unknown = 0,
    dxil_container = 1,   // reusable as-is (DXIL chunk + token-identified stage)
    rdna2_xs2 = 2,        // RDNA2 microcode: needs unpacking
    dxil_bitcode = 3,     // bare DXIL bitcode: needs an envelope
    dxbc_sm5_legacy = 4,  // SM5 DXBC: needs translation, cannot be reused
};

const char* sc_payload_kind_name(sc_payload_kind kind);

// Everything the caller needs to route a shader, and nothing that restates
// `kind`: "needs unpacking" is `kind == rdna2_xs2`, "needs translation" is
// `kind == dxbc_sm5_legacy`.
struct sc_shader_classification {
    sc_payload_kind kind = sc_payload_kind::unknown;
    // Only true for a DXIL container whose token identified a stage. A parsed
    // container with an unidentifiable stage is dxil_container + false.
    bool reusable_as_is = false;

    sc_stage stage = sc_stage::unknown;
    sc_stage_source stage_source = sc_stage_source::none;
    uint32_t shader_model_major = 0; // 0 when the token carried no shader model
    uint32_t shader_model_minor = 0;

    uint32_t carve_offset = 0; // where the container/payload began inside the blob
    std::string sha256;
    std::string reason;                // one line: why this kind
    std::vector<std::string> evidence; // observed/inferred detail

    // Container metadata (chunks, digest, format). Not a copy of the blob: the
    // caller already owns the bytes that were classified.
    sc_container_info container;
    sc_xs2_discovery xs2;

    const char* kind_name() const { return sc_payload_kind_name(kind); }
    std::string to_json() const;
};

// Classifies one blob. Never throws, never reads outside [data, data+size), and
// works on empty/garbage input (returns unknown with a reason).
sc_shader_classification sc_classify_shader(const void* data, size_t size);

} // namespace sc

#endif // SC_SHADER_FRONTEND_H
