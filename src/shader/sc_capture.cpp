// ScarlettCompat shader pipeline - Phase 0 capture subsystem (MIT, clean-room).
#include "shader/sc_capture.h"

#include "shader/sc_sha256.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace sc {
namespace {

void json_esc(std::string& out, const std::string& s) {
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

void json_key(std::string& out, const char* key, const std::string& value) {
    out.push_back('"');
    out += key;
    out += "\":";
    json_esc(out, value);
}

std::string hex16(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return std::string(buf);
}

std::string utc_now() {
    std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return std::string(buf);
}

uint32_t float_bits(float f) {
    uint32_t v = 0;
    std::memcpy(&v, &f, sizeof(v));
    return v;
}

std::string shader_file_name(const sc_capture_pso_desc& desc, size_t index) {
    const std::string tag = sc_stage_tag(desc.shaders[index].stage);
    std::string file = tag + ".bin";
    size_t seen = 0;
    for (size_t i = 0; i < index; ++i) {
        if (std::string(sc_stage_tag(desc.shaders[i].stage)) == tag) {
            ++seen;
        }
    }
    if (seen != 0) {
        file = tag + "_" + std::to_string(seen + 1) + ".bin";
    }
    return file;
}

std::string join_stages(const sc_capture_pso_desc& desc) {
    // Canonical order (sc_stage numeric) so the string is stable.
    std::vector<sc_stage> stages;
    for (const auto& s : desc.shaders) {
        if (s.stage != sc_stage::unknown) {
            stages.push_back(s.stage);
        }
    }
    std::sort(stages.begin(), stages.end(), [](sc_stage a, sc_stage b) {
        return static_cast<uint32_t>(a) < static_cast<uint32_t>(b);
    });
    stages.erase(std::unique(stages.begin(), stages.end()), stages.end());
    std::string out;
    for (size_t i = 0; i < stages.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        out += sc_stage_name(stages[i]);
    }
    return out;
}

} // namespace

std::string sc_capture_record::signature_hex() const { return hex16(signature); }

size_t sc_capture_record::total_shader_bytes() const {
    size_t total = 0;
    for (const auto& s : shaders) {
        total += s.size;
    }
    return total;
}

uint64_t sc_capture_signature(const sc_capture_pso_desc& d) {
    sc_fnv1a64_stream h;
    static const char kMagic[8] = {'S', 'C', 'P', 'S', 'O', '0', '0', '1'};
    h.add(kMagic, sizeof(kMagic));

    h.add_u32(d.node_mask);
    h.add_u32(d.flags);
    h.add_u32(d.sample_count);
    h.add_u32(d.sample_quality);
    h.add_u32(d.sample_mask);
    h.add_u32(d.ib_strip_cut_value);
    h.add_u32(d.patch_control_points);
    h.add_u32(d.primitive_topology_type);
    h.add_u32(d.dsv_format);

    h.add_u32(static_cast<uint32_t>(d.rtv_formats.size()));
    for (uint32_t f : d.rtv_formats) {
        h.add_u32(f);
    }

    h.add_u32(static_cast<uint32_t>(d.input_layout.size()));
    for (const auto& e : d.input_layout) {
        h.add(e.semantic_name.data(), e.semantic_name.size());
        h.add_u32(e.semantic_index);
        h.add_u32(e.format);
        h.add_u32(e.input_slot);
        h.add_u32(e.aligned_byte_offset);
        h.add_u32(e.classification);
        h.add_u32(e.instance_step_rate);
    }

    const sc_capture_rasterizer_state& r = d.rasterizer;
    h.add_u32(r.fill_mode);
    h.add_u32(r.cull_mode);
    h.add_u32(r.front_counter_clockwise ? 1u : 0u);
    h.add_u32(r.depth_bias ? 1u : 0u);
    h.add_u32(r.depth_bias_value);
    h.add_u32(float_bits(r.depth_bias_clamp));
    h.add_u32(float_bits(r.slope_scaled_depth_bias));
    h.add_u32(r.depth_clip_enable ? 1u : 0u);
    h.add_u32(r.multisample_enable ? 1u : 0u);
    h.add_u32(r.antialiased_line_enable ? 1u : 0u);
    h.add_u32(r.forced_sample_count);
    h.add_u32(r.conservative_raster ? 1u : 0u);

    const sc_capture_depth_stencil_state& ds = d.depth_stencil;
    h.add_u32(ds.depth_enable ? 1u : 0u);
    h.add_u32(ds.depth_write_mask);
    h.add_u32(ds.depth_func);
    h.add_u32(ds.stencil_enable ? 1u : 0u);
    h.add_u32(ds.stencil_read_mask);
    h.add_u32(ds.stencil_write_mask);
    h.add_u32(ds.front_face_stencil_enable ? 1u : 0u);
    h.add_u32(ds.back_face_stencil_enable ? 1u : 0u);
    h.add_u32(ds.front_face.stencil_fail_op);
    h.add_u32(ds.front_face.stencil_depth_fail_op);
    h.add_u32(ds.front_face.stencil_pass_op);
    h.add_u32(ds.front_face.stencil_func);
    h.add_u32(ds.back_face.stencil_fail_op);
    h.add_u32(ds.back_face.stencil_depth_fail_op);
    h.add_u32(ds.back_face.stencil_pass_op);
    h.add_u32(ds.back_face.stencil_func);

    for (int i = 0; i < 4; ++i) {
        h.add_u32(float_bits(d.blend_factor[i]));
    }
    h.add_u32(static_cast<uint32_t>(d.render_targets.size()));
    for (const auto& rt : d.render_targets) {
        h.add_u32(rt.format);
        h.add_u32(rt.blend_enable ? 1u : 0u);
        h.add_u32(rt.logic_op_enable ? 1u : 0u);
        h.add_u32(rt.src_blend);
        h.add_u32(rt.dst_blend);
        h.add_u32(rt.blend_op);
        h.add_u32(rt.src_blend_alpha);
        h.add_u32(rt.dst_blend_alpha);
        h.add_u32(rt.blend_op_alpha);
        h.add_u32(rt.logic_op);
        h.add_u32(rt.write_mask);
    }

    h.add_u64(static_cast<uint64_t>(d.root_signature.size()));
    h.add(d.root_signature.data(), d.root_signature.size());

    // Shaders in canonical stage order so submission order cannot change the
    // signature of an otherwise identical PSO.
    std::vector<const sc_capture_stage_blob*> sorted;
    sorted.reserve(d.shaders.size());
    for (const auto& s : d.shaders) {
        sorted.push_back(&s);
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](const sc_capture_stage_blob* a, const sc_capture_stage_blob* b) {
        return static_cast<uint32_t>(a->stage) < static_cast<uint32_t>(b->stage);
    });
    h.add_u32(static_cast<uint32_t>(sorted.size()));
    for (const auto* s : sorted) {
        h.add_u32(static_cast<uint32_t>(s->stage));
        h.add_u64(static_cast<uint64_t>(s->bytes.size()));
        h.add(s->bytes.data(), s->bytes.size());
    }

    return h.value();
}

sc_capture_add_result sc_capture_store::add(const sc_capture_pso_desc& desc) {
    ++adds_;
    sc_capture_add_result result;
    result.signature = sc_capture_signature(desc);

    const auto it = by_signature_.find(result.signature);
    if (it != by_signature_.end()) {
        ++duplicates_;
        sc_capture_record& rec = records_[it->second];
        ++rec.hits;
        result.added = false;
        result.seq = rec.seq;
        result.hits = rec.hits;
        return result;
    }

    sc_capture_record rec;
    rec.seq = records_.size();
    rec.signature = result.signature;
    rec.hits = 1;
    rec.first_add = adds_ - 1;
    rec.desc = desc;
    rec.stages = join_stages(desc);

    for (const auto& blob : desc.shaders) {
        sc_capture_shader_report report;
        report.stage = blob.stage;
        report.size = blob.bytes.size();
        report.sha256 = sc_sha256_hex(blob.bytes.data(), blob.bytes.size());

        sc_container_info info;
        const bool parsed = sc_parse_container(blob.bytes.data(), blob.bytes.size(), info);
        if (parsed) {
            report.format = sc_container_format_name(info.format);
            report.chunks = info.chunk_list();
            report.chunk_count = info.chunk_count;
            report.stage_from_container = info.stage;
            report.has_dxil = info.has_dxil;
            report.has_root_signature = info.has_root_signature;
            if (report.stage == sc_stage::unknown) {
                report.stage = info.stage;
                report.stage_source = sc_stage_source_name(info.stage_from);
            } else if (info.stage == report.stage) {
                report.stage_source = "caller+container";
            } else {
                report.stage_source = "caller";
            }
        } else {
            report.format = "unparsed";
            report.stage_source = (report.stage != sc_stage::unknown) ? "caller" : "none";
            report.parse_error = info.error;
        }
        rec.shaders.push_back(report);
    }

    by_signature_[result.signature] = records_.size();
    records_.push_back(rec);

    result.added = true;
    result.seq = rec.seq;
    result.hits = rec.hits;
    return result;
}

void sc_capture_store::clear() {
    records_.clear();
    by_signature_.clear();
    adds_ = 0;
    duplicates_ = 0;
}

std::string sc_capture_store::stage_histogram_json() const {
    static const sc_stage kOrder[] = {sc_stage::vertex,  sc_stage::hull,   sc_stage::domain,
                                     sc_stage::geometry, sc_stage::pixel,  sc_stage::compute,
                                     sc_stage::mesh,     sc_stage::amplification, sc_stage::unknown};
    std::string out = "{";
    bool first = true;
    for (sc_stage st : kOrder) {
        uint32_t count = 0;
        for (const auto& rec : records_) {
            for (const auto& sh : rec.shaders) {
                if (sh.stage == st) {
                    ++count;
                    break;
                }
            }
        }
        if (count == 0) {
            continue;
        }
        if (!first) {
            out.push_back(',');
        }
        first = false;
        out.push_back('"');
        out += sc_stage_name(st);
        out += std::string("\":") + std::to_string(count);
    }
    out.push_back('}');
    return out;
}

std::string sc_capture_record_json(const sc_capture_record& r) {
    const sc_capture_pso_desc& d = r.desc;
    std::string s = "{";
    s += "\"schema\":\"scarlettcompat.shader_capture.pso/1\"";
    s += ",\"seq\":" + std::to_string(r.seq);
    s += ",\"signature\":";
    json_esc(s, r.signature_hex());
    s += ",\"hits\":" + std::to_string(r.hits);
    s += ",\"first_add\":" + std::to_string(r.first_add);
    s += ",\"stages\":";
    json_esc(s, r.stages);
    s += ",\"source_slot\":" + std::to_string(d.source_slot);
    if (!d.debug_name.empty()) {
        s += ",\"debug_name\":";
        json_esc(s, d.debug_name);
    }
    s += ",\"node_mask\":" + std::to_string(d.node_mask);
    s += ",\"flags\":" + std::to_string(d.flags);
    s += ",\"sample_count\":" + std::to_string(d.sample_count);
    s += ",\"sample_quality\":" + std::to_string(d.sample_quality);
    s += ",\"sample_mask\":" + std::to_string(d.sample_mask);
    s += ",\"ib_strip_cut_value\":" + std::to_string(d.ib_strip_cut_value);
    s += ",\"patch_control_points\":" + std::to_string(d.patch_control_points);
    s += ",\"primitive_topology_type\":" + std::to_string(d.primitive_topology_type);
    s += ",\"dsv_format\":" + std::to_string(d.dsv_format);

    s += ",\"rtv_formats\":[";
    for (size_t i = 0; i < d.rtv_formats.size(); ++i) {
        if (i != 0) {
            s.push_back(',');
        }
        s += std::to_string(d.rtv_formats[i]);
    }
    s += "]";

    s += ",\"input_layout\":[";
    for (size_t i = 0; i < d.input_layout.size(); ++i) {
        const auto& e = d.input_layout[i];
        if (i != 0) {
            s.push_back(',');
        }
        s.push_back('{');
        json_key(s, "semantic", e.semantic_name);
        s += ",\"index\":" + std::to_string(e.semantic_index);
        s += ",\"format\":" + std::to_string(e.format);
        s += ",\"slot\":" + std::to_string(e.input_slot);
        s += ",\"offset\":" + std::to_string(e.aligned_byte_offset);
        s += ",\"classification\":" + std::to_string(e.classification);
        s += ",\"step_rate\":" + std::to_string(e.instance_step_rate);
        s.push_back('}');
    }
    s += "]";

    const sc_capture_rasterizer_state& rz = d.rasterizer;
    s += ",\"rasterizer\":{";
    s += "\"fill_mode\":" + std::to_string(rz.fill_mode);
    s += ",\"cull_mode\":" + std::to_string(rz.cull_mode);
    s += ",\"front_ccw\":";
    s += rz.front_counter_clockwise ? "true" : "false";
    s += ",\"depth_bias\":";
    s += rz.depth_bias ? "true" : "false";
    s += ",\"depth_bias_value\":" + std::to_string(rz.depth_bias_value);
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), ",\"depth_bias_clamp\":%.9g", static_cast<double>(rz.depth_bias_clamp));
        s += buf;
        std::snprintf(buf, sizeof(buf), ",\"slope_scaled_depth_bias\":%.9g",
                      static_cast<double>(rz.slope_scaled_depth_bias));
        s += buf;
    }
    s += ",\"depth_clip_enable\":";
    s += rz.depth_clip_enable ? "true" : "false";
    s += ",\"multisample_enable\":";
    s += rz.multisample_enable ? "true" : "false";
    s += ",\"antialiased_line_enable\":";
    s += rz.antialiased_line_enable ? "true" : "false";
    s += ",\"forced_sample_count\":" + std::to_string(rz.forced_sample_count);
    s += ",\"conservative_raster\":";
    s += rz.conservative_raster ? "true" : "false";
    s.push_back('}');

    const sc_capture_depth_stencil_state& ds = d.depth_stencil;
    s += ",\"depth_stencil\":{";
    s += "\"depth_enable\":";
    s += ds.depth_enable ? "true" : "false";
    s += ",\"depth_write_mask\":" + std::to_string(ds.depth_write_mask);
    s += ",\"depth_func\":" + std::to_string(ds.depth_func);
    s += ",\"stencil_enable\":";
    s += ds.stencil_enable ? "true" : "false";
    s += ",\"stencil_read_mask\":" + std::to_string(ds.stencil_read_mask);
    s += ",\"stencil_write_mask\":" + std::to_string(ds.stencil_write_mask);
    s += ",\"front_stencil_enable\":";
    s += ds.front_face_stencil_enable ? "true" : "false";
    s += ",\"back_stencil_enable\":";
    s += ds.back_face_stencil_enable ? "true" : "false";
    s += ",\"front\":{\"fail\":" + std::to_string(ds.front_face.stencil_fail_op) +
         ",\"depth_fail\":" + std::to_string(ds.front_face.stencil_depth_fail_op) +
         ",\"pass\":" + std::to_string(ds.front_face.stencil_pass_op) +
         ",\"func\":" + std::to_string(ds.front_face.stencil_func) + "}";
    s += ",\"back\":{\"fail\":" + std::to_string(ds.back_face.stencil_fail_op) +
         ",\"depth_fail\":" + std::to_string(ds.back_face.stencil_depth_fail_op) +
         ",\"pass\":" + std::to_string(ds.back_face.stencil_pass_op) +
         ",\"func\":" + std::to_string(ds.back_face.stencil_func) + "}";
    s.push_back('}');

    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), ",\"blend_factor\":[%.9g,%.9g,%.9g,%.9g]",
                      static_cast<double>(d.blend_factor[0]), static_cast<double>(d.blend_factor[1]),
                      static_cast<double>(d.blend_factor[2]), static_cast<double>(d.blend_factor[3]));
        s += buf;
    }

    s += ",\"render_targets\":[";
    for (size_t i = 0; i < d.render_targets.size(); ++i) {
        const auto& rt = d.render_targets[i];
        if (i != 0) {
            s.push_back(',');
        }
        s.push_back('{');
        s += "\"format\":" + std::to_string(rt.format);
        s += ",\"blend_enable\":";
        s += rt.blend_enable ? "true" : "false";
        s += ",\"logic_op_enable\":";
        s += rt.logic_op_enable ? "true" : "false";
        s += ",\"src_blend\":" + std::to_string(rt.src_blend);
        s += ",\"dst_blend\":" + std::to_string(rt.dst_blend);
        s += ",\"blend_op\":" + std::to_string(rt.blend_op);
        s += ",\"src_blend_alpha\":" + std::to_string(rt.src_blend_alpha);
        s += ",\"dst_blend_alpha\":" + std::to_string(rt.dst_blend_alpha);
        s += ",\"blend_op_alpha\":" + std::to_string(rt.blend_op_alpha);
        s += ",\"logic_op\":" + std::to_string(rt.logic_op);
        s += ",\"write_mask\":" + std::to_string(rt.write_mask);
        s.push_back('}');
    }
    s += "]";

    s += ",\"root_signature_bytes\":" + std::to_string(d.root_signature.size());
    s += ",\"root_signature_sha256\":";
    json_esc(s, sc_sha256_hex(d.root_signature.data(), d.root_signature.size()));

    s += ",\"shaders\":[";
    for (size_t i = 0; i < r.shaders.size(); ++i) {
        const auto& sh = r.shaders[i];
        if (i != 0) {
            s.push_back(',');
        }
        s.push_back('{');
        json_key(s, "stage", sc_stage_name(sh.stage));
        s += ",\"stage_from_container\":";
        json_esc(s, sc_stage_name(sh.stage_from_container));
        s += ",\"stage_source\":";
        json_esc(s, sh.stage_source);
        s += ",\"format\":";
        json_esc(s, sh.format);
        s += ",\"sha256\":";
        json_esc(s, sh.sha256);
        s += ",\"size\":" + std::to_string(sh.size);
        s += ",\"chunk_count\":" + std::to_string(sh.chunk_count);
        s += ",\"chunks\":";
        json_esc(s, sh.chunks);
        s += ",\"has_dxil\":";
        s += sh.has_dxil ? "true" : "false";
        s += ",\"has_root_signature\":";
        s += sh.has_root_signature ? "true" : "false";
        if (!sh.parse_error.empty()) {
            s += ",\"parse_error\":";
            json_esc(s, sh.parse_error);
        }
        s += ",\"file\":";
        json_esc(s, shader_file_name(r.desc, i));
        s.push_back('}');
    }
    s += "]";
    s.push_back('}');
    return s;
}

bool sc_make_dirs(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::string partial;
    partial.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        const char c = path[i];
        partial.push_back(c);
        const bool sep = (c == '/' || c == '\\');
        const bool last = (i + 1 == path.size());
        if (!sep && !last) {
            continue;
        }
        if (sep && partial.size() <= 1) {
            continue;
        }
        std::string dir = partial;
        if (sep) {
            dir.pop_back(); // strip the trailing separator
        }
        if (dir.empty() || (dir.size() == 2 && dir[1] == ':')) {
            continue;
        }
#ifdef _WIN32
        _mkdir(dir.c_str());
#else
        mkdir(dir.c_str(), 0755);
#endif
    }
    return true;
}

bool sc_write_file(const std::string& path, const void* data, size_t size) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        return false;
    }
    if (size != 0) {
        f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    f.flush();
    return f.good();
}

namespace {

bool write_text(const std::string& path, const std::string& text) {
    return sc_write_file(path, text.data(), text.size());
}

std::string record_dir_name(const sc_capture_record& r) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "pso_%04llu_%08llx", static_cast<unsigned long long>(r.seq),
                  static_cast<unsigned long long>(r.signature & 0xffffffffull));
    return std::string(buf);
}

} // namespace

sc_capture_dump_report sc_capture_dump(const sc_capture_store& store, const std::string& directory,
                                      const sc_capture_dump_options& options) {
    sc_capture_dump_report report;
    report.root = directory;
    if (directory.empty()) {
        report.errors.push_back("empty output directory");
        return report;
    }
    if (!sc_make_dirs(directory)) {
        report.errors.push_back("could not create output directory: " + directory);
        return report;
    }

    std::string index;
    if (options.write_index) {
        index += "seq\tsignature\thits\tstages\tshaders\tshader_bytes\troot_sig_bytes\tsample_count\trtv_count\t"
                 "dsv_format\tsource_slot\tdir\n";
    }

    std::string manifest_records;
    bool manifest_first = true;

    for (const auto& rec : store.records()) {
        const std::string name = record_dir_name(rec);
        const std::string dir = directory + "/" + name;
        if (!sc_make_dirs(dir)) {
            report.errors.push_back("could not create " + dir);
            continue;
        }
        ++report.records;

        if (options.write_shader_binaries) {
            for (size_t i = 0; i < rec.desc.shaders.size(); ++i) {
                const auto& blob = rec.desc.shaders[i];
                // Never lose data when a stage repeats: suffix the second file.
                const std::string file = shader_file_name(rec.desc, i);
                const std::string path = dir + "/" + file;
                if (!sc_write_file(path, blob.bytes.data(), blob.bytes.size())) {
                    report.errors.push_back("write failed: " + path);
                    continue;
                }
                ++report.files;
                report.bytes += blob.bytes.size();
            }
        }

        if (options.write_root_signature && !rec.desc.root_signature.empty()) {
            const std::string path = dir + "/rootsig.bin";
            if (sc_write_file(path, rec.desc.root_signature.data(), rec.desc.root_signature.size())) {
                ++report.files;
                report.bytes += rec.desc.root_signature.size();
            } else {
                report.errors.push_back("write failed: " + path);
            }
        }

        if (options.write_record_json) {
            const std::string json = sc_capture_record_json(rec);
            const std::string path = dir + "/record.json";
            if (write_text(path, json)) {
                ++report.files;
                report.bytes += json.size();
            } else {
                report.errors.push_back("write failed: " + path);
            }
        }

        if (options.write_index) {
            index += std::to_string(rec.seq) + "\t" + rec.signature_hex() + "\t" + std::to_string(rec.hits) + "\t" +
                     rec.stages + "\t" + std::to_string(rec.shaders.size()) + "\t" +
                     std::to_string(rec.total_shader_bytes()) + "\t" +
                     std::to_string(rec.desc.root_signature.size()) + "\t" +
                     std::to_string(rec.desc.sample_count) + "\t" + std::to_string(rec.desc.rtv_formats.size()) +
                     "\t" + std::to_string(rec.desc.dsv_format) + "\t" + std::to_string(rec.desc.source_slot) + "\t" +
                     name + "\n";
        }

        if (options.write_manifest) {
            if (!manifest_first) {
                manifest_records.push_back(',');
            }
            manifest_first = false;
            manifest_records.push_back('{');
            manifest_records += "\"seq\":" + std::to_string(rec.seq);
            manifest_records += ",\"signature\":";
            json_esc(manifest_records, rec.signature_hex());
            manifest_records += ",\"hits\":" + std::to_string(rec.hits);
            manifest_records += ",\"stages\":";
            json_esc(manifest_records, rec.stages);
            manifest_records += ",\"shaders\":" + std::to_string(rec.shaders.size());
            manifest_records += ",\"shader_bytes\":" + std::to_string(rec.total_shader_bytes());
            manifest_records += ",\"root_signature_bytes\":" + std::to_string(rec.desc.root_signature.size());
            manifest_records += ",\"dir\":";
            json_esc(manifest_records, name);
            manifest_records.push_back('}');
        }
    }

    if (options.write_index) {
        const std::string path = directory + "/index.tsv";
        if (write_text(path, index)) {
            ++report.files;
            report.bytes += index.size();
        } else {
            report.errors.push_back("write failed: " + path);
        }
    }

    if (options.write_manifest) {
        std::string m = "{";
        m += "\"schema\":\"scarlettcompat.shader_capture.manifest/1\"";
        m += ",\"generated_utc\":";
        json_esc(m, utc_now());
        m += ",\"adds\":" + std::to_string(store.adds());
        m += ",\"unique_psos\":" + std::to_string(store.size());
        m += ",\"duplicates\":" + std::to_string(store.duplicates());
        m += ",\"stage_histogram\":" + store.stage_histogram_json();
        m += ",\"records\":[" + manifest_records + "]";
        m.push_back('}');
        const std::string path = directory + "/manifest.json";
        if (write_text(path, m)) {
            ++report.files;
            report.bytes += m.size();
        } else {
            report.errors.push_back("write failed: " + path);
        }
    }

    report.ok = report.errors.empty();
    return report;
}

} // namespace sc
