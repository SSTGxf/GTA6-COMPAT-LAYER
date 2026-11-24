// ScarlettCompat shader pipeline - standalone test harness.
// MIT. Runs without the game, without a D3D12 device and without the layer.
//
//   sc_shader_tests.exe [output-dir]
//
// Covers: SHA-256/FNV-1a, DXBC/DXIL container parsing (synthetic + real DXC
// output), malformed/truncated input safety, capture de-duplication, corpus
// dump round-trip, JSON escaping, and the dxc.exe DXIL-emission seam.
#include "shader/sc_capture.h"
#include "shader/sc_container.h"
#include "shader/sc_dxil_emit.h"
#include "shader/sc_sha256.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using namespace sc;

namespace {

int g_checks = 0;
int g_pass = 0;
int g_fail = 0;

void check(bool condition, const char* expr, const char* file, int line) {
    ++g_checks;
    if (condition) {
        ++g_pass;
        return;
    }
    ++g_fail;
    std::printf("FAIL %s:%d  %s\n", file, line, expr);
}

#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)

void banner(const char* name) { std::printf("\n== %s ==\n", name); }

// ---------------------------------------------------------------- file helpers

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        return false;
    }
    const std::streamsize n = f.tellg();
    if (n < 0) {
        return false;
    }
    out.resize(static_cast<size_t>(n));
    f.seekg(0);
    if (n > 0) {
        f.read(reinterpret_cast<char*>(out.data()), n);
    }
    return true;
}

std::string read_text(const std::string& path) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, bytes)) {
        return std::string();
    }
    return std::string(bytes.begin(), bytes.end());
}

// ------------------------------------------------------- synthetic containers

struct chunk_spec {
    std::string name; // exactly 4 characters
    std::vector<uint8_t> data;
};

void wr32(std::vector<uint8_t>& v, size_t offset, uint32_t value) {
    v[offset + 0] = static_cast<uint8_t>(value);
    v[offset + 1] = static_cast<uint8_t>(value >> 8);
    v[offset + 2] = static_cast<uint8_t>(value >> 16);
    v[offset + 3] = static_cast<uint8_t>(value >> 24);
}

std::vector<uint8_t> u32s(std::initializer_list<uint32_t> values) {
    std::vector<uint8_t> v(values.size() * 4, 0);
    size_t i = 0;
    for (uint32_t value : values) {
        wr32(v, i * 4, value);
        ++i;
    }
    return v;
}

std::vector<uint8_t> build_container(const std::vector<chunk_spec>& chunks) {
    const uint32_t header_size = 32;
    std::vector<uint32_t> offsets;
    uint32_t cursor = header_size + static_cast<uint32_t>(chunks.size()) * 4u;
    for (const auto& c : chunks) {
        offsets.push_back(cursor);
        const uint32_t payload = static_cast<uint32_t>(c.data.size());
        cursor += 8u + ((payload + 3u) & ~3u);
    }
    std::vector<uint8_t> out(cursor, 0);
    out[0] = 'D';
    out[1] = 'X';
    out[2] = 'B';
    out[3] = 'C';
    wr32(out, 20, 1); // header version
    wr32(out, 24, cursor);
    wr32(out, 28, static_cast<uint32_t>(chunks.size()));
    for (size_t i = 0; i < chunks.size(); ++i) {
        const uint32_t off = offsets[i];
        wr32(out, header_size + i * 4u, off);
        std::memcpy(out.data() + off, chunks[i].name.data(), 4);
        wr32(out, off + 4, static_cast<uint32_t>(chunks[i].data.size()));
        if (!chunks[i].data.empty()) {
            std::memcpy(out.data() + off + 8, chunks[i].data.data(), chunks[i].data.size());
        }
    }
    return out;
}

chunk_spec make_chunk(const char* name, std::vector<uint8_t> data) {
    chunk_spec c;
    c.name = name;
    c.data = std::move(data);
    return c;
}

// A plausible SM5-style vertex container: ISGN + OSGN + SHEX (program type 1).
std::vector<uint8_t> synthetic_sm5_vertex() {
    std::vector<chunk_spec> chunks;
    chunks.push_back(make_chunk("ISGN", u32s({0x00000001, 0x00000008})));
    chunks.push_back(make_chunk("OSGN", u32s({0x00000001, 0x00000008})));
    // token: type 1 (vertex) | major 4 << 20
    chunks.push_back(make_chunk("SHEX", u32s({0x00400001u, 0x00000010, 0xDEADBEEF})));
    return build_container(chunks);
}

// A DXIL container in the shape dxc 1.9 emits: a DXIL chunk whose version
// token carries the shader model (low 16 bits, 0x67 == 6.7) and the stage
// (bits 16-23), plus a PSV0 chunk that must NOT be used for stage detection.
std::vector<uint8_t> synthetic_dxil(uint32_t stage_index) {
    std::vector<uint8_t> dxil_chunk = u32s({0x00000067u | (stage_index << 16), 0x00000008u, 0x00000107u});
    const std::vector<uint8_t> bitcode = {'B', 'C', 0xc0, 0xde, 0x00, 0x00, 0x00, 0x00};
    dxil_chunk.insert(dxil_chunk.end(), bitcode.begin(), bitcode.end());

    std::vector<chunk_spec> chunks;
    chunks.push_back(make_chunk("DXIL", dxil_chunk));
    // Deliberately misleading PSV0 (dword1 = 0, which is what a pixel shader has).
    chunks.push_back(make_chunk("PSV0", u32s({0x00000034, 0x00000000, 0x00000000, 0x00000000})));
    return build_container(chunks);
}

// ------------------------------------------------------------------ test cases

void test_sha256() {
    banner("sha256 / fnv1a");
    CHECK(sc_sha256_hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sc_sha256_hex(nullptr, 0) == sc_sha256_hex("", 0));
    CHECK(sc_sha256_hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(sc_sha256_hex(m, std::strlen(m)) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    std::vector<char> million(1000000, 'a');
    CHECK(sc_sha256_hex(million.data(), million.size()) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    const uint64_t h1 = sc_fnv1a64("abc", 3);
    const uint64_t h2 = sc_fnv1a64("abd", 3);
    CHECK(h1 != h2);
    CHECK(h1 == sc_fnv1a64("abc", 3));

    sc_fnv1a64_stream a;
    a.add("ab", 2);
    a.add("c", 1);
    CHECK(a.value() == h1);
}

void test_container_sm5_vertex() {
    banner("container: synthetic SM5 vertex");
    const std::vector<uint8_t> blob = synthetic_sm5_vertex();
    sc_container_info info;
    CHECK(sc_parse_container(blob.data(), blob.size(), info));
    CHECK(info.ok);
    CHECK(info.format == sc_container_format::dxbc_sm5);
    CHECK(info.chunk_count == 3);
    CHECK(info.chunks.size() == 3);
    CHECK(!info.has_dxil);
    CHECK(info.has_shex);
    CHECK(info.stage == sc_stage::vertex);
    CHECK(info.stage_from == sc_stage_source::shader_token);
    CHECK(info.has_program_version);
    CHECK(info.program_major == 4);
    CHECK(info.program_minor == 0);
    CHECK(info.total_size == blob.size());
    std::printf("   chunks=\"%s\" stage=%s source=%s version=%u.%u\n", info.chunk_list().c_str(),
                sc_stage_name(info.stage), sc_stage_source_name(info.stage_from), info.program_major,
                info.program_minor);
}

void test_container_stage_from_dxil_token() {
    banner("container: stage from the DXIL version token (compute, mesh, vertex)");
    sc_container_info info;
    std::vector<uint8_t> blob = synthetic_dxil(5);
    CHECK(sc_parse_container(blob.data(), blob.size(), info));
    CHECK(info.format == sc_container_format::dxbc);
    CHECK(info.has_dxil);
    CHECK(info.stage == sc_stage::compute);
    CHECK(info.stage_from == sc_stage_source::shader_token);
    CHECK(info.has_program_version);
    CHECK(info.program_major == 6);
    CHECK(info.program_minor == 7);

    // DXIL::ShaderKind numbering: mesh is 13, amplification 14 (not 6/7).
    blob = synthetic_dxil(13);
    CHECK(sc_parse_container(blob.data(), blob.size(), info));
    CHECK(info.stage == sc_stage::mesh);
    blob = synthetic_dxil(14);
    CHECK(sc_parse_container(blob.data(), blob.size(), info));
    CHECK(info.stage == sc_stage::amplification);
    // 6 is the shader-library kind, not a pipeline stage: must stay unknown.
    blob = synthetic_dxil(6);
    CHECK(sc_parse_container(blob.data(), blob.size(), info));
    CHECK(info.stage == sc_stage::unknown);

    blob = synthetic_dxil(1);
    CHECK(sc_parse_container(blob.data(), blob.size(), info));
    CHECK(info.stage == sc_stage::vertex);
    CHECK(sc_stage_is_graphics(info.stage));
    CHECK(!sc_stage_is_graphics(sc_stage::compute));

    // Geometry and pixel land in the token too.
    blob = synthetic_dxil(2);
    CHECK(sc_parse_container(blob.data(), blob.size(), info));
    CHECK(info.stage == sc_stage::geometry);
    blob = synthetic_dxil(0);
    CHECK(sc_parse_container(blob.data(), blob.size(), info));
    CHECK(info.stage == sc_stage::pixel);
}

void test_container_robustness() {
    banner("container: malformed and truncated input");
    const std::vector<uint8_t> good = synthetic_sm5_vertex();

    // Every truncation must be rejected without reading out of bounds.
    int rejected = 0;
    for (size_t len = 0; len < good.size(); ++len) {
        sc_container_info info;
        if (!sc_parse_container(good.data(), len, info)) {
            ++rejected;
            if (info.error.empty()) {
                CHECK(!"rejected without an error message");
            }
        }
    }
    CHECK(rejected == static_cast<int>(good.size()));
    std::printf("   rejected %d/%zu truncations\n", rejected, good.size());

    sc_container_info info;
    const uint8_t garbage[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(!sc_parse_container(garbage, sizeof(garbage), info));
    CHECK(!info.ok);

    // Zero chunk count.
    std::vector<uint8_t> blob = good;
    wr32(blob, 28, 0);
    CHECK(!sc_parse_container(blob.data(), blob.size(), info));

    // Absurd chunk count.
    blob = good;
    wr32(blob, 28, 0xffffffffu);
    CHECK(!sc_parse_container(blob.data(), blob.size(), info));

    // Chunk payload beyond the container.
    blob = good;
    const uint32_t first_chunk = static_cast<uint32_t>(blob[32]) | (static_cast<uint32_t>(blob[33]) << 8) |
                                 (static_cast<uint32_t>(blob[34]) << 16) |
                                 (static_cast<uint32_t>(blob[35]) << 24);
    wr32(blob, first_chunk + 4, 0x7fffffffu);
    CHECK(!sc_parse_container(blob.data(), blob.size(), info));

    // Declared total larger than the buffer.
    blob = good;
    wr32(blob, 24, static_cast<uint32_t>(blob.size()) + 64);
    CHECK(!sc_parse_container(blob.data(), blob.size(), info));

    // Raw DXIL bitcode is accepted as its own format.
    const uint8_t bitcode[8] = {'B', 'C', 0xc0, 0xde, 0, 0, 0, 0};
    CHECK(sc_parse_container(bitcode, sizeof(bitcode), info));
    CHECK(info.format == sc_container_format::dxil_bitcode);
}

void test_locate_container() {
    banner("container: located inside a decorated blob");
    const std::vector<uint8_t> inner = synthetic_dxil(1);
    std::vector<uint8_t> outer(64, 0xab);
    outer.insert(outer.end(), inner.begin(), inner.end());

    size_t offset = 0;
    CHECK(sc_locate_container(outer.data(), outer.size(), 0, offset));
    CHECK(offset == 64);

    sc_container_info info;
    CHECK(!sc_parse_container(outer.data(), outer.size(), info)); // not at 0
    CHECK(sc_parse_container(outer.data() + offset, outer.size() - offset, info));
    CHECK(info.stage == sc_stage::vertex);
}

std::vector<uint8_t> fake_root_signature() {
    std::vector<uint8_t> sig(40, 0x5a);
    sig[0] = 'R';
    sig[1] = 'T';
    sig[2] = 'S';
    sig[3] = '0';
    return sig;
}

sc_capture_pso_desc make_desc(const std::vector<uint8_t>& vs, const std::vector<uint8_t>& ps, uint32_t sample_count) {
    sc_capture_pso_desc d;
    d.source_slot = 86;
    d.sample_count = sample_count;
    d.primitive_topology_type = 3; // triangle list
    d.rtv_formats.push_back(28);   // DXGI_FORMAT_R8G8B8A8_UNORM
    d.dsv_format = 45;             // DXGI_FORMAT_D32_FLOAT
    d.render_targets.push_back(sc_capture_render_target_state{});

    sc_capture_input_element e;
    e.semantic_name = "POSITION";
    e.format = 6; // R32G32B32_FLOAT
    d.input_layout.push_back(e);
    e.semantic_name = "TEXCOORD";
    e.semantic_index = 1;
    e.format = 16;
    d.input_layout.push_back(e);

    d.root_signature = fake_root_signature();

    sc_capture_stage_blob vsb;
    vsb.stage = sc_stage::vertex;
    vsb.bytes = vs;
    d.shaders.push_back(vsb);

    sc_capture_stage_blob psb;
    psb.stage = sc_stage::pixel;
    psb.bytes = ps;
    d.shaders.push_back(psb);
    return d;
}

void test_capture_store(const std::string& out_dir) {
    banner("capture: de-duplication and signature stability");
    const std::vector<uint8_t> vs = synthetic_sm5_vertex();
    const std::vector<uint8_t> ps = synthetic_dxil(0);

    sc_capture_store store;
    const sc_capture_pso_desc a = make_desc(vs, ps, 1);
    const sc_capture_pso_desc a2 = make_desc(vs, ps, 1);
    const sc_capture_pso_desc b = make_desc(vs, ps, 4);

    CHECK(sc_capture_signature(a) == sc_capture_signature(a2));
    CHECK(sc_capture_signature(a) != sc_capture_signature(b));

    sc_capture_add_result r = store.add(a);
    CHECK(r.added);
    CHECK(r.seq == 0);
    CHECK(r.hits == 1);

    r = store.add(a2);
    CHECK(!r.added);
    CHECK(r.seq == 0);
    CHECK(r.hits == 2);

    r = store.add(b);
    CHECK(r.added);
    CHECK(r.seq == 1);

    r = store.add(a);
    CHECK(!r.added);
    CHECK(r.hits == 3);

    CHECK(store.size() == 2);
    CHECK(store.adds() == 4);
    CHECK(store.duplicates() == 2);
    CHECK(store.records()[0].stages == "vertex,pixel");
    CHECK(store.records()[0].shaders.size() == 2);
    CHECK(store.records()[0].shaders[0].stage == sc_stage::vertex);
    CHECK(!store.records()[0].shaders[0].has_dxil);
    CHECK(store.records()[0].shaders[0].chunks.find("SHEX") != std::string::npos);
    CHECK(store.records()[0].shaders[1].stage == sc_stage::pixel);
    CHECK(store.records()[0].total_shader_bytes() == vs.size() + ps.size());
    CHECK(store.records()[0].desc.root_signature.size() == 40);
    std::printf("   unique=%zu adds=%llu duplicates=%llu sig=%s hist=%s\n", store.size(),
                static_cast<unsigned long long>(store.adds()), static_cast<unsigned long long>(store.duplicates()),
                store.records()[0].signature_hex().c_str(), store.stage_histogram_json().c_str());

    banner("capture: dump round-trip");
    const std::string dir = out_dir + "/corpus_basic";
    const sc_capture_dump_report rep = sc_capture_dump(store, dir);
    CHECK(rep.ok);
    CHECK(rep.records == 2);
    CHECK(rep.files >= 6);
    CHECK(rep.bytes > 0);
    if (!rep.ok) {
        for (const auto& e : rep.errors) {
            std::printf("   dump error: %s\n", e.c_str());
        }
    }

    const std::string manifest = read_text(dir + "/manifest.json");
    CHECK(!manifest.empty());
    CHECK(manifest.find("\"unique_psos\":2") != std::string::npos);
    CHECK(manifest.find("\"duplicates\":2") != std::string::npos);
    CHECK(manifest.find("\"stage_histogram\":{\"vertex\":2,\"pixel\":2}") != std::string::npos);

    const std::string index = read_text(dir + "/index.tsv");
    int lines = 0;
    for (char c : index) {
        if (c == '\n') {
            ++lines;
        }
    }
    CHECK(lines == 3); // header + 2 unique records

    // Resolve the first record directory from index.tsv and verify payload files.
    std::string first_dir;
    {
        const size_t first_line_end = index.find('\n');
        const size_t second_line_end = index.find('\n', first_line_end + 1);
        const std::string row = index.substr(first_line_end + 1, second_line_end - first_line_end - 1);
        const size_t tab = row.rfind('\t');
        CHECK(tab != std::string::npos);
        if (tab != std::string::npos) {
            first_dir = row.substr(tab + 1);
        }
    }
    CHECK(!first_dir.empty());
    std::vector<uint8_t> vs_file;
    CHECK(read_file(dir + "/" + first_dir + "/vs.bin", vs_file));
    CHECK(vs_file.size() == vs.size());
    CHECK(vs_file == vs);
    std::vector<uint8_t> ps_file;
    CHECK(read_file(dir + "/" + first_dir + "/ps.bin", ps_file));
    CHECK(ps_file.size() == ps.size());
    std::vector<uint8_t> rs_file;
    CHECK(read_file(dir + "/" + first_dir + "/rootsig.bin", rs_file));
    CHECK(rs_file.size() == 40);
    const std::string record_json = read_text(dir + "/" + first_dir + "/record.json");
    CHECK(record_json.find("\"sha256\":") != std::string::npos);
    CHECK(record_json.find("\"has_dxil\":true") != std::string::npos);
    std::printf("   wrote %u files, %llu bytes under %s\n", rep.files,
                static_cast<unsigned long long>(rep.bytes), dir.c_str());
}

void test_json_escaping(const std::string& out_dir) {
    banner("capture: JSON escaping");
    sc_capture_pso_desc d = make_desc(synthetic_dxil(1), synthetic_dxil(0), 1);
    d.debug_name = "weird \"name\" with \\ backslash";
    d.input_layout[0].semantic_name = "POS\"ITION\\X";
    d.input_layout[0].semantic_index = 2;

    sc_capture_store store;
    store.add(d);
    const std::string json = sc_capture_record_json(store.records()[0]);
    CHECK(json.find("POS\\\"ITION\\\\X") != std::string::npos);
    CHECK(json.find("weird \\\"name\\\" with \\\\ backslash") != std::string::npos);
    CHECK(json.find("\"index\":2") != std::string::npos);

    const sc_capture_dump_report rep = sc_capture_dump(store, out_dir + "/corpus_escape");
    CHECK(rep.ok);
}

void test_dxc_emit(const std::string& out_dir) {
    banner("dxil: dxc.exe emission seam");
    const sc_dxil_backend_info probe = sc_dxil_probe();
    std::printf("   probe: available=%d name=%s\n", probe.available ? 1 : 0, probe.name.c_str());
    std::printf("   path=%s\n", probe.path.c_str());
    std::printf("   version=%s\n", probe.version.c_str());
    if (!probe.note.empty()) {
        std::printf("   note=%s\n", probe.note.c_str());
    }
    if (!probe.available) {
        std::printf("   SKIP: no DXC on this machine; the emitter seam stays a noop backend\n");
        sc_noop_emitter noop("not installed");
        sc_dxil_result r;
        sc_dxil_request req;
        CHECK(!noop.compile(req, r));
        CHECK(!r.ok);
        return;
    }

    std::unique_ptr<sc_dxil_emitter> emitter = sc_make_dxil_emitter();
    CHECK(emitter != nullptr);
    CHECK(emitter->info().available);

    sc_dxil_request vs_req;
    vs_req.profile = "vs_6_7";
    vs_req.entry = "main";
    vs_req.hlsl =
        "struct VSOut { float4 pos : SV_Position; };\n"
        "VSOut main(uint id : SV_VertexID) {\n"
        "  VSOut o;\n"
        "  float2 p = float2((id << 1) & 2, id & 2);\n"
        "  o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "  return o;\n"
        "}\n";

    sc_dxil_result vs_res;
    const bool vs_ok = emitter->compile(vs_req, vs_res);
    CHECK(vs_ok);
    if (!vs_ok) {
        std::printf("   dxc failed: %s\n", vs_res.log.c_str());
        return;
    }
    CHECK(!vs_res.dxil.empty());

    sc_container_info vs_info;
    CHECK(sc_parse_container(vs_res.dxil.data(), vs_res.dxil.size(), vs_info));
    CHECK(vs_info.has_dxil);
    CHECK(vs_info.chunk_count >= 2);
    std::printf("   VS: %zu bytes, chunks=\"%s\", container stage=%s (%s)\n", vs_res.dxil.size(),
                vs_info.chunk_list().c_str(), sc_stage_name(vs_info.stage),
                sc_stage_source_name(vs_info.stage_from));

    // If the container identified a stage at all, it must agree with the truth.
    if (vs_info.stage != sc_stage::unknown) {
        CHECK(vs_info.stage == sc_stage::vertex);
    }

    sc_dxil_request ps_req;
    ps_req.profile = "ps_6_7";
    ps_req.entry = "main";
    ps_req.hlsl = "float4 main() : SV_Target { return float4(1.0, 0.5, 0.25, 1.0); }\n";
    sc_dxil_result ps_res;
    CHECK(emitter->compile(ps_req, ps_res));
    CHECK(!ps_res.dxil.empty());
    sc_container_info ps_info;
    CHECK(sc_parse_container(ps_res.dxil.data(), ps_res.dxil.size(), ps_info));
    CHECK(ps_info.has_dxil);
    if (ps_info.stage != sc_stage::unknown) {
        CHECK(ps_info.stage == sc_stage::pixel);
    }
    std::printf("   PS: %zu bytes, container stage=%s (%s)\n", ps_res.dxil.size(),
                sc_stage_name(ps_info.stage), sc_stage_source_name(ps_info.stage_from));

    banner("dxil: real DXIL through the capture path");
    sc_capture_store store;
    sc_capture_pso_desc d;
    d.source_slot = 86;
    d.rtv_formats.push_back(28);
    d.primitive_topology_type = 3;
    sc_capture_stage_blob vsb;
    vsb.stage = sc_stage::vertex;
    vsb.bytes = vs_res.dxil;
    d.shaders.push_back(vsb);
    sc_capture_stage_blob psb;
    psb.stage = sc_stage::pixel;
    psb.bytes = ps_res.dxil;
    d.shaders.push_back(psb);
    d.root_signature = fake_root_signature();

    const sc_capture_add_result ar = store.add(d);
    CHECK(ar.added);
    const sc_capture_record& rec = store.records()[0];
    CHECK(rec.shaders.size() == 2);
    CHECK(rec.shaders[0].has_dxil);
    CHECK(rec.shaders[0].format == "dxbc+dxil");
    CHECK(!rec.shaders[0].sha256.empty());
    CHECK(rec.shaders[0].sha256 != rec.shaders[1].sha256);

    const std::string dir = out_dir + "/corpus_dxc";
    const sc_capture_dump_report rep = sc_capture_dump(store, dir);
    CHECK(rep.ok);
    CHECK(rep.records == 1);
    std::printf("   corpus: %u files, %llu bytes under %s\n", rep.files,
                static_cast<unsigned long long>(rep.bytes), dir.c_str());
    std::printf("   VS sha256=%s\n   PS sha256=%s\n", rec.shaders[0].sha256.c_str(), rec.shaders[1].sha256.c_str());
}

} // namespace

int main(int argc, char** argv) {
    std::string out_dir = (argc > 1) ? argv[1] : "out/shader_tests";
    std::printf("ScarlettCompat shader pipeline tests (standalone, no device, no game)\n");
    std::printf("output dir: %s\n", out_dir.c_str());
    sc_make_dirs(out_dir);

    test_sha256();
    test_container_sm5_vertex();
    test_container_stage_from_dxil_token();
    test_container_robustness();
    test_locate_container();
    test_capture_store(out_dir);
    test_json_escaping(out_dir);
    test_dxc_emit(out_dir);

    std::printf("\n%d checks, %d passed, %d failed\n", g_checks, g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
