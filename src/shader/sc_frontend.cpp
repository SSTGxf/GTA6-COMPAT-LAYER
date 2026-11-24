// ScarlettCompat shader pipeline - Phase 3: frontend classification (MIT).
//
// Deliberately no include of sc_dxil_emit.h / sc_dxil_backend.h: classification
// is pure byte inspection, so the frontend keeps working with no DXC present.
#include "shader/sc_frontend.h"

#include "shader/sc_sha256.h"

#include <cstdio>
#include <cstring>

namespace sc {
namespace {

constexpr size_t kCarveSearchLimit = 4096; // how far into a blob to look for 'DXBC'

void json_escape(std::string& out, const std::string& s) {
    out.push_back('"');
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(ch);
            }
            break;
        }
    }
    out.push_back('"');
}

std::string json_string_array(const std::vector<std::string>& values) {
    std::string out = "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        json_escape(out, values[i]);
    }
    out.push_back(']');
    return out;
}

// Largest chunk payload: the XS2 candidate when a DXBC envelope carries no
// shader chunk we recognize. Inferred rule -- a real corpus may pin the fourcc.
int largest_chunk_index(const sc_container_info& info) {
    int best = -1;
    uint32_t best_size = 0;
    for (size_t i = 0; i < info.chunks.size(); ++i) {
        if (info.chunks[i].size > best_size) {
            best_size = info.chunks[i].size;
            best = static_cast<int>(i);
        }
    }
    return best;
}

} // namespace

const char* sc_payload_kind_name(sc_payload_kind kind) {
    switch (kind) {
    case sc_payload_kind::dxil_container: return "dxil_container";
    case sc_payload_kind::rdna2_xs2: return "rdna2_xs2";
    case sc_payload_kind::dxil_bitcode: return "dxil_bitcode";
    case sc_payload_kind::dxbc_sm5_legacy: return "dxbc_sm5_legacy";
    case sc_payload_kind::unknown:
    default: return "unknown";
    }
}

std::string sc_shader_classification::to_json() const {
    std::string s = "{";
    s += "\"kind\":";
    json_escape(s, sc_payload_kind_name(kind));
    s += ",\"reusable_as_is\":";
    s += reusable_as_is ? "true" : "false";
    s += ",\"stage\":";
    json_escape(s, sc_stage_name(stage));
    s += ",\"stage_source\":";
    json_escape(s, sc_stage_source_name(stage_source));
    s += ",\"shader_model\":\"" + std::to_string(shader_model_major) + "." + std::to_string(shader_model_minor) + "\"";
    s += ",\"carve_offset\":" + std::to_string(carve_offset);
    s += ",\"sha256\":";
    json_escape(s, sha256);
    s += ",\"reason\":";
    json_escape(s, reason);
    s += ",\"evidence\":" + json_string_array(evidence);
    if (container.ok) {
        s += ",\"container\":" + sc_container_info_json(container);
    }
    if (xs2.found || !xs2.terminator_offsets.empty() || !xs2.open_questions.empty()) {
        s += ",\"xs2\":" + xs2.to_json();
    }
    s.push_back('}');
    return s;
}

sc_shader_classification sc_classify_shader(const void* data, size_t size) {
    sc_shader_classification c;
    if (data == nullptr || size == 0) {
        c.reason = "empty blob";
        c.evidence.push_back("observed: zero bytes");
        return c;
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    c.sha256 = sc_sha256_hex(data, size);

    // ---- 1. shader container, at offset 0 or carved from a decorated blob -----
    sc_container_info info;
    bool parsed = sc_parse_container(bytes, size, info);
    uint32_t carve = 0;
    if (!parsed) {
        size_t offset = 0;
        if (sc_locate_container(bytes, size, kCarveSearchLimit, offset)) {
            if (sc_parse_container(bytes + offset, size - offset, info)) {
                parsed = true;
                carve = static_cast<uint32_t>(offset);
                c.evidence.push_back("observed: DXBC envelope carved at offset " + std::to_string(carve));
            }
        }
    }

    if (parsed) {
        c.container = info;
        c.carve_offset = carve;
        c.stage = info.stage;
        c.stage_source = info.stage_from;
        c.shader_model_major = info.program_major;
        c.shader_model_minor = info.program_minor;
        c.evidence.push_back("observed: container chunks = " + info.chunk_list());

        // The reader accepts a bare bitcode blob as its own format; report it as
        // such (no envelope, nothing to build a PSO from yet).
        if (info.format == sc_container_format::dxil_bitcode) {
            c.kind = sc_payload_kind::dxil_bitcode;
            c.reason = "bare DXIL bitcode (no DXBC envelope); needs an envelope before it can be a PSO";
            c.evidence.push_back("observed: LLVM bitcode magic BC C0 DE at offset 0");
            return c;
        }

        if (info.has_dxil) {
            c.kind = sc_payload_kind::dxil_container;
            if (info.stage_from == sc_stage_source::shader_token && info.stage != sc_stage::unknown) {
                c.reusable_as_is = true;
                c.reason = "DXIL chunk present; stage '" + std::string(sc_stage_name(info.stage)) +
                           "' read from the DXIL version token -> usable as a PSO bytecode as-is";
            } else {
                c.reason = "DXIL chunk present but the version token did not identify a stage; "
                           "refusing to call it reusable";
            }
            return c;
        }

        if (info.has_shex) {
            c.kind = sc_payload_kind::dxbc_sm5_legacy;
            c.reason = "SM5 DXBC (SHEX/SHDR); needs translation to DXIL, not reusable and not XS2";
            return c;
        }

        // A container we can read whose chunks are none of the above: the
        // microcode may sit in a bare chunk (inferred rule, see the handoff).
        const int idx = largest_chunk_index(info);
        if (idx >= 0) {
            const sc_chunk& chunk = info.chunks[static_cast<size_t>(idx)];
            c.xs2 = sc_xs2_discover(bytes, size, chunk.offset + 8, chunk.size);
            c.xs2.envelope = "dxbc-chunk:" + chunk.name;
            if (c.xs2.found) {
                c.kind = sc_payload_kind::rdna2_xs2;
                c.reason = "no DXIL/SHEX chunk, but chunk '" + chunk.name +
                           "' holds an s_endpgm-terminated microcode payload -> XS2, needs unpacking";
                return c;
            }
        }
        c.reason = "DXBC container with neither a DXIL/SHEX shader chunk nor recognizable XS2 microcode";
        return c;
    }

    if (!info.error.empty()) {
        c.evidence.push_back("observed: container parse failed: " + info.error);
    }

    // ---- 2. raw RDNA2 microcode -----------------------------------------------
    c.xs2 = sc_xs2_discover(bytes, size, 0, 0);
    if (c.xs2.found) {
        c.kind = sc_payload_kind::rdna2_xs2;
        c.reason = "no DXBC/DXIL container, but an s_endpgm-terminated microcode payload was found -> RDNA2 XS2";
        return c;
    }

    c.reason = "unrecognized payload: no DXBC container, no DXIL bitcode, no XS2 microcode evidence";
    return c;
}

} // namespace sc
