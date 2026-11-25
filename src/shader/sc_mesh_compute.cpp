// ScarlettCompat shader pipeline - Phase 8: mesh-as-compute (CPU reference model).
// MIT. New file; not wired into the boot path. See include/shader/sc_mesh_compute.h.
#include "shader/sc_mesh_compute.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace sc {

namespace {

uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void wr32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xffu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xffu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xffu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xffu);
}

// One vertex record, as it sits in the buffer: 28 bytes, little endian floats.
std::string vertex_key(const uint8_t* record) {
    return std::string(reinterpret_cast<const char*>(record), kScMeshVertexStrideBytes);
}

// A triangle key is the three vertex records with the records sorted, so the key
// does not depend on the winding order the group happened to write.
std::string triangle_key(const std::string& a, const std::string& b, const std::string& c) {
    std::string r[3] = {a, b, c};
    std::sort(r, r + 3);
    return r[0] + r[1] + r[2];
}

} // namespace

sc_mesh_compute_plan sc_mesh_compute_plan_buffers(uint32_t meshlet_count,
                                                  const sc_mesh_compute_limits& limits) {
    sc_mesh_compute_plan plan;
    if (meshlet_count == 0) {
        plan.error = "no meshlets: nothing to dispatch";
        return plan;
    }
    if (limits.threads_per_group == 0 || limits.max_meshlet_vertices == 0 ||
        limits.max_meshlet_indices == 0 || limits.max_total_vertices == 0 ||
        limits.max_total_indices == 0) {
        plan.error = "a limit is zero";
        return plan;
    }
    if (limits.max_total_vertices > 0x7fffffffu || limits.max_total_indices > 0x7fffffffu) {
        plan.error = "limit too large for 32-bit index arithmetic";
        return plan;
    }
    // One group emits one vertex per thread and two indices per thread: a per-meshlet
    // cap larger than that could never be produced, so the limits are inconsistent.
    if (limits.max_meshlet_vertices > limits.threads_per_group) {
        plan.error = "max_meshlet_vertices exceeds threads_per_group";
        return plan;
    }
    if (limits.max_meshlet_indices > 2u * limits.threads_per_group) {
        plan.error = "max_meshlet_indices exceeds two per thread";
        return plan;
    }
    plan.groups = meshlet_count; // one meshlet per group
    plan.threads_per_group = limits.threads_per_group;
    plan.max_meshlet_vertices = limits.max_meshlet_vertices;
    plan.max_meshlet_indices = limits.max_meshlet_indices;
    plan.vertex_stride = kScMeshVertexStrideBytes;
    plan.vertex_buffer_bytes =
        static_cast<uint64_t>(limits.max_total_vertices) * kScMeshVertexStrideBytes;
    plan.index_buffer_bytes = static_cast<uint64_t>(limits.max_total_indices) * 4u;
    plan.args_bytes = kScMeshDrawArgsBytes;
    plan.counter_bytes = kScMeshCounterBytes;
    plan.total_bytes = plan.vertex_buffer_bytes + plan.index_buffer_bytes + plan.args_bytes +
                       plan.counter_bytes;
    plan.ok = true;
    return plan;
}

bool sc_mesh_draw_args_encode(const sc_mesh_draw_args& args,
                              uint8_t out[kScMeshDrawArgsBytes]) {
    if (out == nullptr) {
        return false;
    }
    wr32(out + 0, args.index_count);
    wr32(out + 4, args.instance_count);
    wr32(out + 8, args.first_index);
    wr32(out + 12, args.base_vertex);
    wr32(out + 16, args.first_instance);
    return true;
}

bool sc_mesh_draw_args_decode(const uint8_t in[kScMeshDrawArgsBytes],
                              sc_mesh_draw_args* out) {
    if (in == nullptr || out == nullptr) {
        return false;
    }
    out->index_count = rd32(in + 0);
    out->instance_count = rd32(in + 4);
    out->first_index = rd32(in + 8);
    out->base_vertex = rd32(in + 12);
    out->first_instance = rd32(in + 16);
    return true;
}

bool sc_mesh_compute_execute(const sc_meshlet* meshlets, uint32_t count,
                             const sc_mesh_compute_plan& plan,
                             uint8_t* vertex_buffer, uint64_t vertex_buffer_bytes,
                             uint8_t* index_buffer, uint64_t index_buffer_bytes,
                             sc_mesh_compute_result* out) {
    if (out == nullptr) {
        return false;
    }
    *out = sc_mesh_compute_result();
    if (!plan.ok) {
        out->error = plan.error[0] != '\0' ? plan.error : "plan is not usable";
        return false;
    }
    if (meshlets == nullptr || count == 0) {
        out->error = "no meshlets";
        return false;
    }
    if (vertex_buffer == nullptr || index_buffer == nullptr) {
        out->error = "null output buffer";
        return false;
    }
    if (vertex_buffer_bytes < plan.vertex_buffer_bytes ||
        index_buffer_bytes < plan.index_buffer_bytes) {
        out->error = "output buffer smaller than the plan";
        return false;
    }

    // Reject malformed meshlets before touching the buffers: a partial write would
    // leave the validator comparing against data that was never a valid expansion.
    for (uint32_t m = 0; m < count; ++m) {
        const sc_meshlet& ml = meshlets[m];
        if (ml.positions == nullptr || ml.colors == nullptr || ml.indices == nullptr ||
            ml.vertex_count == 0 || ml.index_count == 0) {
            out->error = "meshlet has a null array or zero count";
            return false;
        }
        if (ml.index_count % 3u != 0u) {
            out->error = "meshlet index_count is not a multiple of 3";
            return false;
        }
        if (ml.vertex_count > plan.max_meshlet_vertices ||
            ml.index_count > plan.max_meshlet_indices) {
            out->error = "meshlet larger than the per-group cap";
            return false;
        }
        if (ml.vertex_count > (vertex_buffer_bytes / kScMeshVertexStrideBytes) ||
            ml.index_count > (index_buffer_bytes / 4u)) {
            out->error = "meshlet larger than its output buffer";
            return false;
        }
        for (uint32_t i = 0; i < ml.index_count; ++i) {
            if (ml.indices[i] >= ml.vertex_count) {
                out->error = "meshlet index out of range";
                return false;
            }
        }
    }

    std::memset(vertex_buffer, 0, static_cast<size_t>(plan.vertex_buffer_bytes));
    std::memset(index_buffer, 0, static_cast<size_t>(plan.index_buffer_bytes));

    const uint32_t max_vertices =
        static_cast<uint32_t>(plan.vertex_buffer_bytes / kScMeshVertexStrideBytes);
    const uint32_t max_indices = static_cast<uint32_t>(plan.index_buffer_bytes / 4u);
    uint32_t vertex_counter = 0;
    uint32_t index_counter = 0;
    uint32_t emitted = 0;
    uint32_t dropped = 0;

    for (uint32_t g = 0; g < count; ++g) {
        const sc_meshlet& ml = meshlets[g];
        // One atomic-add claim per group. A meshlet that would overrun either cap is
        // dropped whole rather than truncated, so the counters never point at a
        // half-written meshlet.
        if (ml.vertex_count > max_vertices - vertex_counter ||
            ml.index_count > max_indices - index_counter) {
            ++dropped;
            continue;
        }
        const uint32_t vertex_base = vertex_counter;
        const uint32_t index_base = index_counter;
        vertex_counter += ml.vertex_count;
        index_counter += ml.index_count;

        for (uint32_t v = 0; v < ml.vertex_count; ++v) {
            float* rec = reinterpret_cast<float*>(
                vertex_buffer + static_cast<size_t>(vertex_base + v) * kScMeshVertexStrideBytes);
            for (uint32_t c = 0; c < 3u; ++c) {
                rec[c] = ml.positions[v * 3u + c];
            }
            for (uint32_t c = 0; c < 4u; ++c) {
                rec[3u + c] = ml.colors[v * 4u + c];
            }
        }
        for (uint32_t i = 0; i < ml.index_count; ++i) {
            wr32(index_buffer + static_cast<size_t>(index_base + i) * 4u,
                 vertex_base + ml.indices[i]);
        }
        ++emitted;
    }

    out->groups_dispatched = count;
    out->meshlets_emitted = emitted;
    out->meshlets_dropped = dropped;
    out->clamped = dropped != 0;
    out->vertices_written = vertex_counter;
    out->indices_written = index_counter;
    out->triangles = index_counter / 3u;
    out->args.index_count = index_counter;
    out->args.instance_count = 1;
    out->args.first_index = 0;
    out->args.base_vertex = 0;
    out->args.first_instance = 0;
    out->ok = true;
    return true;
}

sc_mesh_compute_check sc_mesh_compute_verify(const sc_meshlet* meshlets, uint32_t count,
                                             const uint8_t* vertex_buffer,
                                             uint64_t vertex_buffer_bytes,
                                             const uint8_t* index_buffer,
                                             uint64_t index_buffer_bytes,
                                             const sc_mesh_draw_args& args) {
    sc_mesh_compute_check check;
    if (meshlets == nullptr || count == 0 || vertex_buffer == nullptr || index_buffer == nullptr) {
        check.detail = "nothing to verify";
        return check;
    }
    if (vertex_buffer_bytes > 0xffffffffu || index_buffer_bytes > 0xffffffffu) {
        check.detail = "buffer larger than the 32-bit index space";
        return check;
    }
    const uint32_t vertex_slots =
        static_cast<uint32_t>(vertex_buffer_bytes / kScMeshVertexStrideBytes);
    const uint32_t index_slots = static_cast<uint32_t>(index_buffer_bytes / 4u);

    check.bounds_ok = args.first_index <= index_slots &&
                      args.index_count <= index_slots - args.first_index &&
                      args.instance_count == 1u && args.first_instance == 0u &&
                      args.index_count % 3u == 0u;
    check.args_consistent = check.bounds_ok;
    check.triangles_found = args.index_count / 3u;

    // Expected: every triangle of every meshlet, taken from the meshlet data.
    std::vector<std::string> expected;
    for (uint32_t m = 0; m < count; ++m) {
        const sc_meshlet& ml = meshlets[m];
        check.triangles_expected += ml.index_count / 3u;
        for (uint32_t t = 0; t + 2 < ml.index_count; t += 3) {
            std::string k[3];
            for (uint32_t e = 0; e < 3u; ++e) {
                const uint32_t vi = ml.indices[t + e];
                if (vi >= ml.vertex_count) {
                    ++check.indices_out_of_range;
                    continue;
                }
                std::string rec(kScMeshVertexStrideBytes, '\0');
                for (uint32_t c = 0; c < 3u; ++c) {
                    std::memcpy(&rec[c * 4u], &ml.positions[vi * 3u + c], 4u);
                }
                for (uint32_t c = 0; c < 4u; ++c) {
                    std::memcpy(&rec[(3u + c) * 4u], &ml.colors[vi * 4u + c], 4u);
                }
                k[e] = rec;
            }
            expected.push_back(triangle_key(k[0], k[1], k[2]));
        }
    }

    // Found: what the draw call would read, through the index buffer.
    std::vector<std::string> found;
    for (uint32_t t = 0; t + 2 < args.index_count; t += 3) {
        std::string k[3];
        for (uint32_t e = 0; e < 3u; ++e) {
            const uint32_t slot = args.first_index + t + e;
            const uint32_t vi = rd32(index_buffer + static_cast<size_t>(slot) * 4u);
            const uint32_t absolute = vi + args.base_vertex;
            if (absolute >= vertex_slots) {
                ++check.indices_out_of_range;
                continue;
            }
            k[e] = vertex_key(vertex_buffer +
                              static_cast<size_t>(absolute) * kScMeshVertexStrideBytes);
            ++check.vertices_checked;
        }
        found.push_back(triangle_key(k[0], k[1], k[2]));
    }

    std::sort(expected.begin(), expected.end());
    std::sort(found.begin(), found.end());
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < expected.size() && j < found.size()) {
        if (expected[i] < found[j]) {
            ++i;
        } else if (found[j] < expected[i]) {
            ++j;
        } else {
            ++check.triangles_matched;
            ++i;
            ++j;
        }
    }

    check.ok = check.bounds_ok && check.indices_out_of_range == 0 &&
               check.triangles_found == check.triangles_expected &&
               check.triangles_matched == check.triangles_expected;
    if (!check.ok) {
        check.detail = !check.bounds_ok                       ? "arguments or buffers out of bounds"
                       : check.indices_out_of_range != 0      ? "index out of range"
                       : check.triangles_found != check.triangles_expected
                             ? "argument index count does not match the meshlets"
                             : "triangle multisets differ";
    }
    return check;
}

} // namespace sc
