//! Mesh-as-compute geometry core.
//!
//! Parses a captured meshlet table and computes the vertex indices,
//! primitive indices, and draw args a compute shader emits for it. This is
//! the CPU reference model for the retargeted contract: 128 threads per
//! group, one meshlet per group, fixed 128 verts / 128 prims, no atomic
//! claim, and the compute shader writes the pipeline's full interpolant
//! set (not Phase 8's 28-byte record).
//!
//! Layout (verified against captured buffers, see the reference model):
//! the table is a header (u32 count N in the low 17 bits of word 0, u32
//! base in word 1) plus N 16-byte records at 12 + 16*group, then the
//! payload at 12 + N*16. A record's words 0/1 hold the vertex and
//! primitive counts (each count-1 in 7 bits) and the 17-bit vertex-index
//! and primitive table offsets. Words 2/3 are culling data the mesh
//! shader does not read. The vertex-index table holds one u16 per vertex
//! (parity-selected from a u32 load). The primitive table holds one u32
//! per primitive with **three 7-bit indices packed contiguously in the
//! low 21 bits** (v0 bits 0-6, v1 bits 7-13, v2 bits 14-20) — NOT 10-bit.
//! The shader's Bfrev + 1023-mask sequence extracts these same 7-bit
//! values after a repack; the stored format is 7-bit.

pub mod emit;
pub mod interpolant;
pub mod meshlet;

pub use emit::{emit_geometry_hlsl, emit_menu_hlsl};
pub use meshlet::{
    DrawIndexedArguments, MeshletError, MeshletRecord, MeshletTable, parse_table,
};
