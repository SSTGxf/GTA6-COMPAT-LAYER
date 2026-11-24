// ScarlettCompat shader pipeline - shader container inspection (MIT, clean-room).
#include "shader/sc_container.h"

#include "shader/sc_sha256.h"

#include <cstdio>
#include <cstring>

namespace sc {
namespace {

constexpr uint32_t kMaxChunks = 4096;
constexpr uint32_t kHeaderSize = 32;

inline uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline bool is_printable(uint8_t c) { return c >= 0x20 && c < 0x7f; }

std::string fourcc_text(uint32_t cc) {
    const uint8_t b[4] = {static_cast<uint8_t>(cc), static_cast<uint8_t>(cc >> 8),
                          static_cast<uint8_t>(cc >> 16), static_cast<uint8_t>(cc >> 24)};
    std::string s;
    for (int i = 0; i < 4; ++i) {
        if (is_printable(b[i])) {
            s.push_back(static_cast<char>(b[i]));
        } else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", b[i]);
            s += buf;
        }
    }
    return s;
}

// Chunk fourccs appear in file order in practice ("DXIL", "PSV0", ...), but some
// producers store them byte-reversed. Accept either orientation.
bool cc_is(uint32_t cc, char a, char b, char c, char d) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&cc);
    if (p[0] == static_cast<uint8_t>(a) && p[1] == static_cast<uint8_t>(b) &&
        p[2] == static_cast<uint8_t>(c) && p[3] == static_cast<uint8_t>(d)) {
        return true;
    }
    return p[0] == static_cast<uint8_t>(d) && p[1] == static_cast<uint8_t>(c) &&
           p[2] == static_cast<uint8_t>(b) && p[3] == static_cast<uint8_t>(a);
}

// DXIL::ShaderKind, which is what the DXIL chunk version token actually stores
// (measured against dxc 1.9: ps=0 vs=1 gs=2 hs=3 ds=4 cs=5, mesh=13,
// amplification=14). It is NOT D3D12_SHADER_VERSION_TYPE: that enum has mesh at
// 6, which is where DXIL puts the shader *library* kind, and a real mesh
// container reports 13. Kinds 6..12 (library, raygen, intersection, anyhit,
// closesthit, miss, callable) are not pipeline stages we accept, so they map to
// unknown deliberately.
sc_stage stage_from_dxil_shader_kind(uint32_t v) {
    switch (v) {
    case 0: return sc_stage::pixel;
    case 1: return sc_stage::vertex;
    case 2: return sc_stage::geometry;
    case 3: return sc_stage::hull;
    case 4: return sc_stage::domain;
    case 5: return sc_stage::compute;
    case 13: return sc_stage::mesh;
    case 14: return sc_stage::amplification;
    default: return sc_stage::unknown;
    }
}

// SM5 tokenized program type (low 16 bits of the first dword of SHEX/SHDR).
sc_stage stage_from_program_token(uint32_t token) {
    switch (token & 0xffffu) {
    case 0: return sc_stage::pixel;
    case 1: return sc_stage::vertex;
    case 2: return sc_stage::geometry;
    case 3: return sc_stage::hull;
    case 4: return sc_stage::domain;
    case 5: return sc_stage::compute;
    default: return sc_stage::unknown;
    }
}

} // namespace

const char* sc_stage_name(sc_stage s) {
    switch (s) {
    case sc_stage::vertex: return "vertex";
    case sc_stage::hull: return "hull";
    case sc_stage::domain: return "domain";
    case sc_stage::geometry: return "geometry";
    case sc_stage::pixel: return "pixel";
    case sc_stage::compute: return "compute";
    case sc_stage::mesh: return "mesh";
    case sc_stage::amplification: return "amplification";
    case sc_stage::unknown:
    default: return "unknown";
    }
}

bool sc_stage_is_graphics(sc_stage s) {
    return s == sc_stage::vertex || s == sc_stage::hull || s == sc_stage::domain || s == sc_stage::geometry ||
           s == sc_stage::pixel || s == sc_stage::mesh || s == sc_stage::amplification;
}

const char* sc_stage_tag(sc_stage s) {
    switch (s) {
    case sc_stage::vertex: return "vs";
    case sc_stage::hull: return "hs";
    case sc_stage::domain: return "ds";
    case sc_stage::geometry: return "gs";
    case sc_stage::pixel: return "ps";
    case sc_stage::compute: return "cs";
    case sc_stage::mesh: return "ms";
    case sc_stage::amplification: return "as";
    case sc_stage::unknown:
    default: return "xx";
    }
}

const char* sc_container_format_name(sc_container_format f) {
    switch (f) {
    case sc_container_format::dxbc: return "dxbc+dxil";
    case sc_container_format::dxbc_sm5: return "dxbc+sm5";
    case sc_container_format::dxil_bitcode: return "dxil-bitcode";
    case sc_container_format::unknown:
    default: return "unknown";
    }
}

const char* sc_stage_source_name(sc_stage_source s) {
    switch (s) {
    case sc_stage_source::shader_token: return "shader-token";
    case sc_stage_source::none:
    default: return "none";
    }
}

std::string sc_container_info::chunk_list() const {
    std::string s;
    for (size_t i = 0; i < chunks.size(); ++i) {
        if (i != 0) {
            s.push_back(',');
        }
        s += chunks[i].name;
    }
    return s;
}

bool sc_locate_container(const void* data, size_t size, size_t search_limit, size_t& offset) {
    offset = 0;
    if (data == nullptr || size < kHeaderSize) {
        return false;
    }
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t limit = size - kHeaderSize;
    if (search_limit != 0 && search_limit < limit) {
        limit = search_limit;
    }
    for (size_t i = 0; i <= limit; ++i) {
        if (p[i] == 'D' && p[i + 1] == 'X' && p[i + 2] == 'B' && p[i + 3] == 'C') {
            const uint32_t declared = rd32(p + i + 24);
            const uint32_t count = rd32(p + i + 28);
            if (declared >= kHeaderSize && declared <= size - i && count <= kMaxChunks) {
                offset = i;
                return true;
            }
        }
    }
    return false;
}

bool sc_parse_container(const void* data, size_t size, sc_container_info& out) {
    out = sc_container_info{};
    if (data == nullptr || size == 0) {
        out.error = "empty blob";
        return false;
    }
    const uint8_t* p = static_cast<const uint8_t*>(data);

    // Raw DXIL bitcode (LLVM wrapper magic 'BC\xC0\xDE'). Checked before the
    // header-size floor: a bare bitcode blob is legitimately shorter than a
    // container header.
    if (size >= 4 && p[0] == 'B' && p[1] == 'C' && p[2] == 0xc0 && p[3] == 0xde) {
        out.ok = true;
        out.format = sc_container_format::dxil_bitcode;
        out.total_size = static_cast<uint32_t>(size);
        out.stage_from = sc_stage_source::none;
        return true;
    }

    if (size < kHeaderSize) {
        out.error = "blob smaller than a container header";
        return false;
    }

    if (!(p[0] == 'D' && p[1] == 'X' && p[2] == 'B' && p[3] == 'C')) {
        // Not a container we know. Locate one inside if the blob is decorated.
        size_t off = 0;
        if (sc_locate_container(data, size, 4096, off)) {
            out.error = "container not at offset 0 (found at " + std::to_string(off) + "; use sc_locate_container)";
        } else {
            out.error = "no DXBC magic";
        }
        return false;
    }

    out.format = sc_container_format::dxbc;
    std::memcpy(out.digest, p + 4, sizeof(out.digest));
    out.header_version = rd32(p + 20);
    out.total_size = rd32(p + 24);
    out.chunk_count = rd32(p + 28);

    if (out.total_size < kHeaderSize) {
        out.error = "declared total size below header size";
        return false;
    }
    if (out.total_size > size) {
        out.error = "truncated container (declared " + std::to_string(out.total_size) + " > available " +
                    std::to_string(size) + ")";
        return false;
    }
    if (out.chunk_count == 0) {
        out.error = "container declares zero chunks";
        return false;
    }
    if (out.chunk_count > kMaxChunks) {
        out.error = "implausible chunk count " + std::to_string(out.chunk_count);
        return false;
    }
    if (static_cast<uint64_t>(kHeaderSize) + static_cast<uint64_t>(out.chunk_count) * 4ull > out.total_size) {
        out.error = "chunk offset table exceeds container";
        return false;
    }

    out.chunks.reserve(out.chunk_count);
    for (uint32_t i = 0; i < out.chunk_count; ++i) {
        const uint32_t off = rd32(p + kHeaderSize + i * 4);
        if (off < kHeaderSize || off > out.total_size || out.total_size - off < 8) {
            out.error = "chunk " + std::to_string(i) + " header out of range";
            return false;
        }
        sc_chunk c;
        c.fourcc = rd32(p + off);
        c.size = rd32(p + off + 4);
        c.offset = off;
        if (c.size > out.total_size - off - 8) {
            out.error = "chunk " + std::to_string(i) + " payload out of range";
            return false;
        }
        c.name = fourcc_text(c.fourcc);
        out.chunks.push_back(c);

        const uint8_t* payload = p + off + 8;
        const int idx = static_cast<int>(i);
        if (cc_is(c.fourcc, 'D', 'X', 'I', 'L')) {
            out.has_dxil = true;
            out.shader_chunk = idx;
            out.format = sc_container_format::dxbc;
            // DXIL chunk version token (verified against dxc 1.9 output for
            // vs/ps/gs/hs/ds/cs/ms): bits 0-15 = shader model (0x67 == 6.7),
            // bits 16-23 = DXIL::ShaderKind stage, bits 24-31 reserved. This is
            // the only authoritative stage source in a DXIL container.
            if (c.size >= 4) {
                out.shader_token = rd32(payload);
                const uint32_t sm = out.shader_token & 0xffffu;
                out.program_major = static_cast<uint16_t>((sm >> 4) & 0xfu);
                out.program_minor = static_cast<uint16_t>(sm & 0xfu);
                out.has_program_version = true;
                const sc_stage s = stage_from_dxil_shader_kind((out.shader_token >> 16) & 0xffu);
                if (s != sc_stage::unknown) {
                    out.stage = s;
                    out.stage_from = sc_stage_source::shader_token;
                }
            }
        } else if (cc_is(c.fourcc, 'S', 'H', 'E', 'X') || cc_is(c.fourcc, 'S', 'H', 'D', 'R')) {
            out.has_shex = true;
            out.shader_chunk = idx;
            out.format = sc_container_format::dxbc_sm5;
            if (c.size >= 4) {
                out.shader_token = rd32(payload);
                out.program_major = static_cast<uint16_t>((out.shader_token >> 20) & 0xfu);
                out.program_minor = static_cast<uint16_t>((out.shader_token >> 24) & 0xfu);
                out.has_program_version = true;
            }
        } else if (cc_is(c.fourcc, 'R', 'T', 'S', '0')) {
            out.has_root_signature = true;
        }
    }

    // Stage identification. The DXIL token was consumed above. SM5 containers
    // fall back to the SHEX/SHDR program token. PSV0 is deliberately NOT used:
    // measured against dxc 1.9, its dword 1 is 1 for a vertex shader but 0 for
    // pixel and compute and 3 for geometry, so it is not a stage field. There
    // is also no ISGN/OSGN heuristic any more -- compute containers ship
    // ISG1/OSG1 too, so signature presence proves nothing.
    if (out.stage == sc_stage::unknown && out.has_shex && out.has_program_version) {
        const sc_stage s = stage_from_program_token(out.shader_token);
        if (s != sc_stage::unknown) {
            out.stage = s;
            out.stage_from = sc_stage_source::shader_token;
        }
    }
    out.ok = true;
    return true;
}

namespace {

void json_escape_into(std::string& out, const std::string& s) {
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

} // namespace

std::string sc_container_info_json(const sc_container_info& info) {
    std::string s = "{";
    s += "\"ok\":";
    s += info.ok ? "true" : "false";
    if (!info.ok) {
        s += ",\"error\":";
        json_escape_into(s, info.error);
        s += "}";
        return s;
    }
    s += ",\"format\":";
    json_escape_into(s, sc_container_format_name(info.format));
    s += ",\"total_size\":" + std::to_string(info.total_size);
    s += ",\"chunk_count\":" + std::to_string(info.chunk_count);
    s += ",\"digest\":";
    json_escape_into(s, sc_hex(info.digest, sizeof(info.digest)));
    s += ",\"stage\":";
    json_escape_into(s, sc_stage_name(info.stage));
    s += ",\"stage_from\":";
    json_escape_into(s, sc_stage_source_name(info.stage_from));
    if (info.has_program_version) {
        s += ",\"program_version\":\"" + std::to_string(info.program_major) + "." +
             std::to_string(info.program_minor) + "\"";
    }
    s += ",\"has_dxil\":";
    s += info.has_dxil ? "true" : "false";
    s += ",\"has_root_signature\":";
    s += info.has_root_signature ? "true" : "false";
    s += ",\"chunks\":[";
    for (size_t i = 0; i < info.chunks.size(); ++i) {
        if (i != 0) {
            s.push_back(',');
        }
        s += "{\"name\":";
        json_escape_into(s, info.chunks[i].name);
        s += ",\"size\":" + std::to_string(info.chunks[i].size);
        s += ",\"offset\":" + std::to_string(info.chunks[i].offset);
        s += "}";
    }
    s += "]}";
    return s;
}

} // namespace sc
