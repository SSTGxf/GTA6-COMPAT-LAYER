// ScarlettCompat shader pipeline - Phase 0 capture subsystem.
// MIT. New file; not wired into the boot path.
//
// WHAT THIS IS
//   A neutral, D3D12-free record of one PSO as seen at pipeline creation, plus
//   a store that de-duplicates records by content signature and a dumper that
//   writes the corpus to disk (raw container bytes + JSON manifest).
//
// WHY NEUTRAL STRUCTS
//   The capture module must be testable without D3D12, without a device and
//   without the game, and it must not pull d3d12_xs headers into the layer.
//   The field values are the D3D12 numeric enum values, so a caller can copy
//   them straight out of a D3D12_GRAPHICS_PIPELINE_STATE_DESC / stream desc.
//
// HOW FREEBUFF WIRES IT (when slots 86/16 land)
//   1. Fill an sc_capture_pso_desc from the D3D12 pipeline state desc (stage
//      bytecodes, root signature blob, formats, states).
//   2. sc_capture_store::add(desc) -> signature + hit count (dedupe is free).
//   3. sc_capture_dump(store, "shader_corpus") after N PSOs or at shutdown.
//   Nothing here needs a device, so it can also be fed from a file.
#ifndef SC_SHADER_CAPTURE_H
#define SC_SHADER_CAPTURE_H

#include "shader/sc_container.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sc {

// One shader stage as handed to pipeline state creation.
struct sc_capture_stage_blob {
    sc_stage stage = sc_stage::unknown;
    std::vector<uint8_t> bytes; // container bytes exactly as received
};

struct sc_capture_input_element {
    std::string semantic_name;
    uint32_t semantic_index = 0;
    uint32_t format = 0; // DXGI_FORMAT
    uint32_t input_slot = 0;
    uint32_t aligned_byte_offset = 0;
    uint32_t classification = 0; // D3D12_INPUT_CLASSIFICATION
    uint32_t instance_step_rate = 0;
};

struct sc_capture_render_target_state {
    uint32_t format = 0; // DXGI_FORMAT
    bool blend_enable = false;
    bool logic_op_enable = false;
    uint32_t src_blend = 1;
    uint32_t dst_blend = 1;
    uint32_t blend_op = 1;
    uint32_t src_blend_alpha = 1;
    uint32_t dst_blend_alpha = 1;
    uint32_t blend_op_alpha = 1;
    uint32_t logic_op = 0;
    uint32_t write_mask = 0x0f; // D3D12_COLOR_WRITE_ENABLE bits
};

struct sc_capture_rasterizer_state {
    uint32_t fill_mode = 2; // D3D12_FILL_MODE_SOLID
    uint32_t cull_mode = 1; // D3D12_CULL_MODE_BACK
    bool front_counter_clockwise = false;
    bool depth_bias = false;
    uint32_t depth_bias_value = 0; // as int32 bit pattern
    float depth_bias_clamp = 0.0f;
    float slope_scaled_depth_bias = 0.0f;
    bool depth_clip_enable = true;
    bool multisample_enable = false;
    bool antialiased_line_enable = false;
    uint32_t forced_sample_count = 0;
    bool conservative_raster = false;
};

struct sc_capture_stencil_op {
    uint32_t stencil_fail_op = 1;
    uint32_t stencil_depth_fail_op = 1;
    uint32_t stencil_pass_op = 1;
    uint32_t stencil_func = 1;
};

struct sc_capture_depth_stencil_state {
    bool depth_enable = true;
    uint32_t depth_write_mask = 1; // D3D12_DEPTH_WRITE_MASK_ALL
    uint32_t depth_func = 1;       // D3D12_COMPARISON_FUNC_LESS
    bool stencil_enable = false;
    uint32_t stencil_read_mask = 0xff;
    uint32_t stencil_write_mask = 0xff;
    bool front_face_stencil_enable = false;
    bool back_face_stencil_enable = false;
    sc_capture_stencil_op front_face;
    sc_capture_stencil_op back_face;
};

// One pipeline state as handed to slot 86 (CreatePipelineState).
struct sc_capture_pso_desc {
    std::string debug_name;
    uint32_t source_slot = 86;

    uint32_t node_mask = 0;
    uint32_t flags = 0; // D3D12_PIPELINE_STATE_FLAGS
    uint32_t sample_count = 1;
    uint32_t sample_quality = 0;
    uint32_t sample_mask = 0xffffffffu;
    uint32_t ib_strip_cut_value = 0;
    uint32_t patch_control_points = 0;
    uint32_t primitive_topology_type = 0; // D3D12_PRIMITIVE_TOPOLOGY_TYPE

    std::vector<uint32_t> rtv_formats;
    uint32_t dsv_format = 0; // DXGI_FORMAT_UNKNOWN == 0
    std::vector<sc_capture_input_element> input_layout;
    sc_capture_rasterizer_state rasterizer;
    sc_capture_depth_stencil_state depth_stencil;
    float blend_factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    std::vector<sc_capture_render_target_state> render_targets;

    std::vector<uint8_t> root_signature; // serialized root signature blob
    std::vector<sc_capture_stage_blob> shaders;
};

// Per-stage analysis result produced at capture time.
struct sc_capture_shader_report {
    sc_stage stage = sc_stage::unknown;
    sc_stage stage_from_container = sc_stage::unknown;
    std::string stage_source;   // shader-token / caller+container / caller / none
    std::string format;         // dxbc+dxil / dxbc+sm5 / dxil-bitcode / unknown
    std::string sha256;         // hex, over the container bytes
    std::string chunks;         // "DXIL,PSV0,ISG1"
    size_t size = 0;
    uint32_t chunk_count = 0;
    bool has_dxil = false;
    bool has_root_signature = false;
    std::string parse_error; // empty when the container parsed
};

struct sc_capture_record {
    uint64_t seq = 0;      // stable capture index (0-based)
    uint64_t signature = 0;// FNV-1a 64 over the canonical descriptor
    uint64_t hits = 0;     // times this exact PSO was submitted
    uint64_t first_add = 0;// store-wide add index of the first occurrence
    sc_capture_pso_desc desc;
    std::vector<sc_capture_shader_report> shaders;
    std::string stages;    // "vertex,pixel" in canonical order
    std::string signature_hex() const;
    size_t total_shader_bytes() const;
};

struct sc_capture_add_result {
    bool added = false;   // false => duplicate of an existing record
    uint64_t seq = 0;
    uint64_t signature = 0;
    uint64_t hits = 0;
};

class sc_capture_store {
public:
    // Adds a PSO. Identical descriptors (by signature) are recorded once and
    // bump `hits`, which keeps the corpus small while preserving frequency.
    sc_capture_add_result add(const sc_capture_pso_desc& desc);

    const std::vector<sc_capture_record>& records() const { return records_; }
    size_t size() const { return records_.size(); }
    uint64_t adds() const { return adds_; }
    uint64_t duplicates() const { return duplicates_; }
    void clear();

    // {"vertex":12,"pixel":9,...} over unique records.
    std::string stage_histogram_json() const;

private:
    std::vector<sc_capture_record> records_;
    std::unordered_map<uint64_t, size_t> by_signature_;
    uint64_t adds_ = 0;
    uint64_t duplicates_ = 0;
};

// Content signature of a descriptor (stable across runs, order-canonical).
uint64_t sc_capture_signature(const sc_capture_pso_desc& desc);

struct sc_capture_dump_options {
    bool write_shader_binaries = true;
    bool write_root_signature = true;
    bool write_record_json = true;
    bool write_index = true;
    bool write_manifest = true;
};

struct sc_capture_dump_report {
    bool ok = false;
    std::string root;
    uint32_t records = 0;
    uint32_t files = 0;
    uint64_t bytes = 0;
    std::vector<std::string> errors;
};

// Writes:
//   <dir>/pso_0000_<sig>/vs.bin, ps.bin, ..., rootsig.bin, record.json
//   <dir>/index.tsv
//   <dir>/manifest.json
sc_capture_dump_report sc_capture_dump(const sc_capture_store& store, const std::string& directory,
                                      const sc_capture_dump_options& options = {});

std::string sc_capture_record_json(const sc_capture_record& record);

// Small filesystem helpers (exported because the test harness uses them).
bool sc_make_dirs(const std::string& path);
bool sc_write_file(const std::string& path, const void* data, size_t size);

} // namespace sc

#endif // SC_SHADER_CAPTURE_H
