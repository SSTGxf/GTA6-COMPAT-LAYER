// ScarlettCompat shader pipeline - Phase 6: resources and pipeline.
// MIT. New file; not wired into the boot path.
//
// WHAT THIS IS
//   The binding a captured PSO implies: where each root parameter's arguments go in
//   the root-argument space, how a table's descriptor ranges pack into one contiguous
//   run of 32-byte descriptors, and which shader/format/state every stage of the
//   pipeline binds. Input is the Phase 0 record (sc_capture_pso_desc), unchanged.
//
// WHERE THE ROOT SIGNATURE COMES FROM
//   A capture stores the serialized root signature exactly as D3D12 produced it, and
//   that blob is the only input: sc_root_signature_parse reads the DXBC container, the
//   RTS0 chunk header, the 12-byte parameter entries, their payloads (constants,
//   descriptor registers, or a table's range section), the ranges and the 52-byte
//   static samplers. Version 1 (D3D12SerializeRootSignature) and version 2 (1.1, via
//   D3D12SerializeVersionedRootSignature) are both parsed; version 2 adds a flags DWORD
//   to each descriptor payload and to each range. Both encodings were measured from the
//   serializers' own output - see tests/shader/sc_pipeline_binding_fixtures.h - and
//   reproduce every sample exactly, each parsed section ending on the last byte of its
//   blob. A caller that also holds the original desc can compare it with
//   sc_root_signature_matches, but nothing in the binding path needs it.
//
// WHAT IS SYNTHETIC
//   Every record in the harness is built in code. The root signature bytes in it are
//   real D3D12 output for a desc written in the harness, so the header checks are
//   checked against the real encoding; the desc is still synthetic and nothing here
//   has seen a game PSO.
#pragma once
#include "shader/sc_capture.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sc {

// D3D12_ROOT_PARAMETER_TYPE and D3D12_DESCRIPTOR_RANGE_TYPE numeric values.
enum class sc_root_param_kind : uint32_t {
    descriptor_table = 0,
    constants32 = 1,
    cbv = 2,
    srv = 3,
    uav = 4,
};
// D3D12_DESCRIPTOR_RANGE_TYPE numeric values: SRV is 0, not CBV.
enum class sc_descriptor_range_kind : uint32_t {
    srv = 0,
    uav = 1,
    cbv = 2,
    sampler = 3,
};

// D3D12_DESCRIPTOR_HEAP_TYPE_* descriptor sizes are all 32 bytes; the constant is
// named once so a caller cannot silently use a different stride.
constexpr uint32_t kScDescriptorBytes = 32;
// A root descriptor (CBV/SRV/UAV) occupies two DWORDs of root arguments; a table
// occupies one (a heap pointer); 32-bit constants occupy Num32BitValues DWORDs.
constexpr uint32_t kScRootDescriptorDwords = 2;
constexpr uint32_t kScRootTableDwords = 1;
constexpr uint32_t kScMaxRootParams = 64;
constexpr uint32_t kScMaxRootArgDwords = 64;

struct sc_descriptor_range {
    sc_descriptor_range_kind kind = sc_descriptor_range_kind::srv;
    uint32_t num_descriptors = 1;
    uint32_t base_register = 0;
    uint32_t register_space = 0;
    uint32_t flags = 0;           // D3D12_DESCRIPTOR_RANGE_FLAGS; always 0 in 1.0
    uint32_t offset_in_table = 0; // descriptors from the start of its table
};

struct sc_root_param {
    sc_root_param_kind kind = sc_root_param_kind::cbv;
    uint32_t visibility = 0;      // D3D12_SHADER_VISIBILITY
    uint32_t shader_register = 0;
    uint32_t register_space = 0;
    uint32_t descriptor_flags = 0; // D3D12_ROOT_DESCRIPTOR_FLAGS; 0 in 1.0
    uint32_t num_32bit = 0;        // constants32 only
    std::vector<sc_descriptor_range> ranges; // descriptor_table only
};

struct sc_static_sampler {
    uint32_t filter = 0;
    uint32_t address_u = 1;
    uint32_t address_v = 1;
    uint32_t address_w = 1;
    float mip_lod_bias = 0.0f;
    uint32_t max_anisotropy = 1;
    uint32_t comparison_func = 1;
    uint32_t border_color = 0;
    float min_lod = 0.0f;
    float max_lod = 100.0f;
    uint32_t shader_register = 0;
    uint32_t register_space = 0;
    uint32_t visibility = 0;
};

// The neutral form of a root signature. Produced by sc_root_signature_parse from the
// record's own bytes; a caller may also build one by hand, but only to cross-check.
struct sc_root_signature_desc {
    bool ok = true;
    std::string error;
    uint32_t version = 1;
    uint32_t flags = 0;
    std::vector<sc_root_param> params;
    std::vector<sc_static_sampler> samplers;
};

// What the serialized blob says about itself. Fields are read from the container
// header only (see the header comment).
struct sc_root_signature_identity {
    bool ok = false;
    bool has_magic = false;     // "DXBC" at 0 and "RTS0" chunk at 36
    uint32_t container_version = 0;
    uint32_t root_version = 0;  // first dword of the RTS0 chunk
    uint32_t declared_bytes = 0;
    uint32_t chunk_bytes = 0;
    uint32_t param_count = 0;
    uint32_t sampler_count = 0;
    uint32_t flags = 0;
    uint64_t bytes = 0;
    std::string sha256;
    std::string error;
};

sc_root_signature_identity sc_root_signature_identity_from_blob(const uint8_t* bytes, size_t size);

// Parses the serialized root signature into the neutral description: the RTS0 header,
// then one 3-DWORD entry per parameter (type, visibility, byte offset of its payload),
// the payloads themselves (constants or descriptor registers; for a table, its range
// count and the offset of its ranges), the ranges, and the 13-DWORD static samplers.
//
// Both serialized versions are handled, keyed on the RTS0 chunk version (1 or 2):
//   version 1 (D3D12SerializeRootSignature)      range = 20 bytes, no descriptor flags;
//   version 2 (D3D12SerializeVersionedRootSignature, 1.1) adds a flags DWORD to every
//   root descriptor/SRV/UAV payload (3 DWORDs instead of 2) and to every range
//   (24 bytes, flags before the table offset).
// Both encodings were measured from blobs the two serializers produced for synthetic
// descs - see tests/shader/sc_pipeline_binding_fixtures.h - and reproduce every one of
// them byte for byte. Rejects a blob whose counts, offsets or types leave the chunk.
sc_root_signature_desc sc_root_signature_parse(const uint8_t* bytes, size_t size);

// One descriptor table's placement in the descriptor heap.
struct sc_table_binding {
    uint32_t param_index = 0;
    uint32_t visibility = 0;
    uint32_t base_descriptor = 0;   // absolute heap index of the table's first range
    uint32_t descriptors = 0;       // table size in descriptors
    uint32_t range_count = 0;
};

// Where one parameter's arguments go in the root-argument space.
struct sc_param_binding {
    uint32_t param_index = 0;
    sc_root_param_kind kind = sc_root_param_kind::cbv;
    uint32_t arg_offset_dwords = 0;
    uint32_t arg_dwords = 0;        // 2 for descriptors, 1 for a table, N for constants
};

struct sc_root_binding {
    bool ok = false;
    std::string error;
    std::vector<sc_param_binding> params;
    std::vector<sc_table_binding> tables;
    uint32_t root_arg_dwords = 0;      // total root-argument space
    uint32_t descriptor_count = 0;     // sum over tables
    uint64_t descriptor_bytes = 0;     // descriptor_count * 32
    uint32_t sampler_count = 0;
};

// Computes the root/table layout. Ranges inside a table are sorted by their table
// offset, so two equivalent descriptions that list them in a different order produce
// the same layout. Rejects: more than kScMaxRootParams parameters, a table with no
// ranges, a range with zero descriptors, overlapping ranges in one table, a table
// whose ranges do not start at 0 or leave gaps, root arguments over 64 DWORDs.
sc_root_binding sc_root_binding_build(const sc_root_signature_desc& desc);

// The pipeline-state half: which stage binds which shader, and the fixed-function
// state the record already carries.
struct sc_stage_binding {
    sc_stage stage = sc_stage::unknown;
    bool present = false;
    uint64_t bytes = 0;
    std::string sha256;     // from the capture report when present
    bool from_capture_report = false;
};

struct sc_pipeline_binding {
    bool ok = false;
    std::string error;
    std::vector<sc_stage_binding> stages;   // canonical stage order
    uint32_t rtv_count = 0;
    std::vector<uint32_t> rtv_formats;
    uint32_t dsv_format = 0;
    uint32_t sample_count = 0;
    uint32_t sample_quality = 0;
    uint32_t sample_mask = 0;
    uint32_t primitive_topology_type = 0;
    uint32_t node_mask = 0;
    uint32_t flags = 0;
    uint32_t input_element_count = 0;
    uint32_t input_slot_count = 0;
    sc_root_signature_identity root_signature;
    sc_root_signature_desc root_desc; // parsed from desc.root_signature
    sc_root_binding root;
};

// Binds one Phase 0 record and nothing else: the parameter and table structure is
// parsed from desc.root_signature itself.
sc_pipeline_binding sc_pipeline_bind(const sc_capture_pso_desc& desc);

// Optional cross-check against another description - one the caller filled from its own
// copy of the desc, or the same desc parsed from the other serialized version. Compares
// semantics only: the serialized version is not part of the comparison, so a 1.0 and a
// 1.1 blob of the same desc match. Returns false and names the first difference.
bool sc_root_signature_matches(const sc_root_signature_desc& parsed,
                               const sc_root_signature_desc& supplied, std::string* detail);

// What the validator measured. Every comparison is order independent where the
// hardware or the capture does not fix an order.
struct sc_pipeline_check {
    uint32_t stages_bound = 0;
    uint32_t stages_with_blob = 0;
    uint32_t rtv_formats_matched = 0;
    uint32_t tables_checked = 0;
    uint32_t ranges_checked = 0;
    uint32_t descriptor_overlaps = 0;
    uint32_t arg_offsets_monotonic = 0;
    bool identity_matches_header = false;
    bool sample_state_consistent = false;
    bool input_layout_consistent = false;
    bool ok = false;
    std::string detail;
};

// Checks a binding against the record it came from: stage set equality, RTV/DSV and
// sample-state agreement, descriptor ranges non-overlapping and covering their table
// exactly, root-argument offsets packing without holes, and the parsed description
// agreeing with the blob header it came from.
sc_pipeline_check sc_pipeline_verify(const sc_capture_pso_desc& desc,
                                     const sc_pipeline_binding& binding);

} // namespace sc
