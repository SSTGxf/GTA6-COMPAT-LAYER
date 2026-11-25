// ScarlettCompat shader pipeline - Phase 6 implementation.
// MIT. New file; not wired into the boot path.
#include "shader/sc_pipeline_binding.h"

#include "shader/sc_sha256.h"

#include <algorithm>
#include <cstring>

namespace sc {
namespace {

constexpr uint32_t kRts0 = 0x30535452u; // "RTS0" little endian
constexpr uint32_t kDxbc = 0x43425844u; // "DXBC" little endian

bool read_u32(const uint8_t* p, size_t n, size_t off, uint32_t& v) {
    if (p == nullptr || off > n || n - off < 4) {
        return false;
    }
    v = static_cast<uint32_t>(p[off]) | (static_cast<uint32_t>(p[off + 1]) << 8) |
        (static_cast<uint32_t>(p[off + 2]) << 16) | (static_cast<uint32_t>(p[off + 3]) << 24);
    return true;
}

bool range_less(const sc_descriptor_range& a, const sc_descriptor_range& b) {
    if (a.offset_in_table != b.offset_in_table) {
        return a.offset_in_table < b.offset_in_table;
    }
    if (a.register_space != b.register_space) {
        return a.register_space < b.register_space;
    }
    if (a.base_register != b.base_register) {
        return a.base_register < b.base_register;
    }
    return static_cast<uint32_t>(a.kind) < static_cast<uint32_t>(b.kind);
}

// Canonical graphics/compute stage order. The capture may hand stages in any order;
// the binding always reports them in this order so two records that differ only in
// the order of the shader array produce identical bindings.
const sc_stage kStageOrder[] = {
    sc_stage::vertex,  sc_stage::hull,        sc_stage::domain, sc_stage::geometry,
    sc_stage::pixel,   sc_stage::compute,     sc_stage::mesh,   sc_stage::amplification,
};

bool chunk_u32(const uint8_t* p, size_t n, size_t chunk_base, size_t chunk_bytes, size_t rel,
               uint32_t& v) {
    if (rel > chunk_bytes || chunk_bytes - rel < 4) {
        return false;
    }
    return read_u32(p, n, chunk_base + rel, v);
}

float bits_to_float(uint32_t bits) {
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

uint32_t param_arg_dwords(const sc_root_param& p) {
    switch (p.kind) {
    case sc_root_param_kind::constants32:
        return p.num_32bit;
    case sc_root_param_kind::descriptor_table:
        return kScRootTableDwords;
    default:
        return kScRootDescriptorDwords;
    }
}

} // namespace

sc_root_signature_identity sc_root_signature_identity_from_blob(const uint8_t* bytes, size_t size) {
    sc_root_signature_identity id;
    id.bytes = size;
    id.sha256 = sc_sha256_hex(bytes, size);

    uint32_t magic = 0;
    uint32_t chunk_count = 0;
    uint32_t chunk_offset = 0;
    uint32_t chunk_id = 0;
    if (size < 68 || !read_u32(bytes, size, 0, magic) || magic != kDxbc) {
        id.error = "root signature blob is not a DXBC container";
        return id;
    }
    id.has_magic = true;
    if (!read_u32(bytes, size, 20, id.container_version) ||
        !read_u32(bytes, size, 24, id.declared_bytes) ||
        !read_u32(bytes, size, 28, chunk_count) ||
        !read_u32(bytes, size, 32, chunk_offset) ||
        !read_u32(bytes, size, 36, chunk_id) ||
        !read_u32(bytes, size, 40, id.chunk_bytes)) {
        id.error = "root signature blob header is truncated";
        return id;
    }
    if (id.container_version == 0) {
        id.error = "root signature blob declares container version 0";
        return id;
    }
    if (id.declared_bytes != size) {
        id.error = "root signature blob declares a different size";
        return id;
    }
    if (chunk_count != 1 || chunk_id != kRts0) {
        id.error = "root signature blob does not hold exactly one RTS0 chunk";
        return id;
    }
    if (chunk_offset != 36 || static_cast<uint64_t>(chunk_offset) + 8 + id.chunk_bytes != size) {
        id.error = "root signature RTS0 chunk size does not match the blob";
        return id;
    }

    const size_t base = static_cast<size_t>(chunk_offset) + 8;
    if (!read_u32(bytes, size, base, id.root_version) ||
        !read_u32(bytes, size, base + 4, id.param_count) ||
        !read_u32(bytes, size, base + 12, id.sampler_count) ||
        !read_u32(bytes, size, base + 20, id.flags)) {
        id.error = "root signature RTS0 chunk header is truncated";
        return id;
    }
    if (id.root_version == 0 || id.root_version > 2) {
        id.error = "root signature RTS0 chunk declares an unknown version";
        return id;
    }
    uint32_t params_offset = 0;
    uint32_t samplers_offset = 0;
    if (!read_u32(bytes, size, base + 8, params_offset) ||
        !read_u32(bytes, size, base + 16, samplers_offset)) {
        id.error = "root signature RTS0 section offsets are missing";
        return id;
    }
    if (params_offset < 24 || params_offset > id.chunk_bytes ||
        samplers_offset < 24 || samplers_offset > id.chunk_bytes) {
        id.error = "root signature RTS0 section offset is outside the chunk";
        return id;
    }
    id.ok = true;
    return id;
}


sc_root_signature_desc sc_root_signature_parse(const uint8_t* bytes, size_t size) {
    sc_root_signature_desc out;
    const sc_root_signature_identity id = sc_root_signature_identity_from_blob(bytes, size);
    if (!id.ok) {
        out.ok = false;
        out.error = id.error;
        return out;
    }

    // Container offsets are fixed for a serialized root signature: 36-byte container
    // header, 8-byte chunk header, so the RTS0 data starts at byte 44 and runs for
    // id.chunk_bytes. Every offset inside the chunk is relative to that start.
    const size_t base = 44;
    const size_t chunk = id.chunk_bytes;
    uint32_t version = 0;
    uint32_t param_count = 0;
    uint32_t params_off = 0;
    uint32_t sampler_count = 0;
    uint32_t samplers_off = 0;
    uint32_t flags = 0;
    if (!chunk_u32(bytes, size, base, chunk, 0, version) ||
        !chunk_u32(bytes, size, base, chunk, 4, param_count) ||
        !chunk_u32(bytes, size, base, chunk, 8, params_off) ||
        !chunk_u32(bytes, size, base, chunk, 12, sampler_count) ||
        !chunk_u32(bytes, size, base, chunk, 16, samplers_off) ||
        !chunk_u32(bytes, size, base, chunk, 20, flags)) {
        out.ok = false;
        out.error = "root signature chunk header is truncated";
        return out;
    }
    out.version = version;
    out.flags = flags;
    // Version 2 is what D3D12SerializeVersionedRootSignature emits for 1.1: every
    // root descriptor/SRV/UAV payload carries a flags DWORD and every range is 24 bytes
    // (flags before the table offset). Version 1 is the 1.0 encoding.
    const bool root_1_1 = id.root_version >= 2;
    if (param_count > kScMaxRootParams) {
        out.ok = false;
        out.error = "root signature declares more parameters than the limit";
        return out;
    }
    if (sampler_count > 64) {
        out.ok = false;
        out.error = "root signature declares more static samplers than the limit";
        return out;
    }

    for (uint32_t i = 0; i < param_count; ++i) {
        const size_t entry = static_cast<size_t>(params_off) + 12u * i;
        uint32_t type = 0;
        uint32_t visibility = 0;
        uint32_t payload_off = 0;
        if (!chunk_u32(bytes, size, base, chunk, entry, type) ||
            !chunk_u32(bytes, size, base, chunk, entry + 4, visibility) ||
            !chunk_u32(bytes, size, base, chunk, entry + 8, payload_off)) {
            out.ok = false;
            out.error = "root signature parameter entry is outside the chunk";
            return out;
        }
        sc_root_param param;
        param.visibility = visibility;
        if (type > static_cast<uint32_t>(sc_root_param_kind::uav)) {
            out.ok = false;
            out.error = "root signature parameter has an unknown type";
            return out;
        }
        param.kind = static_cast<sc_root_param_kind>(type);
        const size_t payload = payload_off;
        if (param.kind == sc_root_param_kind::descriptor_table) {
            uint32_t ranges = 0;
            uint32_t ranges_off = 0;
            if (!chunk_u32(bytes, size, base, chunk, payload, ranges) ||
                !chunk_u32(bytes, size, base, chunk, payload + 4, ranges_off)) {
                out.ok = false;
                out.error = "descriptor table payload is outside the chunk";
                return out;
            }
            if (ranges == 0 || ranges > 64) {
                out.ok = false;
                out.error = "descriptor table declares an impossible range count";
                return out;
            }
            const size_t range_bytes = root_1_1 ? 24u : 20u;
            for (uint32_t r = 0; r < ranges; ++r) {
                const size_t at = static_cast<size_t>(ranges_off) + range_bytes * r;
                uint32_t kind = 0;
                uint32_t num = 0;
                uint32_t reg = 0;
                uint32_t space = 0;
                uint32_t range_flags = 0;
                uint32_t offset = 0;
                const size_t flags_at = at + 16;
                const size_t offset_at = root_1_1 ? at + 20 : at + 16;
                if (!chunk_u32(bytes, size, base, chunk, at, kind) ||
                    !chunk_u32(bytes, size, base, chunk, at + 4, num) ||
                    !chunk_u32(bytes, size, base, chunk, at + 8, reg) ||
                    !chunk_u32(bytes, size, base, chunk, at + 12, space) ||
                    !chunk_u32(bytes, size, base, chunk, offset_at, offset)) {
                    out.ok = false;
                    out.error = "descriptor range is outside the chunk";
                    return out;
                }
                if (root_1_1 && !chunk_u32(bytes, size, base, chunk, flags_at, range_flags)) {
                    out.ok = false;
                    out.error = "descriptor range flags are outside the chunk";
                    return out;
                }
                if (kind > static_cast<uint32_t>(sc_descriptor_range_kind::sampler) || num == 0) {
                    out.ok = false;
                    out.error = "descriptor range has an impossible type or count";
                    return out;
                }
                sc_descriptor_range range;
                range.kind = static_cast<sc_descriptor_range_kind>(kind);
                range.num_descriptors = num;
                range.base_register = reg;
                range.register_space = space;
                range.flags = range_flags;
                range.offset_in_table = offset;
                param.ranges.push_back(range);
            }
        } else {
            uint32_t reg = 0;
            uint32_t space = 0;
            if (!chunk_u32(bytes, size, base, chunk, payload, reg) ||
                !chunk_u32(bytes, size, base, chunk, payload + 4, space)) {
                out.ok = false;
                out.error = "root parameter payload is outside the chunk";
                return out;
            }
            param.shader_register = reg;
            param.register_space = space;
            if (root_1_1 && param.kind != sc_root_param_kind::constants32) {
                uint32_t descriptor_flags = 0;
                if (!chunk_u32(bytes, size, base, chunk, payload + 8, descriptor_flags)) {
                    out.ok = false;
                    out.error = "root descriptor flags are outside the chunk";
                    return out;
                }
                param.descriptor_flags = descriptor_flags;
            }
            if (param.kind == sc_root_param_kind::constants32) {
                uint32_t values = 0;
                if (!chunk_u32(bytes, size, base, chunk, payload + 8, values)) {
                    out.ok = false;
                    out.error = "root constants payload is outside the chunk";
                    return out;
                }
                if (values == 0) {
                    out.ok = false;
                    out.error = "root constants declare no 32-bit values";
                    return out;
                }
                param.num_32bit = values;
            }
        }
        out.params.push_back(param);
    }

    for (uint32_t i = 0; i < sampler_count; ++i) {
        const size_t at = static_cast<size_t>(samplers_off) + 52u * i;
        uint32_t w[13] = {};
        for (size_t k = 0; k < 13; ++k) {
            if (!chunk_u32(bytes, size, base, chunk, at + 4 * k, w[k])) {
                out.ok = false;
                out.error = "static sampler is outside the chunk";
                return out;
            }
        }
        sc_static_sampler sampler;
        sampler.filter = w[0];
        sampler.address_u = w[1];
        sampler.address_v = w[2];
        sampler.address_w = w[3];
        sampler.mip_lod_bias = bits_to_float(w[4]);
        sampler.max_anisotropy = w[5];
        sampler.comparison_func = w[6];
        sampler.border_color = w[7];
        sampler.min_lod = bits_to_float(w[8]);
        sampler.max_lod = bits_to_float(w[9]);
        sampler.shader_register = w[10];
        sampler.register_space = w[11];
        sampler.visibility = w[12];
        out.samplers.push_back(sampler);
    }

    out.ok = true;
    return out;
}

sc_root_binding sc_root_binding_build(const sc_root_signature_desc& desc) {
    sc_root_binding out;
    if (desc.params.size() > kScMaxRootParams) {
        out.error = "root signature has more parameters than the limit";
        return out;
    }
    if (desc.samplers.size() > 64) {
        out.error = "root signature has more static samplers than the limit";
        return out;
    }

    uint32_t arg_offset = 0;
    uint32_t descriptor_base = 0;
    for (size_t i = 0; i < desc.params.size(); ++i) {
        const sc_root_param& p = desc.params[i];
        sc_param_binding pb;
        pb.param_index = static_cast<uint32_t>(i);
        pb.kind = p.kind;
        pb.arg_dwords = param_arg_dwords(p);
        if (pb.arg_dwords == 0) {
            out.error = "root constants declare no 32-bit values";
            return out;
        }
        if (arg_offset + pb.arg_dwords > kScMaxRootArgDwords) {
            out.error = "root arguments exceed the 64-DWORD limit";
            return out;
        }
        pb.arg_offset_dwords = arg_offset;
        arg_offset += pb.arg_dwords;
        out.params.push_back(pb);

        if (p.kind != sc_root_param_kind::descriptor_table) {
            continue;
        }
        if (p.ranges.empty()) {
            out.error = "descriptor table holds no ranges";
            return out;
        }
        if (p.ranges.size() > 64) {
            out.error = "descriptor table holds more ranges than the limit";
            return out;
        }
        std::vector<sc_descriptor_range> ranges = p.ranges;
        std::stable_sort(ranges.begin(), ranges.end(), range_less);

        sc_table_binding tb;
        tb.param_index = pb.param_index;
        tb.visibility = p.visibility;
        tb.range_count = static_cast<uint32_t>(ranges.size());
        uint32_t cursor = 0;
        for (size_t r = 0; r < ranges.size(); ++r) {
            const sc_descriptor_range& range = ranges[r];
            if (range.num_descriptors == 0) {
                out.error = "descriptor range declares zero descriptors";
                return out;
            }
            if (static_cast<uint32_t>(range.kind) > 3) {
                out.error = "descriptor range has an unknown range type";
                return out;
            }
            if (range.offset_in_table < cursor) {
                out.error = "descriptor ranges in one table overlap";
                return out;
            }
            if (range.offset_in_table > cursor) {
                out.error = "descriptor ranges in one table leave a gap";
                return out;
            }
            cursor += range.num_descriptors;
            if (descriptor_base + cursor > 0x100000u) {
                out.error = "descriptor table is larger than the heap limit";
                return out;
            }
        }
        tb.base_descriptor = descriptor_base;
        tb.descriptors = cursor;
        descriptor_base += cursor;
        out.tables.push_back(tb);
    }

    out.sampler_count = static_cast<uint32_t>(desc.samplers.size());
    for (size_t i = 0; i < desc.samplers.size(); ++i) {
        for (size_t j = i + 1; j < desc.samplers.size(); ++j) {
            if (desc.samplers[i].shader_register == desc.samplers[j].shader_register &&
                desc.samplers[i].register_space == desc.samplers[j].register_space) {
                out.error = "two static samplers claim the same register";
                return out;
            }
        }
    }

    out.root_arg_dwords = arg_offset;
    out.descriptor_count = descriptor_base;
    out.descriptor_bytes = static_cast<uint64_t>(descriptor_base) * kScDescriptorBytes;
    out.ok = true;
    return out;
}

sc_pipeline_binding sc_pipeline_bind(const sc_capture_pso_desc& desc) {
    sc_pipeline_binding out;
    out.root_signature = sc_root_signature_identity_from_blob(
        desc.root_signature.empty() ? nullptr : desc.root_signature.data(),
        desc.root_signature.size());
    out.root_desc = sc_root_signature_parse(
        desc.root_signature.empty() ? nullptr : desc.root_signature.data(),
        desc.root_signature.size());
    out.root = sc_root_binding_build(out.root_desc);

    out.rtv_count = static_cast<uint32_t>(desc.rtv_formats.size());
    out.rtv_formats = desc.rtv_formats;
    out.dsv_format = desc.dsv_format;
    out.sample_count = desc.sample_count;
    out.sample_quality = desc.sample_quality;
    out.sample_mask = desc.sample_mask;
    out.primitive_topology_type = desc.primitive_topology_type;
    out.node_mask = desc.node_mask;
    out.flags = desc.flags;
    out.input_element_count = static_cast<uint32_t>(desc.input_layout.size());

    uint32_t highest_slot = 0;
    bool any_slot = false;
    for (size_t i = 0; i < desc.input_layout.size(); ++i) {
        const uint32_t slot = desc.input_layout[i].input_slot;
        if (!any_slot || slot > highest_slot) {
            highest_slot = slot;
            any_slot = true;
        }
    }
    out.input_slot_count = any_slot ? highest_slot + 1 : 0;

    for (size_t s = 0; s < sizeof(kStageOrder) / sizeof(kStageOrder[0]); ++s) {
        sc_stage_binding sb;
        sb.stage = kStageOrder[s];
        for (size_t i = 0; i < desc.shaders.size(); ++i) {
            if (desc.shaders[i].stage != kStageOrder[s]) {
                continue;
            }
            sb.present = !desc.shaders[i].bytes.empty();
            sb.bytes = desc.shaders[i].bytes.size();
            sb.sha256 = sc_sha256_hex(desc.shaders[i].bytes.empty() ? nullptr
                                                                  : desc.shaders[i].bytes.data(),
                                      desc.shaders[i].bytes.size());
            break;
        }
        out.stages.push_back(sb);
    }

    if (desc.shaders.empty()) {
        out.error = "record holds no shader stage";
        return out;
    }
    for (size_t i = 0; i < desc.shaders.size(); ++i) {
        if (desc.shaders[i].stage == sc_stage::unknown) {
            out.error = "record holds a shader whose stage was not identified";
            return out;
        }
        if (desc.shaders[i].bytes.empty()) {
            out.error = "record holds a shader stage with no bytes";
            return out;
        }
    }
    if (desc.root_signature.empty()) {
        out.error = "record holds no root signature blob";
        return out;
    }
    if (!out.root_signature.ok) {
        out.error = "root signature blob: " + out.root_signature.error;
        return out;
    }
    if (!out.root_desc.ok) {
        out.error = "root signature parse: " + out.root_desc.error;
        return out;
    }
    if (!out.root.ok) {
        out.error = "root signature layout: " + out.root.error;
        return out;
    }
    if (out.rtv_count > 8) {
        out.error = "record binds more render targets than D3D12 allows";
        return out;
    }
    if (desc.render_targets.size() > out.rtv_count) {
        out.error = "record has more render-target states than formats";
        return out;
    }
    out.ok = true;
    return out;
}

sc_pipeline_check sc_pipeline_verify(const sc_capture_pso_desc& desc,
                                     const sc_pipeline_binding& binding) {
    sc_pipeline_check c;
    // Everything structural comes from the description the binding parsed out of the
    // record's own bytes; no caller-supplied root signature is involved.
    const sc_root_signature_desc& rootsig = binding.root_desc;
    // Range overlaps are a property of the description, not of the binding, so they are
    // counted before the binding gate: a rejected binding still reports them.
    for (size_t i = 0; i < rootsig.params.size(); ++i) {
        const sc_root_param& p = rootsig.params[i];
        if (p.kind != sc_root_param_kind::descriptor_table) {
            continue;
        }
        c.ranges_checked += static_cast<uint32_t>(p.ranges.size());
        for (size_t r = 0; r < p.ranges.size(); ++r) {
            for (size_t q = 0; q < p.ranges.size(); ++q) {
                if (q == r) {
                    continue;
                }
                const uint32_t a0 = p.ranges[r].offset_in_table;
                const uint32_t a1 = a0 + p.ranges[r].num_descriptors;
                const uint32_t b0 = p.ranges[q].offset_in_table;
                const uint32_t b1 = b0 + p.ranges[q].num_descriptors;
                if (a0 < b1 && b0 < a1) {
                    ++c.descriptor_overlaps;
                }
            }
        }
    }
    if (c.descriptor_overlaps != 0) {
        c.detail = "descriptor ranges in one table overlap";
        return c;
    }
    if (!binding.ok) {
        c.detail = binding.error;
        return c;
    }

    // ---- stages: set equality, order independent --------------------------------
    uint32_t expected_stages = 0;
    for (size_t i = 0; i < desc.shaders.size(); ++i) {
        if (desc.shaders[i].bytes.empty()) {
            continue;
        }
        ++expected_stages;
        bool found = false;
        for (size_t s = 0; s < binding.stages.size(); ++s) {
            if (binding.stages[s].stage == desc.shaders[i].stage && binding.stages[s].present &&
                binding.stages[s].bytes == desc.shaders[i].bytes.size()) {
                found = true;
                break;
            }
        }
        if (!found) {
            c.detail = "binding drops a captured stage";
            return c;
        }
    }
    c.stages_with_blob = expected_stages;
    for (size_t s = 0; s < binding.stages.size(); ++s) {
        if (!binding.stages[s].present) {
            continue;
        }
        ++c.stages_bound;
        bool in_record = false;
        for (size_t i = 0; i < desc.shaders.size(); ++i) {
            if (desc.shaders[i].stage == binding.stages[s].stage &&
                !desc.shaders[i].bytes.empty()) {
                in_record = true;
                break;
            }
        }
        if (!in_record) {
            c.detail = "binding invents a stage the record does not hold";
            return c;
        }
    }
    if (c.stages_bound != c.stages_with_blob) {
        c.detail = "stage count differs between record and binding";
        return c;
    }

    // ---- render targets --------------------------------------------------------
    if (binding.rtv_count != desc.rtv_formats.size() ||
        binding.rtv_formats.size() != desc.rtv_formats.size()) {
        c.detail = "render-target count differs from the record";
        return c;
    }
    for (size_t i = 0; i < desc.rtv_formats.size(); ++i) {
        if (binding.rtv_formats[i] != desc.rtv_formats[i]) {
            c.detail = "render-target format differs from the record";
            return c;
        }
        ++c.rtv_formats_matched;
    }
    if (binding.dsv_format != desc.dsv_format) {
        c.detail = "depth-stencil format differs from the record";
        return c;
    }

    // ---- sample state ----------------------------------------------------------
    c.sample_state_consistent = binding.sample_count == desc.sample_count &&
                                binding.sample_quality == desc.sample_quality &&
                                binding.sample_mask == desc.sample_mask &&
                                desc.render_targets.size() == desc.rtv_formats.size();
    if (!c.sample_state_consistent) {
        c.detail = "sample state or render-target states disagree with the record";
        return c;
    }

    // ---- input layout ----------------------------------------------------------
    c.input_layout_consistent = binding.input_element_count == desc.input_layout.size();
    if (c.input_layout_consistent) {
        uint32_t highest = 0;
        bool any = false;
        for (size_t i = 0; i < desc.input_layout.size(); ++i) {
            if (!any || desc.input_layout[i].input_slot > highest) {
                highest = desc.input_layout[i].input_slot;
                any = true;
            }
        }
        c.input_layout_consistent = (any ? highest + 1 : 0) == binding.input_slot_count;
    }
    if (!c.input_layout_consistent) {
        c.detail = "input layout counts disagree with the record";
        return c;
    }

    // ---- descriptor tables: coverage, and overlaps counted order independently --
    for (size_t t = 0; t < binding.root.tables.size(); ++t) {
        const sc_table_binding& tb = binding.root.tables[t];
        if (tb.param_index >= rootsig.params.size()) {
            c.detail = "table binding points at a parameter that does not exist";
            return c;
        }
        const sc_root_param& p = rootsig.params[tb.param_index];
        if (p.kind != sc_root_param_kind::descriptor_table || p.ranges.empty()) {
            c.detail = "table binding points at a parameter that is not a table";
            return c;
        }
        ++c.tables_checked;
        uint32_t cursor = 0;
        for (size_t r = 0; r < p.ranges.size(); ++r) {
            if (p.ranges[r].num_descriptors == 0) {
                c.detail = "descriptor range declares zero descriptors";
                return c;
            }
        }
        for (size_t r = 0; r < p.ranges.size(); ++r) {
            cursor += p.ranges[r].num_descriptors;
        }
        if (cursor != tb.descriptors) {
            c.detail = "table size differs from the sum of its ranges";
            return c;
        }
        if (tb.base_descriptor + tb.descriptors > binding.root.descriptor_count) {
            c.detail = "table does not fit the descriptor run";
            return c;
        }
    }
    if (c.tables_checked != binding.root.tables.size() ||
        binding.root.descriptor_count != binding.root.descriptor_bytes / kScDescriptorBytes) {
        c.detail = "descriptor run does not agree with its byte size";
        return c;
    }

    // ---- root argument packing -------------------------------------------------
    if (binding.root.params.size() != rootsig.params.size()) {
        c.detail = "root parameter count differs from the description";
        return c;
    }
    uint32_t expect = 0;
    for (size_t i = 0; i < binding.root.params.size(); ++i) {
        const sc_param_binding& pb = binding.root.params[i];
        if (pb.param_index != i || pb.arg_offset_dwords != expect) {
            c.detail = "root argument offsets do not pack";
            return c;
        }
        expect += pb.arg_dwords;
        ++c.arg_offsets_monotonic;
    }
    if (expect != binding.root.root_arg_dwords) {
        c.detail = "root argument total differs from the sum of the parameters";
        return c;
    }

    // ---- blob header vs the neutral description -------------------------------
    c.identity_matches_header = binding.root_signature.ok &&
                                binding.root_signature.param_count == rootsig.params.size() &&
                                binding.root_signature.sampler_count == rootsig.samplers.size() &&
                                binding.root_signature.flags == rootsig.flags;
    if (!c.identity_matches_header) {
        c.detail = "root signature blob header disagrees with the description";
        return c;
    }

    c.ok = true;
    return c;
}

bool sc_root_signature_matches(const sc_root_signature_desc& parsed,
                               const sc_root_signature_desc& supplied, std::string* detail) {
    auto fail = [detail](const char* what) {
        if (detail != nullptr) {
            *detail = what;
        }
        return false;
    };
    if (!parsed.ok || !supplied.ok) {
        return fail("one of the descriptions is not valid");
    }
    // The serialized version (1 for 1.0, 2 for 1.1) is a container detail; two blobs of
    // the same desc serialized by the two APIs must compare equal here.
    if (parsed.flags != supplied.flags) {
        return fail("root signature flags differ");
    }
    if (parsed.params.size() != supplied.params.size()) {
        return fail("root signature parameter counts differ");
    }
    for (size_t i = 0; i < parsed.params.size(); ++i) {
        const sc_root_param& a = parsed.params[i];
        const sc_root_param& b = supplied.params[i];
        if (a.kind != b.kind) {
            return fail("root parameter types differ");
        }
        if (a.visibility != b.visibility) {
            return fail("root parameter visibilities differ");
        }
        if (a.kind == sc_root_param_kind::constants32 && a.num_32bit != b.num_32bit) {
            return fail("root constants counts differ");
        }
        if (a.kind != sc_root_param_kind::descriptor_table &&
            (a.shader_register != b.shader_register || a.register_space != b.register_space ||
             a.descriptor_flags != b.descriptor_flags)) {
            return fail("root parameter registers or descriptor flags differ");
        }
        if (a.kind == sc_root_param_kind::descriptor_table) {
            if (a.ranges.size() != b.ranges.size()) {
                return fail("descriptor range counts differ");
            }
            std::vector<sc_descriptor_range> ra = a.ranges;
            std::vector<sc_descriptor_range> rb = b.ranges;
            std::stable_sort(ra.begin(), ra.end(), range_less);
            std::stable_sort(rb.begin(), rb.end(), range_less);
            for (size_t r = 0; r < ra.size(); ++r) {
                if (ra[r].kind != rb[r].kind || ra[r].num_descriptors != rb[r].num_descriptors ||
                    ra[r].base_register != rb[r].base_register ||
                    ra[r].register_space != rb[r].register_space ||
                    ra[r].flags != rb[r].flags ||
                    ra[r].offset_in_table != rb[r].offset_in_table) {
                    return fail("descriptor ranges differ");
                }
            }
        }
    }
    if (parsed.samplers.size() != supplied.samplers.size()) {
        return fail("static sampler counts differ");
    }
    auto sampler_before = [](const sc_static_sampler& a, const sc_static_sampler& b) {
        if (a.register_space != b.register_space) {
            return a.register_space < b.register_space;
        }
        return a.shader_register < b.shader_register;
    };
    std::vector<sc_static_sampler> sa = parsed.samplers;
    std::vector<sc_static_sampler> sb = supplied.samplers;
    std::stable_sort(sa.begin(), sa.end(), sampler_before);
    std::stable_sort(sb.begin(), sb.end(), sampler_before);
    for (size_t i = 0; i < sa.size(); ++i) {
        const sc_static_sampler& a = sa[i];
        const sc_static_sampler& b = sb[i];
        if (a.filter != b.filter || a.address_u != b.address_u || a.address_v != b.address_v ||
            a.address_w != b.address_w || a.mip_lod_bias != b.mip_lod_bias ||
            a.max_anisotropy != b.max_anisotropy || a.comparison_func != b.comparison_func ||
            a.border_color != b.border_color || a.min_lod != b.min_lod || a.max_lod != b.max_lod ||
            a.shader_register != b.shader_register || a.register_space != b.register_space ||
            a.visibility != b.visibility) {
            return fail("static samplers differ");
        }
    }
    if (detail != nullptr) {
        detail->clear();
    }
    return true;
}

} // namespace sc
