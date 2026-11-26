# victory-mesh — mesh-as-compute translator

The mesh-shader translator for running GTA 6 on a GPU with no mesh
shaders (here, the RX 5700 XT). It turns a captured meshlet table into
the vertex indices, primitive indices, and draw args a compute shader
emits, plus the HLSL for that compute shader. The game thinks mesh
shaders exist; the host refuses the mesh pipeline and this crate
reproduces the draw as compute instead.

## Layout (verified against captured buffers)

- 128 threads/group, one meshlet/group, fixed 128 verts / 128 prims,
  no atomic claim.
- Header (8 bytes) + N 16-byte meshlet records at 12 + 16*group,
  payload at 12 + N*16.
- Vertex-index table: one u16 per vertex (parity-selected from a u32
  load). Primitive table: one u32 per primitive.
- **Primitive indices are three 7-bit values packed contiguously in
  the low 21 bits** (v0 bits 0-6, v1 bits 7-13, v2 bits 14-20) — NOT
  10-bit. The 7-bit unpack gives clean sequential triangles
  (0,1,2), (2,3,0), (4,5,6) on real buffers; the 10-bit unpack fails
  almost everywhere. The translator MUST use the 7-bit unpack.
- Words +8/+12 of a record are culling data the mesh shader does not
  read; they do not block the translator.

See `CONTRACT.md` for the full retargeted contract (measured output
signatures, dispatch, I/O, Phase 3 draw, verification).

## Status

- Geometry core verified: `cargo test` passes, including
  `captured_tables_verify` against real captured buffers (0 bad
  indices; totals 56/28, 2204/2572, 10785/13249).
- HLSL emission verified: the generated compute shader compiles with
  `dxc -T cs_6_6` to valid DXIL.
- **Menu interpolant lift done:** `src/interpolant.rs` + `emit_menu_hlsl()`
  generate the menu shader's real 9-float interpolants (cbuffer transform
  of a format-dispatched attribute, SNORM16 decode, colour byte / 255),
  lifted from the captured DXIL. The emitted menu HLSL compiles with
  `dxc -T cs_6_6` to valid DXIL (6,152 bytes). `cargo test` 7/7,
  `cargo clippy --all-targets` clean. The shader takes one extra input,
  `CBufferHeap` (t3): the 64 KiB heap cbuffer the host supplies. See
  `MESH-INTERPOLANT-LIFT-FINDINGS.md` (in the parent docs) for the exact
  cbuffer rows, the offset arithmetic, and the open items (heap-cbuffer
  capture, Phase 3 wiring, world shaders).
- The generic geometry layer (`emit_geometry_hlsl`) still writes a zeros
  stub for the interpolants; use `emit_menu_hlsl()` for the menu shader.

## Shader-translator files (`shader/`)

Reference copies of the shader translator's mesh work (from
`crates/victory-shader/src/emit/` in the game client). These are NOT
part of the `victory-mesh` crate build — they depend on
`victory-shader` types and are kept here as reference, not compiled
by this crate's `Cargo.toml`.

- `shader/bary.rs` — barycentrics emulation for a host without hardware
  `SV_Barycentrics` (RX 5700 XT): the translated pixel shader reads the
  weights as the ordinary input `BARY_SEMANTIC`, and the vertex side
  puts out (1, 0, 0), (0, 1, 0), (0, 0, 1) at a triangle's three
  corners. Fixes 4 refused pixel shaders and ~230 dropped draws/frame,
  including the character's hair. (2026-11-26.)
- `shader/mesh_compute.rs` — the general mesh-to-compute form: every mesh
  shader is compiled as a compute shader that writes its vertices and
  triangles into buffers, followed by an ordinary draw of those buffers.
  Covers all mesh shaders at once (not one-at-a-time lifts). The reader
  vertex shader (`reader_vs`) supplies the barycentric weights itself.
  (2026-11-26.)

## Build and test

```sh
cargo test -p victory-mesh
cargo clippy -p victory-mesh --all-targets
cargo run -p victory-mesh --example gen_hlsl
```

Requires Rust 1.88+. GPL-3.0-or-later.
