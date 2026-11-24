// ScarlettCompat shader pipeline - shader container inspection.
// MIT. Clean-room reader for the publicly documented DXBC container layout
// (magic "DXBC", digest, version, total size, chunk count, chunk offset table,
// chunks of {fourcc, size, payload padded to 4 bytes}).
//
// It makes no assumption that the game hands us a well-formed blob: every
// length is bounds checked and the reader never reads outside the caller's
// buffer. Xbox Series X|S shaders arrive in this container (DXIL chunk inside
// a DXBC envelope); raw DXIL bitcode is also accepted.
//
// Not wired into the boot path. See docs/SHADER-PIPELINE-PHASE0.md.
#ifndef SC_SHADER_CONTAINER_H
#define SC_SHADER_CONTAINER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sc {

// Shader stages in D3D12 order for pipeline-state flags. `unknown` is a valid
// observation (capture records it verbatim so the corpus can tell us what the
// game actually ships).
enum class sc_stage : uint32_t {
    unknown = 0,
    vertex = 1,
    hull = 2,
    domain = 3,
    geometry = 4,
    pixel = 5,
    compute = 6,
    mesh = 7,
    amplification = 8,
};

const char* sc_stage_name(sc_stage s);
bool sc_stage_is_graphics(sc_stage s);
// Single-letter tag used for dump file names ("vs", "ps", ...).
const char* sc_stage_tag(sc_stage s);

enum class sc_container_format : uint32_t {
    unknown = 0,
    dxbc,          // DXBC envelope with a DXIL chunk (SM6 / Xbox Series X|S)
    dxbc_sm5,      // DXBC envelope with a SHEX/SHDR chunk (SM5)
    dxil_bitcode,  // raw LLVM bitcode ("BC\xC0\xDE"), no envelope
};

const char* sc_container_format_name(sc_container_format f);

struct sc_chunk {
    uint32_t fourcc = 0;
    std::string name;    // fourcc bytes in file order, printable replacement for non-print
    uint32_t offset = 0; // absolute offset of the chunk header inside the blob
    uint32_t size = 0;   // payload size in bytes (padding excluded)
};

// Where the stage identification came from. After the token correction there is
// exactly one possible answer; it stays explicit so a record is self-describing.
enum class sc_stage_source : uint32_t {
    none = 0,
    shader_token, // DXIL chunk version token (SM6) or SHEX/SHDR token (SM5) -- authoritative
};

const char* sc_stage_source_name(sc_stage_source s);

struct sc_container_info {
    bool ok = false;
    std::string error;

    sc_container_format format = sc_container_format::unknown;
    uint32_t header_version = 0;
    uint32_t total_size = 0;   // as declared by the header (or blob size for raw bitcode)
    uint32_t chunk_count = 0;
    uint8_t digest[16] = {};   // 16-byte container digest (opaque)

    sc_stage stage = sc_stage::unknown;
    sc_stage_source stage_from = sc_stage_source::none;
    // Raw first dword of the shader chunk. DXIL: bits 0-15 = shader model
    // (0x67 == 6.7), bits 16-23 = stage. SHEX/SHDR: low 16 bits = program type.
    uint32_t shader_token = 0;
    uint16_t program_major = 0;
    uint16_t program_minor = 0;
    bool has_program_version = false;

    std::vector<sc_chunk> chunks;

    bool has_dxil = false;
    bool has_shex = false;
    bool has_root_signature = false; // RTS0

    int shader_chunk = -1; // index into chunks of the DXIL/SHEX/SHDR chunk

    std::string chunk_list() const; // "DXIL,PSV0,ISG1,..." for logs/manifests
};

// Parse a blob. Returns out.ok; on failure out.error says why and out.ok is
// false. Never reads outside [data, data+size).
bool sc_parse_container(const void* data, size_t size, sc_container_info& out);

// Serialize the same information as JSON (single line, no trailing comma).
std::string sc_container_info_json(const sc_container_info& info);

// Find the first "DXBC" envelope inside a buffer that may carry a prefix
// (Xbox resource headers, RPF chunk decoration, ...). Returns true and sets
// `offset` when found. `search_limit` bounds the scan (0 = whole buffer).
bool sc_locate_container(const void* data, size_t size, size_t search_limit, size_t& offset);

} // namespace sc

#endif // SC_SHADER_CONTAINER_H
