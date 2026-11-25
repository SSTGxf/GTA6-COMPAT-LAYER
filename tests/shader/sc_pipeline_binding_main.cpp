// ScarlettCompat shader pipeline - Phase 6 harness (resources and pipeline).
// MIT. Standalone: no game, no guest, no device, no layer, no emitter.
//
// Every PSO record here is SYNTHETIC: built in this file, never captured from a game.
// The root signature bytes are real D3D12 output, though: each fixture in
// sc_pipeline_binding_fixtures.h is the exact blob D3D12SerializeRootSignature
// produced for the desc named beside it, so parsing them exercises the real RTS0
// encoding. The descs are ours and no game PSO has been seen, so nothing here claims
// real-record validation.
#include "shader/sc_pipeline_binding.h"

#include "sc_pipeline_binding_fixtures.h"
#include "shader/sc_sha256.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace sc;

namespace {

int g_checks = 0;
int g_fail = 0;

void check(bool condition, const char* expr, const char* file, int line) {
    ++g_checks;
    if (condition) {
        return;
    }
    ++g_fail;
    std::printf("FAIL %s:%d  %s\n", file, line, expr);
}

#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)

void banner(const char* name) { std::printf("\n== %s ==\n", name); }

template <size_t N>
std::vector<uint8_t> blob_bytes(const uint32_t (&words)[N]) {
    std::vector<uint8_t> out(N * 4);
    for (size_t i = 0; i < N; ++i) {
        out[i * 4 + 0] = static_cast<uint8_t>(words[i] & 0xffu);
        out[i * 4 + 1] = static_cast<uint8_t>((words[i] >> 8) & 0xffu);
        out[i * 4 + 2] = static_cast<uint8_t>((words[i] >> 16) & 0xffu);
        out[i * 4 + 3] = static_cast<uint8_t>((words[i] >> 24) & 0xffu);
    }
    return out;
}

uint32_t get_u32(const std::vector<uint8_t>& b, size_t off) {
    return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
           (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
}

void put_u32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    b[off + 0] = static_cast<uint8_t>(v & 0xffu);
    b[off + 1] = static_cast<uint8_t>((v >> 8) & 0xffu);
    b[off + 2] = static_cast<uint8_t>((v >> 16) & 0xffu);
    b[off + 3] = static_cast<uint8_t>((v >> 24) & 0xffu);
}

sc_descriptor_range rng(sc_descriptor_range_kind kind, uint32_t count, uint32_t reg,
                        uint32_t space, uint32_t offset) {
    sc_descriptor_range r;
    r.kind = kind;
    r.num_descriptors = count;
    r.base_register = reg;
    r.register_space = space;
    r.offset_in_table = offset;
    return r;
}

sc_root_param consts(uint32_t reg, uint32_t space, uint32_t n, uint32_t vis) {
    sc_root_param p;
    p.kind = sc_root_param_kind::constants32;
    p.shader_register = reg;
    p.register_space = space;
    p.num_32bit = n;
    p.visibility = vis;
    return p;
}

sc_root_param root_desc_param(sc_root_param_kind kind, uint32_t reg, uint32_t space, uint32_t vis) {
    sc_root_param p;
    p.kind = kind;
    p.shader_register = reg;
    p.register_space = space;
    p.visibility = vis;
    return p;
}

sc_root_param table_param(uint32_t vis, const std::vector<sc_descriptor_range>& ranges) {
    sc_root_param p;
    p.kind = sc_root_param_kind::descriptor_table;
    p.visibility = vis;
    p.ranges = ranges;
    return p;
}

sc_static_sampler sampler_desc(uint32_t filter, uint32_t addr, uint32_t aniso, uint32_t cmp,
                               uint32_t reg, uint32_t space, uint32_t vis, float max_lod) {
    sc_static_sampler s;
    s.filter = filter;
    s.address_u = addr;
    s.address_v = addr;
    s.address_w = addr;
    s.max_anisotropy = aniso;
    s.comparison_func = cmp;
    s.shader_register = reg;
    s.register_space = space;
    s.visibility = vis;
    s.max_lod = max_lod;
    return s;
}

// SYNTHETIC PSO record in the real Phase 0 format, with a real D3D12 root signature.
sc_capture_pso_desc record_with(std::vector<uint8_t> root_signature) {
    sc_capture_pso_desc d;
    d.debug_name = "synthetic-phase6";
    d.node_mask = 1;
    d.sample_count = 1;
    d.sample_mask = 0xffffffffu;
    d.primitive_topology_type = 3; // TRIANGLE
    d.rtv_formats = {28u, 24u};    // R8G8B8A8_UNORM, R10G10B10A2_UNORM
    d.dsv_format = 45;             // D32_FLOAT

    sc_capture_input_element p0;
    p0.semantic_name = "POSITION";
    p0.format = 6;
    p0.input_slot = 0;
    d.input_layout.push_back(p0);
    sc_capture_input_element t0;
    t0.semantic_name = "TEXCOORD";
    t0.format = 16;
    t0.input_slot = 1;
    d.input_layout.push_back(t0);

    sc_capture_render_target_state rt;
    rt.format = 28;
    d.render_targets.push_back(rt);
    rt.format = 24;
    d.render_targets.push_back(rt);

    d.root_signature = std::move(root_signature);

    sc_capture_stage_blob vs;
    vs.stage = sc_stage::vertex;
    vs.bytes.assign(96, 0x11);
    d.shaders.push_back(vs);
    sc_capture_stage_blob ps;
    ps.stage = sc_stage::pixel;
    ps.bytes.assign(64, 0x5a);
    d.shaders.push_back(ps);
    return d;
}

} // namespace

int main() {
    std::printf("sc_pipeline_binding_test -- SYNTHETIC records, real D3D12 root signature bytes\n");

    const std::vector<uint8_t> b_consts = blob_bytes(test_fixtures::kBlobC1Consts);
    const std::vector<uint8_t> b_tables = blob_bytes(test_fixtures::kBlobC2Tables);
    const std::vector<uint8_t> b_full = blob_bytes(test_fixtures::kBlobC3Full);
    const std::vector<uint8_t> b_four = blob_bytes(test_fixtures::kBlobC4Four);
    const std::vector<uint8_t> b_samplers = blob_bytes(test_fixtures::kBlobC2Samplers);
    const std::vector<uint8_t> b_probe = blob_bytes(test_fixtures::kBlobProbeTwo);

    banner("parser: one root-constants parameter");
    {
        const sc_root_signature_desc d = sc_root_signature_parse(b_consts.data(), b_consts.size());
        CHECK(d.ok);
        CHECK(d.error.empty());
        CHECK(d.version == 1);
        CHECK(d.flags == 0);
        CHECK(d.params.size() == 1);
        CHECK(d.params[0].kind == sc_root_param_kind::constants32);
        CHECK(d.params[0].shader_register == 0xa1);
        CHECK(d.params[0].register_space == 0xb2);
        CHECK(d.params[0].num_32bit == 6);
        CHECK(d.params[0].visibility == 1);
        CHECK(d.params[0].ranges.empty());
        CHECK(d.samplers.empty());
    }

    banner("parser: two descriptor tables");
    {
        const sc_root_signature_desc d = sc_root_signature_parse(b_tables.data(), b_tables.size());
        CHECK(d.ok);
        CHECK(d.params.size() == 2);
        CHECK(d.params[0].kind == sc_root_param_kind::descriptor_table);
        CHECK(d.params[0].visibility == 1);
        CHECK(d.params[0].ranges.size() == 1);
        CHECK(d.params[0].ranges[0].kind == sc_descriptor_range_kind::srv);
        CHECK(d.params[0].ranges[0].num_descriptors == 8);
        CHECK(d.params[0].ranges[0].base_register == 0xa1);
        CHECK(d.params[0].ranges[0].register_space == 0xb2);
        CHECK(d.params[0].ranges[0].offset_in_table == 0);
        CHECK(d.params[1].visibility == 5);
        CHECK(d.params[1].ranges.size() == 1);
        CHECK(d.params[1].ranges[0].kind == sc_descriptor_range_kind::uav);
        CHECK(d.params[1].ranges[0].num_descriptors == 3);
        CHECK(d.params[1].ranges[0].base_register == 0xc3);
        CHECK(d.params[1].ranges[0].register_space == 0xd4);
        CHECK(d.params[1].ranges[0].offset_in_table == 5);
    }

    banner("parser: constants, a two-range table, a root SRV and a static sampler");
    {
        const sc_root_signature_desc d = sc_root_signature_parse(b_full.data(), b_full.size());
        CHECK(d.ok);
        CHECK(d.flags == 1);
        CHECK(d.params.size() == 3);
        CHECK(d.params[0].kind == sc_root_param_kind::constants32);
        CHECK(d.params[0].shader_register == 0x11);
        CHECK(d.params[0].register_space == 0x22);
        CHECK(d.params[0].num_32bit == 4);
        CHECK(d.params[1].kind == sc_root_param_kind::descriptor_table);
        CHECK(d.params[1].ranges.size() == 2);
        CHECK(d.params[1].ranges[0].kind == sc_descriptor_range_kind::srv);
        CHECK(d.params[1].ranges[0].num_descriptors == 8);
        CHECK(d.params[1].ranges[1].kind == sc_descriptor_range_kind::cbv);
        CHECK(d.params[1].ranges[1].num_descriptors == 3);
        CHECK(d.params[1].ranges[1].register_space == 0xd4);
        CHECK(d.params[1].ranges[1].offset_in_table == 8);
        CHECK(d.params[2].kind == sc_root_param_kind::srv);
        CHECK(d.params[2].shader_register == 0xe5);
        CHECK(d.params[2].register_space == 0xf6);
        CHECK(d.params[2].visibility == 0);
        CHECK(d.samplers.size() == 1);
        CHECK(d.samplers[0].filter == 0);
        CHECK(d.samplers[0].address_u == 1);
        CHECK(d.samplers[0].max_anisotropy == 1);
        CHECK(d.samplers[0].comparison_func == 8);
        CHECK(d.samplers[0].shader_register == 0x33);
        CHECK(d.samplers[0].register_space == 0x44);
        CHECK(d.samplers[0].visibility == 5);
    }

    banner("parser: four parameters, two tables of different sizes");
    {
        const sc_root_signature_desc d = sc_root_signature_parse(b_four.data(), b_four.size());
        CHECK(d.ok);
        CHECK(d.params.size() == 4);
        CHECK(d.params[0].kind == sc_root_param_kind::descriptor_table);
        CHECK(d.params[0].ranges.size() == 2);
        CHECK(d.params[0].ranges[0].kind == sc_descriptor_range_kind::cbv);
        CHECK(d.params[0].ranges[1].kind == sc_descriptor_range_kind::srv);
        CHECK(d.params[0].ranges[1].offset_in_table == 2);
        CHECK(d.params[1].kind == sc_root_param_kind::cbv);
        CHECK(d.params[1].shader_register == 0x77);
        CHECK(d.params[1].register_space == 0x88);
        CHECK(d.params[2].kind == sc_root_param_kind::descriptor_table);
        CHECK(d.params[2].ranges.size() == 1);
        CHECK(d.params[2].ranges[0].kind == sc_descriptor_range_kind::uav);
        CHECK(d.params[2].ranges[0].num_descriptors == 1);
        CHECK(d.params[3].kind == sc_root_param_kind::constants32);
        CHECK(d.params[3].num_32bit == 1);
        CHECK(d.params[3].shader_register == 0x99);
        CHECK(d.samplers.empty());
    }

    banner("parser: static samplers without parameters");
    {
        const sc_root_signature_desc d = sc_root_signature_parse(b_samplers.data(), b_samplers.size());
        CHECK(d.ok);
        CHECK(d.params.empty());
        CHECK(d.samplers.size() == 2);
        CHECK(d.samplers[0].filter == 0);
        CHECK(d.samplers[0].shader_register == 0xa1);
        CHECK(d.samplers[0].register_space == 0xb2);
        CHECK(d.samplers[0].visibility == 1);
        CHECK(d.samplers[1].filter == 0x15);
        CHECK(d.samplers[1].address_u == 3);
        CHECK(d.samplers[1].max_anisotropy == 2);
        CHECK(d.samplers[1].comparison_func == 2);
        CHECK(d.samplers[1].shader_register == 0xc3);
        CHECK(d.samplers[1].register_space == 0xd4);
        CHECK(d.samplers[1].visibility == 5);
    }

    banner("parser: flags and float fields survive the round trip");
    {
        const sc_root_signature_desc d = sc_root_signature_parse(b_probe.data(), b_probe.size());
        CHECK(d.ok);
        CHECK(d.flags == 5); // ALLOW_IA | DENY_HULL
        CHECK(d.params.size() == 2);
        CHECK(d.samplers.size() == 1);
        CHECK(d.samplers[0].max_lod == 100.0f);
        CHECK(d.samplers[0].min_lod == 0.0f);
        CHECK(d.samplers[0].mip_lod_bias == 0.0f);
    }

    banner("binding: the layout comes from the record's own root signature bytes");
    {
        sc_capture_pso_desc rec = record_with(b_full);
        const sc_pipeline_binding pb = sc_pipeline_bind(rec);
        CHECK(pb.ok);
        CHECK(pb.error.empty());
        CHECK(pb.root_desc.params.size() == 3);
        CHECK(pb.root.params.size() == 3);
        CHECK(pb.root.params[0].arg_offset_dwords == 0);
        CHECK(pb.root.params[0].arg_dwords == 4);
        CHECK(pb.root.params[1].arg_offset_dwords == 4);
        CHECK(pb.root.params[1].arg_dwords == 1);
        CHECK(pb.root.params[2].arg_offset_dwords == 5);
        CHECK(pb.root.params[2].arg_dwords == 2);
        CHECK(pb.root.root_arg_dwords == 7);
        CHECK(pb.root.tables.size() == 1);
        CHECK(pb.root.tables[0].base_descriptor == 0);
        CHECK(pb.root.tables[0].descriptors == 11);
        CHECK(pb.root.tables[0].range_count == 2);
        CHECK(pb.root.descriptor_count == 11);
        CHECK(pb.root.descriptor_bytes == 352);
        CHECK(pb.root.sampler_count == 1);
        CHECK(pb.stages.size() == 8);
        CHECK(pb.stages[0].present);
        CHECK(pb.stages[0].bytes == 96);
        CHECK(pb.stages[0].sha256 == sc_sha256_hex(rec.shaders[0].bytes.data(),
                                                   rec.shaders[0].bytes.size()));
        CHECK(pb.stages[4].present);
        CHECK(pb.rtv_count == 2);
        CHECK(pb.rtv_formats[1] == 24);
        CHECK(pb.dsv_format == 45);
        CHECK(pb.sample_mask == 0xffffffffu);
        CHECK(pb.input_slot_count == 2);
    }

    banner("binding: two tables tile one descriptor run");
    {
        const sc_pipeline_binding pb = sc_pipeline_bind(record_with(b_four));
        CHECK(pb.ok);
        CHECK(pb.root.root_arg_dwords == 5);
        CHECK(pb.root.tables.size() == 2);
        CHECK(pb.root.tables[0].base_descriptor == 0);
        CHECK(pb.root.tables[0].descriptors == 7);
        CHECK(pb.root.tables[1].base_descriptor == 7);
        CHECK(pb.root.tables[1].descriptors == 1);
        CHECK(pb.root.descriptor_count == 8);
        CHECK(pb.root.descriptor_bytes == 256);
        CHECK(pb.root.sampler_count == 0);
    }

    banner("validator: a binding parsed from the record passes");
    {
        const sc_capture_pso_desc rec = record_with(b_full);
        const sc_pipeline_binding pb = sc_pipeline_bind(rec);
        const sc_pipeline_check c = sc_pipeline_verify(rec, pb);
        CHECK(c.ok);
        CHECK(c.detail.empty());
        CHECK(c.stages_bound == 2);
        CHECK(c.stages_with_blob == 2);
        CHECK(c.rtv_formats_matched == 2);
        CHECK(c.tables_checked == 1);
        CHECK(c.ranges_checked == 2);
        CHECK(c.descriptor_overlaps == 0);
        CHECK(c.arg_offsets_monotonic == 3);
        CHECK(c.identity_matches_header);
        CHECK(c.sample_state_consistent);
        CHECK(c.input_layout_consistent);
    }

    banner("cross-check: a hand-filled description can be compared, order independently");
    {
        sc_root_signature_desc supplied;
        supplied.version = 1;
        supplied.flags = 1;
        supplied.params.push_back(consts(0x11, 0x22, 4, 1));
        supplied.params.push_back(table_param(5, {rng(sc_descriptor_range_kind::srv, 8, 0xa1, 0xb2, 0),
                                                 rng(sc_descriptor_range_kind::cbv, 3, 0xc3, 0xd4, 8)}));
        supplied.params.push_back(root_desc_param(sc_root_param_kind::srv, 0xe5, 0xf6, 0));
        supplied.samplers.push_back(sampler_desc(0, 1, 1, 8, 0x33, 0x44, 5, 0.0f));

        const sc_root_signature_desc parsed = sc_root_signature_parse(b_full.data(), b_full.size());
        std::string detail = "untouched";
        CHECK(sc_root_signature_matches(parsed, supplied, &detail));
        CHECK(detail.empty());

        sc_root_signature_desc reordered = supplied;
        std::swap(reordered.params[1].ranges[0], reordered.params[1].ranges[1]);
        CHECK(sc_root_signature_matches(parsed, reordered, &detail));

        sc_root_signature_desc wrong = supplied;
        wrong.params[2].shader_register = 0xe6;
        CHECK(!sc_root_signature_matches(parsed, wrong, &detail));
        CHECK(!detail.empty());

        sc_root_signature_desc wrong_flags = supplied;
        wrong_flags.flags = 0;
        CHECK(!sc_root_signature_matches(parsed, wrong_flags, &detail));

        sc_root_signature_desc wrong_sampler = supplied;
        wrong_sampler.samplers[0].max_lod = 1.0f;
        CHECK(!sc_root_signature_matches(parsed, wrong_sampler, &detail));
    }

    banner("root layout: range order does not change the packing");
    {
        sc_root_signature_desc forward;
        forward.params.push_back(table_param(5, {rng(sc_descriptor_range_kind::srv, 8, 0xa1, 0xb2, 0),
                                                 rng(sc_descriptor_range_kind::cbv, 3, 0xc3, 0xd4, 8)}));
        sc_root_signature_desc backward;
        backward.params.push_back(table_param(5, {rng(sc_descriptor_range_kind::cbv, 3, 0xc3, 0xd4, 8),
                                                  rng(sc_descriptor_range_kind::srv, 8, 0xa1, 0xb2, 0)}));
        const sc_root_binding a = sc_root_binding_build(forward);
        const sc_root_binding b = sc_root_binding_build(backward);
        CHECK(a.ok);
        CHECK(b.ok);
        CHECK(a.tables[0].descriptors == b.tables[0].descriptors);
        CHECK(a.tables[0].descriptors == 11);
        CHECK(a.descriptor_bytes == b.descriptor_bytes);
        CHECK(a.descriptor_bytes == 352);
    }

    const std::vector<uint8_t> b11_consts = blob_bytes(test_fixtures::kBlobV11Consts);
    const std::vector<uint8_t> b11_tables = blob_bytes(test_fixtures::kBlobV11Tables);
    const std::vector<uint8_t> b11_full = blob_bytes(test_fixtures::kBlobV11Full);
    const std::vector<uint8_t> b11_flags = blob_bytes(test_fixtures::kBlobV11Flags);
    const std::vector<uint8_t> b11_samplers = blob_bytes(test_fixtures::kBlobV11Samplers);

    banner("parser 1.1: version 2 chunk, root constants");
    {
        const sc_root_signature_desc d =
            sc_root_signature_parse(b11_consts.data(), b11_consts.size());
        CHECK(d.ok);
        CHECK(d.error.empty());
        CHECK(d.version == 2);
        CHECK(d.flags == 0);
        CHECK(d.params.size() == 1);
        CHECK(d.params[0].kind == sc_root_param_kind::constants32);
        CHECK(d.params[0].shader_register == 0xa1);
        CHECK(d.params[0].register_space == 0xb2);
        CHECK(d.params[0].num_32bit == 6);
        CHECK(d.params[0].visibility == 1);
        CHECK(d.params[0].descriptor_flags == 0);
    }

    banner("parser 1.1: tables with 24-byte ranges");
    {
        const sc_root_signature_desc d =
            sc_root_signature_parse(b11_tables.data(), b11_tables.size());
        CHECK(d.ok);
        CHECK(d.version == 2);
        CHECK(d.params.size() == 2);
        CHECK(d.params[0].kind == sc_root_param_kind::descriptor_table);
        CHECK(d.params[0].ranges.size() == 1);
        CHECK(d.params[0].ranges[0].kind == sc_descriptor_range_kind::srv);
        CHECK(d.params[0].ranges[0].num_descriptors == 8);
        CHECK(d.params[0].ranges[0].base_register == 0xa1);
        CHECK(d.params[0].ranges[0].register_space == 0xb2);
        CHECK(d.params[0].ranges[0].flags == 0);
        CHECK(d.params[0].ranges[0].offset_in_table == 0);
        CHECK(d.params[1].ranges.size() == 1);
        CHECK(d.params[1].ranges[0].kind == sc_descriptor_range_kind::uav);
        CHECK(d.params[1].ranges[0].num_descriptors == 3);
        CHECK(d.params[1].ranges[0].offset_in_table == 5);
    }

    banner("parser 1.1: range and root-descriptor flags");
    {
        const sc_root_signature_desc d =
            sc_root_signature_parse(b11_flags.data(), b11_flags.size());
        CHECK(d.ok);
        CHECK(d.version == 2);
        CHECK(d.params.size() == 2);
        CHECK(d.params[0].kind == sc_root_param_kind::descriptor_table);
        CHECK(d.params[0].visibility == 0);
        CHECK(d.params[0].ranges.size() == 1);
        CHECK(d.params[0].ranges[0].num_descriptors == 4);
        CHECK(d.params[0].ranges[0].base_register == 0x77);
        CHECK(d.params[0].ranges[0].register_space == 0x88);
        CHECK(d.params[0].ranges[0].flags == 2); // DATA_VOLATILE
        CHECK(d.params[0].ranges[0].offset_in_table == 0);
        CHECK(d.params[1].kind == sc_root_param_kind::cbv);
        CHECK(d.params[1].shader_register == 0x99);
        CHECK(d.params[1].register_space == 0xaa);
        CHECK(d.params[1].descriptor_flags == 4); // DATA_STATIC_WHILE_SET_AT_EXECUTE
        CHECK(d.params[1].visibility == 5);
    }

    banner("parser 1.1: samplers keep the 1.0 encoding");
    {
        const sc_root_signature_desc d =
            sc_root_signature_parse(b11_samplers.data(), b11_samplers.size());
        CHECK(d.ok);
        CHECK(d.params.empty());
        CHECK(d.samplers.size() == 2);
        CHECK(d.samplers[0].filter == 0);
        CHECK(d.samplers[0].shader_register == 0xa1);
        CHECK(d.samplers[0].register_space == 0xb2);
        CHECK(d.samplers[0].visibility == 1);
        CHECK(d.samplers[1].filter == 0x15);
        CHECK(d.samplers[1].address_u == 3);
        CHECK(d.samplers[1].max_anisotropy == 2);
        CHECK(d.samplers[1].shader_register == 0xc3);
        CHECK(d.samplers[1].visibility == 5);
    }

    banner("parser 1.1: the full three-parameter blob binds like its 1.0 twin");
    {
        const sc_capture_pso_desc rec = record_with(b11_full);
        const sc_pipeline_binding pb = sc_pipeline_bind(rec);
        CHECK(pb.ok);
        CHECK(pb.root_desc.version == 2);
        CHECK(pb.root_desc.params.size() == 3);
        CHECK(pb.root.root_arg_dwords == 7);
        CHECK(pb.root.tables.size() == 1);
        CHECK(pb.root.tables[0].base_descriptor == 0);
        CHECK(pb.root.tables[0].descriptors == 11);
        CHECK(pb.root.descriptor_bytes == 352);
        CHECK(pb.root.sampler_count == 1);
        const sc_pipeline_check c = sc_pipeline_verify(rec, pb);
        CHECK(c.ok);
        CHECK(c.tables_checked == 1);
        CHECK(c.ranges_checked == 2);
        CHECK(c.identity_matches_header);

        const sc_pipeline_binding pb_flags = sc_pipeline_bind(record_with(b11_flags));
        CHECK(pb_flags.ok);
        CHECK(pb_flags.root.root_arg_dwords == 3);
        CHECK(pb_flags.root.descriptor_count == 4);
        CHECK(pb_flags.root.descriptor_bytes == 128);
    }

    banner("cross-version: one desc serialized by both APIs decodes identically");
    {
        std::string detail = "untouched";
        const sc_root_signature_desc parsed_1_0 =
            sc_root_signature_parse(b_full.data(), b_full.size());
        const sc_root_signature_desc parsed_1_1 =
            sc_root_signature_parse(b11_full.data(), b11_full.size());
        CHECK(parsed_1_0.version == 1);
        CHECK(parsed_1_1.version == 2);
        CHECK(sc_root_signature_matches(parsed_1_0, parsed_1_1, &detail));
        CHECK(detail.empty());

        const sc_root_signature_desc parsed_tables_1_0 =
            sc_root_signature_parse(b_tables.data(), b_tables.size());
        const sc_root_signature_desc parsed_tables_1_1 =
            sc_root_signature_parse(b11_tables.data(), b11_tables.size());
        CHECK(sc_root_signature_matches(parsed_tables_1_0, parsed_tables_1_1, &detail));
    }

    banner("parser: malformed blobs are refused");
    {
        CHECK(!sc_root_signature_parse(nullptr, 0).ok);
        CHECK(!sc_root_signature_parse(b_consts.data(), b_consts.size() - 4).ok);

        std::vector<uint8_t> bad_type = b_consts;
        put_u32(bad_type, 68, 9); // parameter 0 entry starts at chunk offset 24
        const sc_root_signature_desc d_type =
            sc_root_signature_parse(bad_type.data(), bad_type.size());
        CHECK(!d_type.ok);
        CHECK(d_type.error.find("unknown type") != std::string::npos);

        std::vector<uint8_t> bad_payload = b_consts;
        put_u32(bad_payload, 76, 0xffffu);
        CHECK(!sc_root_signature_parse(bad_payload.data(), bad_payload.size()).ok);

        std::vector<uint8_t> no_values = b_consts;
        const uint32_t payload = get_u32(b_consts, 76);
        put_u32(no_values, 44 + payload + 8, 0);
        const sc_root_signature_desc d_values =
            sc_root_signature_parse(no_values.data(), no_values.size());
        CHECK(!d_values.ok);
        CHECK(d_values.error.find("no 32-bit values") != std::string::npos);

        std::vector<uint8_t> no_ranges = b_tables;
        put_u32(no_ranges, 44 + get_u32(b_tables, 76), 0);
        CHECK(!sc_root_signature_parse(no_ranges.data(), no_ranges.size()).ok);

        std::vector<uint8_t> bad_samplers = b_samplers;
        put_u32(bad_samplers, 60, 0xffffu); // samplers section offset
        CHECK(!sc_root_signature_parse(bad_samplers.data(), bad_samplers.size()).ok);
    }

    banner("validator: overlapping ranges in a corrupted blob are caught, either order");
    {
        std::vector<uint8_t> overlapping = b_full;
        put_u32(overlapping, 160, 4); // CBV range moves from 8 to 4, inside the SRV range
        const sc_capture_pso_desc rec = record_with(overlapping);
        const sc_pipeline_binding pb = sc_pipeline_bind(rec);
        CHECK(!pb.ok);
        CHECK(pb.error.find("overlap") != std::string::npos);

        const sc_pipeline_check c = sc_pipeline_verify(rec, pb);
        CHECK(!c.ok);
        CHECK(c.descriptor_overlaps > 0);

        sc_pipeline_binding flipped = pb;
        std::swap(flipped.root_desc.params[1].ranges[0], flipped.root_desc.params[1].ranges[1]);
        const sc_pipeline_check c2 = sc_pipeline_verify(rec, flipped);
        CHECK(c2.descriptor_overlaps == c.descriptor_overlaps);
    }

    banner("validator: corrupted bindings and records are refused");
    {
        const sc_capture_pso_desc rec = record_with(b_full);
        const sc_pipeline_binding good = sc_pipeline_bind(rec);
        CHECK(good.ok);

        sc_pipeline_binding bad_format = good;
        bad_format.rtv_formats[1] = 99;
        CHECK(!sc_pipeline_verify(rec, bad_format).ok);

        sc_pipeline_binding dropped = good;
        dropped.stages[0].present = false;
        CHECK(!sc_pipeline_verify(rec, dropped).ok);

        sc_capture_pso_desc more_targets = rec;
        more_targets.render_targets.push_back(sc_capture_render_target_state{});
        CHECK(!sc_pipeline_verify(more_targets, good).ok);

        sc_capture_pso_desc other_sample = rec;
        other_sample.sample_count = 4;
        CHECK(!sc_pipeline_verify(other_sample, good).ok);

        sc_capture_pso_desc other_formats = rec;
        other_formats.rtv_formats[0] = 10;
        CHECK(!sc_pipeline_verify(other_formats, good).ok);
    }

    banner("binding: incomplete or malformed records are refused");
    {
        sc_capture_pso_desc no_stage = record_with(b_full);
        no_stage.shaders.clear();
        CHECK(!sc_pipeline_bind(no_stage).ok);

        sc_capture_pso_desc unknown_stage = record_with(b_full);
        unknown_stage.shaders[1].stage = sc_stage::unknown;
        CHECK(!sc_pipeline_bind(unknown_stage).ok);

        sc_capture_pso_desc no_root = record_with({});
        const sc_pipeline_binding pb_root = sc_pipeline_bind(no_root);
        CHECK(!pb_root.ok);
        CHECK(pb_root.error.find("root signature") != std::string::npos);

        sc_capture_pso_desc many_targets = record_with(b_full);
        for (int i = 0; i < 8; ++i) {
            many_targets.rtv_formats.push_back(28);
            many_targets.render_targets.push_back(sc_capture_render_target_state{});
        }
        CHECK(!sc_pipeline_bind(many_targets).ok);

        std::vector<uint8_t> bad_type = b_consts;
        put_u32(bad_type, 68, 9);
        sc_capture_pso_desc corrupt = record_with(bad_type);
        const sc_pipeline_binding pb_corrupt = sc_pipeline_bind(corrupt);
        CHECK(!pb_corrupt.ok);
        CHECK(pb_corrupt.error.find("parse") != std::string::npos);
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
    if (g_fail != 0) {
        std::printf("PHASE 6 HARNESS FAILED\n");
        return 1;
    }
    std::printf("phase 6 harness: all synthetic checks passed\n");
    return 0;
}
