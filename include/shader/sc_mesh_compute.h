// ScarlettCompat shader pipeline - Phase 8: mesh-as-compute (CPU reference model).
// MIT. New file; not wired into the boot path.
//
// WHY THIS EXISTS
//   The RX 5700 XT (gfx10.3, FL 12_1) has no hardware mesh-shader tier, so mesh
//   work has to run as compute-to-draw: a meshlet's vertex generation is done by a
//   compute dispatch that writes a vertex buffer and an index buffer, and a plain
//   indexed draw consumes them. The DX12 mesh pipeline needs two things from that
//   arrangement that a CPU prototype cannot fake: the exact buffer/argument
//   layout, and a validator that can say whether a GPU run produced the right
//   geometry. This module is that contract plus a CPU reference implementation of
//   it, so the GPU compute pass (Freebuff's side) can be diffed against it.
//
// WHAT IS MEASURED AND WHAT IS ASSUMED
//   MEASURED / FIXED BY THIS MODULE: the 28-byte vertex record (float3 clip-space
//   position + float4 colour) that the existing RDNA1 mesh fallback VS already
//   consumes; the 20-byte D3D12_DRAW_INDEXED_ARGUMENTS layout, encoded here byte
//   by byte and checked in the harness; the per-group caps (64 vertices, 128
//   indices, one meshlet per group); the counter-based compaction rule and the
//   argument derivation from the final counters.
//   ASSUMED: the guest's actual meshlet record format and the amplification stage's
//   culling. Neither exists in this tree and neither can be read from a real
//   capture yet (no corpus), so they stay outside this module: the caller supplies
//   meshlets.
//
// ORDER INDEPENDENCE
//   A real dispatch lets groups run in any order, so the vertex order in the output
//   buffer is not fixed. Every check here is therefore order independent: the
//   validator compares triangles as a multiset of vertex records, not as a byte
//   sequence.
#pragma once
#include <cstdint>

namespace sc {

// One meshlet per thread group; thread t writes vertex t and indices 2t, 2t+1.
constexpr uint32_t kScMeshThreadsPerGroup = 64;
constexpr uint32_t kScMeshMaxMeshletVertices = 64;
constexpr uint32_t kScMeshMaxMeshletIndices = 128;
// float3 position + float4 colour, the layout the fallback vertex shader reads.
constexpr uint32_t kScMeshVertexStrideBytes = 28;
// D3D12_DRAW_INDEXED_ARGUMENTS: indexCount, instanceCount, startIndexLocation,
// baseVertexLocation, startInstanceLocation.
constexpr uint32_t kScMeshDrawArgsBytes = 20;
// Two u32 counters after the arguments: vertices written, indices written.
constexpr uint32_t kScMeshCounterBytes = 8;

struct sc_meshlet {
    const float* positions;    // vertex_count * 3 floats, clip space
    const float* colors;       // vertex_count * 4 floats
    const uint32_t* indices;   // index_count entries, each < vertex_count
    uint32_t vertex_count;
    uint32_t index_count;      // multiple of 3
};

struct sc_mesh_draw_args {
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t first_index;
    uint32_t base_vertex;
    uint32_t first_instance;
};

struct sc_mesh_compute_limits {
    uint32_t threads_per_group = kScMeshThreadsPerGroup;
    uint32_t max_meshlet_vertices = kScMeshMaxMeshletVertices;
    uint32_t max_meshlet_indices = kScMeshMaxMeshletIndices;
    uint32_t max_total_vertices = 65536;
    uint32_t max_total_indices = 1u << 20;
};

// Buffer sizes are fixed by the caps, not by the input: the GPU path allocates
// them once and the compute stage fills them up to the counters.
struct sc_mesh_compute_plan {
    uint32_t groups = 0;
    // The per-group caps the plan was built from. execute() enforces exactly these,
    // so a caller cannot size the buffers from one limit set and dispatch another.
    uint32_t threads_per_group = kScMeshThreadsPerGroup;
    uint32_t max_meshlet_vertices = kScMeshMaxMeshletVertices;
    uint32_t max_meshlet_indices = kScMeshMaxMeshletIndices;
    uint32_t vertex_stride = kScMeshVertexStrideBytes;
    uint64_t vertex_buffer_bytes = 0;
    uint64_t index_buffer_bytes = 0;
    uint64_t args_bytes = kScMeshDrawArgsBytes;
    uint64_t counter_bytes = kScMeshCounterBytes;
    uint64_t total_bytes = 0;
    bool ok = false;
    const char* error = "";
};

struct sc_mesh_compute_result {
    uint32_t groups_dispatched = 0;
    uint32_t meshlets_emitted = 0;
    uint32_t meshlets_dropped = 0;   // would not fit in the caps
    uint32_t vertices_written = 0;
    uint32_t indices_written = 0;
    uint32_t triangles = 0;
    bool clamped = false;            // something was dropped
    sc_mesh_draw_args args{};
    bool ok = false;
    const char* error = "";
};

// Sizes the three buffers from the caps and computes the dispatch width
// (one group per meshlet), and copies the per-group caps into the plan for
// execute() to enforce. Fails when meshlet_count is zero, a limit is zero, or the
// per-meshlet caps cannot be emitted by one group (a group has one thread per
// vertex and two per index, so max_meshlet_vertices <= threads_per_group and
// max_meshlet_indices <= 2 * threads_per_group).
sc_mesh_compute_plan sc_mesh_compute_plan_buffers(uint32_t meshlet_count,
                                                  const sc_mesh_compute_limits& limits);

bool sc_mesh_draw_args_encode(const sc_mesh_draw_args& args,
                              uint8_t out[kScMeshDrawArgsBytes]);
bool sc_mesh_draw_args_decode(const uint8_t in[kScMeshDrawArgsBytes],
                              sc_mesh_draw_args* out);

// CPU reference model of the compute stage. For each group, in ascending group
// order (a GPU may use any order; see the note on order independence):
//   1. the group claims `vertex_count` slots with one atomic add on the vertex
//      counter and `index_count` slots with one atomic add on the index counter;
//   2. if the claim would exceed a cap, the whole meshlet is dropped and the
//      counters are not moved (the real path clamps the same way);
//   3. the group writes its vertices interleaved at the claimed vertex base and
//      its indices rebased on that base.
// Afterwards the arguments are derived from the counters: one instance, no first
// index, no base vertex. vertex_buffer must be at least plan.vertex_buffer_bytes
// and index_buffer at least plan.index_buffer_bytes; both are wiped first so a
// stale run cannot pass the validator.
//
// Rejects, with an error string in the result: a null buffer or meshlet array, an
// empty meshlet, index_count not a multiple of 3, an index >= vertex_count, or a
// meshlet larger than the plan's per-group caps (vertex_count >
// plan.max_meshlet_vertices or index_count > plan.max_meshlet_indices). A meshlet
// that breaks a per-group cap is malformed input, not a runtime overflow, so it is
// rejected rather than dropped; only the *total* caps cause a drop. In the GPU path
// the two counters stay in device memory and feed ExecuteIndirect; here they are
// reported in the result.
bool sc_mesh_compute_execute(const sc_meshlet* meshlets, uint32_t count,
                             const sc_mesh_compute_plan& plan,
                             uint8_t* vertex_buffer, uint64_t vertex_buffer_bytes,
                             uint8_t* index_buffer, uint64_t index_buffer_bytes,
                             sc_mesh_compute_result* out);

// What the validator measured.
struct sc_mesh_compute_check {
    uint32_t triangles_expected = 0;   // from the meshlets, ignoring caps
    uint32_t triangles_found = 0;      // args.index_count / 3
    uint32_t triangles_matched = 0;    // multiset intersection with the reference
    uint32_t vertices_checked = 0;
    uint32_t indices_out_of_range = 0;
    bool args_consistent = false;
    bool bounds_ok = false;
    bool ok = false;
    const char* detail = "";
};

// Compares the geometry the buffers describe with a direct, group-order-free
// expansion of the same meshlets: every triangle is keyed by its three (sorted)
// 28-byte vertex records, and the two triangle multisets must be equal. Also
// checks that no index leaves the written range and that the arguments describe
// exactly the written indices.
sc_mesh_compute_check sc_mesh_compute_verify(const sc_meshlet* meshlets, uint32_t count,
                                             const uint8_t* vertex_buffer,
                                             uint64_t vertex_buffer_bytes,
                                             const uint8_t* index_buffer,
                                             uint64_t index_buffer_bytes,
                                             const sc_mesh_draw_args& args);

} // namespace sc
