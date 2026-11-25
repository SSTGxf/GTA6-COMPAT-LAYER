// ScarlettCompat shader pipeline - Phase 8 harness (mesh-as-compute).
// MIT. Standalone: no game, no guest, no device, no layer, no emitter.
//
//   sc_mesh_compute_test.exe
//
// Every meshlet here is SYNTHETIC: hand-written clip-space triangles, quads and a
// strip, plus a grid built in code. No real mesh shader data exists yet (no corpus
// until the layer creates PSOs), so nothing in this file claims to validate the
// guest's geometry. What it does prove:
//   * the buffer/argument contract (28-byte vertex record, 20-byte
//     DRAW_INDEXED_ARGUMENTS, counter-based compaction) is exact and round-trips;
//   * the reference compute model writes a vertex/index pair whose triangles are
//     the meshlets' triangles as a multiset, whatever order the groups ran in;
//   * the validator catches corruption, out-of-range indices and bad arguments, so
//     a green run from a future GPU compute pass means something.
#include "shader/sc_mesh_compute.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
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

// A synthetic meshlet: positions and colours are owned by the library below, the
// index list is the meshlet's own (0-based) topology.
struct fixture_mesh {
    std::vector<float> positions;
    std::vector<float> colors;
    std::vector<uint32_t> indices;
    sc_meshlet view{};
};

fixture_mesh make_triangle(float x, float y) {
    fixture_mesh f;
    f.positions = {x, y, 0.f, x + 1.f, y, 0.f, x, y + 1.f, 0.f};
    f.colors = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
    f.indices = {0, 1, 2};
    f.view.positions = f.positions.data();
    f.view.colors = f.colors.data();
    f.view.indices = f.indices.data();
    f.view.vertex_count = 3;
    f.view.index_count = 3;
    return f;
}

fixture_mesh make_quad(float x, float y) {
    fixture_mesh f;
    f.positions = {x, y, 0.f, x + 1.f, y, 0.f, x + 1.f, y + 1.f, 0.f, x, y + 1.f, 0.f};
    f.colors = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    f.indices = {0, 1, 2, 0, 2, 3};
    f.view.positions = f.positions.data();
    f.view.colors = f.colors.data();
    f.view.indices = f.indices.data();
    f.view.vertex_count = 4;
    f.view.index_count = 6;
    return f;
}

fixture_mesh make_strip(float x, float y) {
    fixture_mesh f;
    f.positions = {x, y, 0.f, x + 1.f, y, 0.f, x, y + 1.f, 0.f, x + 1.f, y + 1.f, 0.f};
    f.colors = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    f.indices = {0, 1, 2, 1, 3, 2};
    f.view.positions = f.positions.data();
    f.view.colors = f.colors.data();
    f.view.indices = f.indices.data();
    f.view.vertex_count = 4;
    f.view.index_count = 6;
    return f;
}

std::vector<uint8_t> vertex_bytes(const fixture_mesh& f) {
    std::vector<uint8_t> out(f.positions.size() / 3 * kScMeshVertexStrideBytes);
    for (size_t v = 0; v < f.positions.size() / 3; ++v) {
        std::memcpy(&out[v * kScMeshVertexStrideBytes], &f.positions[v * 3], 12);
        std::memcpy(&out[v * kScMeshVertexStrideBytes + 12], &f.colors[v * 4], 16);
    }
    return out;
}

} // namespace

int main() {
    banner("plan: buffer sizes come from the caps, not the input");
    {
        sc_mesh_compute_limits lim;
        lim.max_total_vertices = 8;
        lim.max_total_indices = 24;
        const sc_mesh_compute_plan p = sc_mesh_compute_plan_buffers(2, lim);
        CHECK(p.ok);
        CHECK(p.groups == 2);
        CHECK(p.vertex_stride == 28);
        CHECK(p.vertex_buffer_bytes == 8ull * 28ull);
        CHECK(p.index_buffer_bytes == 24ull * 4ull);
        CHECK(p.args_bytes == 20);
        CHECK(p.counter_bytes == 8);
        // The plan carries the per-group caps so execute() enforces the same set.
        CHECK(p.threads_per_group == lim.threads_per_group);
        CHECK(p.max_meshlet_vertices == lim.max_meshlet_vertices);
        CHECK(p.max_meshlet_indices == lim.max_meshlet_indices);
        CHECK(p.total_bytes == 8ull * 28ull + 24ull * 4ull + 20 + 8);

        const sc_mesh_compute_plan zero = sc_mesh_compute_plan_buffers(0, lim);
        CHECK(!zero.ok && zero.error[0] != '\0');
        sc_mesh_compute_limits bad = lim;
        bad.max_total_vertices = 0;
        const sc_mesh_compute_plan p2 = sc_mesh_compute_plan_buffers(1, bad);
        CHECK(!p2.ok);

        // A per-meshlet cap one group could never emit is inconsistent limits, not a
        // plan that silently cannot be met.
        sc_mesh_compute_limits wide_vertices = lim;
        wide_vertices.threads_per_group = 32;
        wide_vertices.max_meshlet_vertices = 64;   // one vertex per thread
        CHECK(!sc_mesh_compute_plan_buffers(1, wide_vertices).ok);
        sc_mesh_compute_limits wide_indices = lim;
        wide_indices.max_meshlet_indices = 130;    // two indices per thread = 128
        CHECK(!sc_mesh_compute_plan_buffers(1, wide_indices).ok);
    }

    banner("arguments: 20-byte DRAW_INDEXED_ARGUMENTS layout");
    {
        sc_mesh_draw_args a{};
        a.index_count = 0x11223344u;
        a.instance_count = 1;
        a.first_index = 0;
        a.base_vertex = 0;
        a.first_instance = 0;
        uint8_t buf[kScMeshDrawArgsBytes] = {};
        CHECK(sc_mesh_draw_args_encode(a, buf));
        CHECK(buf[0] == 0x44 && buf[1] == 0x33 && buf[2] == 0x22 && buf[3] == 0x11);
        CHECK(buf[4] == 1 && buf[5] == 0 && buf[6] == 0 && buf[7] == 0);
        sc_mesh_draw_args b{};
        CHECK(sc_mesh_draw_args_decode(buf, &b));
        CHECK(b.index_count == a.index_count && b.instance_count == 1);
        CHECK(!sc_mesh_draw_args_encode(a, nullptr));
        CHECK(!sc_mesh_draw_args_decode(nullptr, &b));
    }

    banner("execute: one meshlet, exact bytes and arguments");
    {
        fixture_mesh tri = make_triangle(0.f, 0.f);
        const sc_meshlet* m = &tri.view;
        sc_mesh_compute_limits lim;
        lim.max_total_vertices = 8;
        lim.max_total_indices = 24;
        const sc_mesh_compute_plan p = sc_mesh_compute_plan_buffers(1, lim);
        std::vector<uint8_t> vb(static_cast<size_t>(p.vertex_buffer_bytes));
        std::vector<uint8_t> ib(static_cast<size_t>(p.index_buffer_bytes));
        sc_mesh_compute_result r;
        CHECK(sc_mesh_compute_execute(m, 1, p, vb.data(), vb.size(), ib.data(), ib.size(), &r));
        CHECK(r.ok);
        CHECK(r.groups_dispatched == 1 && r.meshlets_emitted == 1 && r.meshlets_dropped == 0);
        CHECK(r.vertices_written == 3 && r.indices_written == 3 && r.triangles == 1);
        CHECK(!r.clamped);
        CHECK(r.args.index_count == 3 && r.args.instance_count == 1);
        CHECK(r.args.first_index == 0 && r.args.base_vertex == 0 && r.args.first_instance == 0);
        // 28-byte records, in meshlet order, matching the source bytes exactly.
        const std::vector<uint8_t> expect_vb = vertex_bytes(tri);
        CHECK(expect_vb.size() == 3ull * kScMeshVertexStrideBytes);
        CHECK(std::memcmp(vb.data(), expect_vb.data(), expect_vb.size()) == 0);
        const uint32_t i0 = static_cast<uint32_t>(ib[0]);
        const uint32_t i1 = static_cast<uint32_t>(ib[4]);
        const uint32_t i2 = static_cast<uint32_t>(ib[8]);
        CHECK(i0 == 0 && i1 == 1 && i2 == 2);
        const sc_mesh_compute_check c =
            sc_mesh_compute_verify(m, 1, vb.data(), vb.size(), ib.data(), ib.size(), r.args);
        CHECK(c.ok);
        CHECK(c.triangles_expected == 1 && c.triangles_found == 1 && c.triangles_matched == 1);
        CHECK(c.vertices_checked == 3 && c.indices_out_of_range == 0);
    }

    banner("execute: three meshlets compact into one draw");
    {
        fixture_mesh tri = make_triangle(0.f, 0.f);
        fixture_mesh quad = make_quad(2.f, 0.f);
        fixture_mesh strip = make_strip(4.f, 0.f);
        const sc_meshlet meshes[3] = {tri.view, quad.view, strip.view};
        sc_mesh_compute_limits lim;
        lim.max_total_vertices = 11;
        lim.max_total_indices = 15;
        const sc_mesh_compute_plan p = sc_mesh_compute_plan_buffers(3, lim);
        std::vector<uint8_t> vb(static_cast<size_t>(p.vertex_buffer_bytes));
        std::vector<uint8_t> ib(static_cast<size_t>(p.index_buffer_bytes));
        sc_mesh_compute_result r;
        CHECK(sc_mesh_compute_execute(meshes, 3, p, vb.data(), vb.size(), ib.data(), ib.size(), &r));
        CHECK(r.ok && !r.clamped);
        CHECK(r.meshlets_emitted == 3 && r.meshlets_dropped == 0);
        CHECK(r.vertices_written == 11 && r.indices_written == 15 && r.triangles == 5);
        CHECK(r.args.index_count == 15);
        // Rebased indices: the quad's vertices start at slot 3 and the strip's at 7.
        CHECK(ib[3 * 4] == 3 && ib[4 * 4] == 4 && ib[5 * 4] == 5);
        CHECK(ib[9 * 4] == 7 && ib[10 * 4] == 8 && ib[11 * 4] == 9);
        const sc_mesh_compute_check c =
            sc_mesh_compute_verify(meshes, 3, vb.data(), vb.size(), ib.data(), ib.size(), r.args);
        CHECK(c.ok);
        CHECK(c.triangles_expected == 5 && c.triangles_matched == 5);
        CHECK(c.vertices_checked == 15);

        // Determinism: the same input produces the same bytes.
        std::vector<uint8_t> vb2(vb.size());
        std::vector<uint8_t> ib2(ib.size());
        sc_mesh_compute_result r2;
        CHECK(sc_mesh_compute_execute(meshes, 3, p, vb2.data(), vb2.size(), ib2.data(), ib2.size(), &r2));
        CHECK(std::memcmp(vb.data(), vb2.data(), vb.size()) == 0);
        CHECK(std::memcmp(ib.data(), ib2.data(), ib.size()) == 0);
        CHECK(r2.args.index_count == r.args.index_count);

        // Order independence: reversing the group order changes the buffer layout
        // (that is the GPU's freedom) but must not change the triangle multiset.
        const sc_meshlet reversed[3] = {strip.view, quad.view, tri.view};
        CHECK(sc_mesh_compute_execute(reversed, 3, p, vb2.data(), vb2.size(), ib2.data(), ib2.size(), &r2));
        CHECK(r2.args.index_count == 15);
        const sc_mesh_compute_check c2 = sc_mesh_compute_verify(
            meshes, 3, vb2.data(), vb2.size(), ib2.data(), ib2.size(), r2.args);
        CHECK(c2.ok);
        CHECK(c2.triangles_matched == 5);
    }

    banner("caps: a meshlet that does not fit is dropped whole");
    {
        fixture_mesh tri = make_triangle(0.f, 0.f);
        fixture_mesh quad = make_quad(2.f, 0.f);
        const sc_meshlet meshes[2] = {tri.view, quad.view};
        sc_mesh_compute_limits lim;
        lim.max_total_vertices = 4; // the triangle fits, the quad does not
        lim.max_total_indices = 24;
        const sc_mesh_compute_plan p = sc_mesh_compute_plan_buffers(2, lim);
        std::vector<uint8_t> vb(static_cast<size_t>(p.vertex_buffer_bytes));
        std::vector<uint8_t> ib(static_cast<size_t>(p.index_buffer_bytes));
        sc_mesh_compute_result r;
        CHECK(sc_mesh_compute_execute(meshes, 2, p, vb.data(), vb.size(), ib.data(), ib.size(), &r));
        CHECK(r.ok);
        CHECK(r.meshlets_emitted == 1 && r.meshlets_dropped == 1 && r.clamped);
        CHECK(r.vertices_written == 3 && r.indices_written == 3 && r.triangles == 1);
        const sc_mesh_compute_check c =
            sc_mesh_compute_verify(meshes, 2, vb.data(), vb.size(), ib.data(), ib.size(), r.args);
        CHECK(!c.ok);
        CHECK(c.triangles_expected == 3 && c.triangles_found == 1 && c.triangles_matched == 1);
        CHECK(c.detail != nullptr && c.detail[0] != '\0');
    }

    banner("caps: the per-group meshlet limits are enforced by execute");
    {
        // The reported hole: a 100-vertex meshlet with a buffer big enough for it
        // used to pass a 64-vertex per-group cap. It must now be rejected.
        std::vector<float> positions(100 * 3, 0.5f);
        std::vector<float> colors(100 * 4, 1.f);
        std::vector<uint32_t> indices(300);
        for (uint32_t i = 0; i < 300; ++i) {
            indices[i] = i % 100;
        }
        sc_meshlet big{};
        big.positions = positions.data();
        big.colors = colors.data();
        big.indices = indices.data();
        big.vertex_count = 100;
        big.index_count = 300;
        sc_mesh_compute_limits lim;            // totals large enough to hold it
        lim.max_total_vertices = 256;
        lim.max_total_indices = 1024;
        const sc_mesh_compute_plan p = sc_mesh_compute_plan_buffers(1, lim);
        CHECK(p.ok);
        CHECK(p.max_meshlet_vertices == 64 && p.max_meshlet_indices == 128);
        std::vector<uint8_t> vb(static_cast<size_t>(p.vertex_buffer_bytes));
        std::vector<uint8_t> ib(static_cast<size_t>(p.index_buffer_bytes));
        sc_mesh_compute_result r;
        CHECK(!sc_mesh_compute_execute(&big, 1, p, vb.data(), vb.size(), ib.data(), ib.size(), &r));
        CHECK(r.error != nullptr && r.error[0] != 0);
        CHECK(std::strstr(r.error, "per-group cap") != nullptr);

        // The same meshlet under a plan whose caps allow it is accepted: the check is
        // the plan's cap, not a hard-coded 64.
        sc_mesh_compute_limits wide;
        wide.threads_per_group = 160;          // 320 indices of headroom
        wide.max_meshlet_vertices = 128;
        wide.max_meshlet_indices = 300;
        wide.max_total_vertices = 256;
        wide.max_total_indices = 1024;
        const sc_mesh_compute_plan pw = sc_mesh_compute_plan_buffers(1, wide);
        CHECK(pw.ok);
        std::vector<uint8_t> vbw(static_cast<size_t>(pw.vertex_buffer_bytes));
        std::vector<uint8_t> ibw(static_cast<size_t>(pw.index_buffer_bytes));
        sc_meshlet fits = big;
        fits.index_count = 255; // multiple of 3, still over the 64-vertex plan's index cap
        fits.indices = indices.data();
        CHECK(!sc_mesh_compute_execute(&fits, 1, p, vb.data(), vb.size(), ib.data(), ib.size(), &r));
        CHECK(sc_mesh_compute_execute(&big, 1, pw, vbw.data(), vbw.size(), ibw.data(), ibw.size(), &r));
        CHECK(r.ok && r.vertices_written == 100 && r.indices_written == 300);

        // A meshlet exactly at the cap still works (4 vertices, 6 indices).
        fixture_mesh quad = make_quad(0.f, 0.f);
        sc_mesh_compute_limits tight;
        tight.threads_per_group = 4;
        tight.max_meshlet_vertices = 4;
        tight.max_meshlet_indices = 6;
        tight.max_total_vertices = 4;
        tight.max_total_indices = 6;
        const sc_mesh_compute_plan pt = sc_mesh_compute_plan_buffers(1, tight);
        CHECK(pt.ok);
        std::vector<uint8_t> vbt(static_cast<size_t>(pt.vertex_buffer_bytes));
        std::vector<uint8_t> ibt(static_cast<size_t>(pt.index_buffer_bytes));
        sc_mesh_compute_result rt;
        CHECK(sc_mesh_compute_execute(&quad.view, 1, pt, vbt.data(), vbt.size(), ibt.data(),
                                      ibt.size(), &rt));
        CHECK(rt.ok && rt.triangles == 2);
        CHECK(sc_mesh_compute_verify(&quad.view, 1, vbt.data(), vbt.size(), ibt.data(), ibt.size(),
                                     rt.args).ok);
    }

    banner("rejects: malformed meshlets and unusable buffers");
    {
        fixture_mesh tri = make_triangle(0.f, 0.f);
        sc_mesh_compute_limits lim;
        lim.max_total_vertices = 8;
        lim.max_total_indices = 24;
        const sc_mesh_compute_plan p = sc_mesh_compute_plan_buffers(1, lim);
        std::vector<uint8_t> vb(static_cast<size_t>(p.vertex_buffer_bytes));
        std::vector<uint8_t> ib(static_cast<size_t>(p.index_buffer_bytes));
        sc_mesh_compute_result r;

        sc_meshlet bad = tri.view;
        const uint32_t oob[3] = {0, 1, 3};
        bad.indices = oob;
        CHECK(!sc_mesh_compute_execute(&bad, 1, p, vb.data(), vb.size(), ib.data(), ib.size(), &r));
        CHECK(r.error != nullptr && r.error[0] != 0);

        const uint32_t not_tris[4] = {0, 1, 2, 0};
        bad = tri.view;
        bad.indices = not_tris;
        bad.index_count = 4;
        CHECK(!sc_mesh_compute_execute(&bad, 1, p, vb.data(), vb.size(), ib.data(), ib.size(), &r));

        bad = tri.view;
        bad.positions = nullptr;
        CHECK(!sc_mesh_compute_execute(&bad, 1, p, vb.data(), vb.size(), ib.data(), ib.size(), &r));

        const sc_meshlet* m = &tri.view;
        CHECK(!sc_mesh_compute_execute(m, 1, p, nullptr, vb.size(), ib.data(), ib.size(), &r));
        CHECK(!sc_mesh_compute_execute(m, 1, p, vb.data(), vb.size(), nullptr, ib.size(), &r));
        CHECK(!sc_mesh_compute_execute(m, 1, p, vb.data(), vb.size() - 1, ib.data(), ib.size(), &r));
        CHECK(!sc_mesh_compute_execute(m, 0, p, vb.data(), vb.size(), ib.data(), ib.size(), &r));
        sc_mesh_compute_plan bad_plan;
        CHECK(!sc_mesh_compute_execute(m, 1, bad_plan, vb.data(), vb.size(), ib.data(), ib.size(), &r));
    }

    banner("validator: corruption and bad arguments are caught");
    {
        fixture_mesh tri = make_triangle(0.f, 0.f);
        fixture_mesh quad = make_quad(2.f, 0.f);
        const sc_meshlet meshes[2] = {tri.view, quad.view};
        sc_mesh_compute_limits lim;
        lim.max_total_vertices = 8;
        lim.max_total_indices = 12;
        const sc_mesh_compute_plan p = sc_mesh_compute_plan_buffers(2, lim);
        std::vector<uint8_t> vb(static_cast<size_t>(p.vertex_buffer_bytes));
        std::vector<uint8_t> ib(static_cast<size_t>(p.index_buffer_bytes));
        sc_mesh_compute_result r;
        CHECK(sc_mesh_compute_execute(meshes, 2, p, vb.data(), vb.size(), ib.data(), ib.size(), &r));
        CHECK(sc_mesh_compute_verify(meshes, 2, vb.data(), vb.size(), ib.data(), ib.size(), r.args).ok);

        // A moved vertex breaks the multiset without breaking any bound.
        std::vector<uint8_t> vb_bad = vb;
        vb_bad[0] ^= 0x01u;
        const sc_mesh_compute_check moved =
            sc_mesh_compute_verify(meshes, 2, vb_bad.data(), vb_bad.size(), ib.data(), ib.size(), r.args);
        CHECK(!moved.ok);
        CHECK(moved.triangles_matched < moved.triangles_expected);
        CHECK(moved.indices_out_of_range == 0);

        // An index past the written vertices is caught as out of range.
        std::vector<uint8_t> ib_bad = ib;
        const uint32_t huge = 999u;
        std::memcpy(&ib_bad[0], &huge, 4);
        const sc_mesh_compute_check oob =
            sc_mesh_compute_verify(meshes, 2, vb.data(), vb.size(), ib_bad.data(), ib_bad.size(), r.args);
        CHECK(!oob.ok && oob.indices_out_of_range > 0);

        // Arguments that claim more indices than the buffer holds.
        sc_mesh_draw_args big_args = r.args;
        big_args.index_count = static_cast<uint32_t>(p.index_buffer_bytes / 4u) + 3u;
        const sc_mesh_compute_check big =
            sc_mesh_compute_verify(meshes, 2, vb.data(), vb.size(), ib.data(), ib.size(), big_args);
        CHECK(!big.ok && !big.bounds_ok);

        // A second instance is not what this draw encodes.
        sc_mesh_draw_args two = r.args;
        two.instance_count = 2;
        CHECK(!sc_mesh_compute_verify(meshes, 2, vb.data(), vb.size(), ib.data(), ib.size(), two).ok);
    }

    banner("grid: 16 synthetic meshlets fill the caps exactly");
    {
        std::vector<fixture_mesh> cells;
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                cells.push_back(make_quad(static_cast<float>(x) * 2.f, static_cast<float>(y) * 2.f));
            }
        }
        // Rebuild the views from the final vector: sc_meshlet holds raw pointers,
        // so it must not outlive a copy of the object that owns the arrays.
        std::vector<sc_meshlet> views;
        for (size_t i = 0; i < cells.size(); ++i) {
            sc_meshlet v{};
            v.positions = cells[i].positions.data();
            v.colors = cells[i].colors.data();
            v.indices = cells[i].indices.data();
            v.vertex_count = 4;
            v.index_count = 6;
            views.push_back(v);
        }
        sc_mesh_compute_limits lim;
        lim.max_total_vertices = 64;
        lim.max_total_indices = 96;
        const sc_mesh_compute_plan p =
            sc_mesh_compute_plan_buffers(static_cast<uint32_t>(views.size()), lim);
        CHECK(p.ok && p.groups == 16);
        std::vector<uint8_t> vb(static_cast<size_t>(p.vertex_buffer_bytes));
        std::vector<uint8_t> ib(static_cast<size_t>(p.index_buffer_bytes));
        sc_mesh_compute_result r;
        CHECK(sc_mesh_compute_execute(views.data(), static_cast<uint32_t>(views.size()), p, vb.data(),
                                      vb.size(), ib.data(), ib.size(), &r));
        CHECK(r.ok && !r.clamped);
        CHECK(r.vertices_written == 64 && r.indices_written == 96 && r.triangles == 32);
        const sc_mesh_compute_check c = sc_mesh_compute_verify(
            views.data(), static_cast<uint32_t>(views.size()), vb.data(), vb.size(), ib.data(),
            ib.size(), r.args);
        CHECK(c.ok);
        CHECK(c.triangles_expected == 32 && c.triangles_matched == 32);
        CHECK(c.vertices_checked == 96);
        std::printf("   grid: %u groups, %u vertices, %u indices, %u triangles, all matched\n",
                    r.groups_dispatched, r.vertices_written, r.indices_written, r.triangles);
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
