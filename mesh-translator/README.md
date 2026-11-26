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
- The vertex interpolants (positions, texcoords) are still a zeros
  stub in the emitted HLSL. They are a cbuffer transform of a 2D
  attribute, not a raw vertex position — reproducing them is the DXIL
  lift, which needs the captured CBV (the 48-byte CB0). The CBV
  capture is in the game-client (`draw.rs`, `c{slot}d{i}.buf`).

## Build and test

```sh
cargo test -p victory-mesh
cargo clippy -p victory-mesh --all-targets
cargo run -p victory-mesh --example gen_hlsl
```

Requires Rust 1.88+. GPL-3.0-or-later.
