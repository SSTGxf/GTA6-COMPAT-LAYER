# Shader pipeline — Phase 8: mesh-as-compute

Status: **started, buildable, and self-verified.** The CPU reference model of the compute-to-draw
contract is in `src/shader/sc_mesh_compute.cpp`, its harness passes 79/79 checks, and the module is
part of the standalone build gate (C++17 and C++14, `/W4 /WX`, zero warnings). It does **not** run on
a GPU and has **not** seen a real mesh shader: there is no corpus until the layer's PSO slots 86/16
create PSOs.

Phase numbering used here is the one in the completion prompt: Phase 6 is resources/pipeline
(corpus-gated), Phase 7 is BCPack (B1/B2 exist in the layer; B3–B5 are not this module's job),
Phase 8 is mesh-as-compute. Nothing in this phase touches BCPack.

## 1. Why compute-to-draw

The RX 5700 XT is gfx10.3 / FL 12_1 with no mesh-shader tier, so a DX12 mesh pipeline cannot be
created on it at all. Phase 8's job is to run the mesh shader's work as compute and then draw the
result with a normal indexed draw:

1. a compute dispatch runs one thread group per meshlet;
2. each group claims space in a shared vertex buffer and index buffer with an atomic add on two
   counters, writes its vertices, and writes its indices rebased on the claimed vertex base;
3. the counters become the arguments of a `DrawIndexedInstanced`-style indirect draw
   (`D3D12_DRAW_INDEXED_ARGUMENTS`).

## 2. What is fixed by this module

| item | value | where |
|---|---|---|
| vertex record | 28 bytes: `float3` clip-space position, `float4` colour | `kScMeshVertexStrideBytes` |
| draw arguments | 20 bytes, five `uint32`: indexCount, instanceCount, startIndex, baseVertex, startInstance | `sc_mesh_draw_args_encode/decode` |
| counters | two `uint32`: vertices written, indices written | `kScMeshCounterBytes` |
| group | 64 threads, one meshlet per group | `kScMeshThreadsPerGroup` |
| meshlet caps | 64 vertices, 128 indices per group, copied into the plan and **enforced by execute** (a bigger meshlet is rejected, not dropped) | `kScMeshMaxMeshlet*`, `sc_mesh_compute_plan` |
| cap consistency | a group emits one vertex and two indices per thread, so `max_meshlet_vertices <= threads_per_group` and `max_meshlet_indices <= 2 * threads_per_group`; an inconsistent set is rejected by the plan | `sc_mesh_compute_plan_buffers` |
| buffer sizing | from the caps, not from the input, so the GPU path allocates once | `sc_mesh_compute_plan_buffers` |
| compaction | one atomic claim per group; a meshlet that would overrun a cap is dropped whole | `sc_mesh_compute_execute` |

The 28-byte record is the layout the existing RDNA1 mesh fallback vertex shader already consumes
(`include/opt_mesh_fallback.h`), so a Phase 8 draw can reuse that shader without a second format.

## 3. What is not fixed, and why

- **The guest's meshlet record format.** No capture contains one, and inventing it would be a guess.
  The caller supplies `sc_meshlet` arrays; the module never parses guest bytes.
- **The amplification stage.** Meshlet selection and culling stay with the caller (the same split the
  Phase 3 fallback uses).
- **The dispatch's real concurrency.** Groups may run in any order on hardware, so the vertex order
  in the output buffer is not defined. Every check here is order independent.

## 4. How it proves itself

`tests/shader/build_mesh_compute_test.bat` builds one binary (`/W4 /WX`, no D3D header, no emitter)
and runs it:

```
== plan: buffer sizes come from the caps, not the input ==
== arguments: 20-byte DRAW_INDEXED_ARGUMENTS layout ==
== execute: one meshlet, exact bytes and arguments ==
== execute: three meshlets compact into one draw ==
== caps: a meshlet that does not fit is dropped whole ==
== caps: the per-group meshlet limits are enforced by execute ==
== rejects: malformed meshlets and unusable buffers ==
== validator: corruption and bad arguments are caught ==
== grid: 16 synthetic meshlets fill the caps exactly ==
   grid: 16 groups, 64 vertices, 96 indices, 32 triangles, all matched

97 checks, 0 failed
```

What each block pins:

- **Arguments.** The five `uint32` sit at byte offsets 0/4/8/12/16 little endian; a wrong order is
  silent corruption on a real draw, so the encode/decode round-trip and the raw bytes are asserted.
- **Exact output.** For one meshlet the 84 vertex bytes must equal the source records and the index
  buffer must hold 0,1,2; for three meshlets the quad's indices must be rebased to 3,4,5 and the
  strip's to 7,8,9.
- **Compaction.** Three meshlets (3+4+4 vertices, 3+6+6 indices) must produce 11 vertices, 15
  indices, 5 triangles and one argument record with `indexCount = 15`.
- **Caps.** With a 4-vertex buffer the triangle fits and the quad is dropped whole: emitted 1,
  dropped 1, `clamped` set, and the validator agrees the buffers describe only the triangle.
- **Per-group caps.** A 100-vertex meshlet is rejected even when the buffers are large enough to
  hold it (`"meshlet larger than the per-group cap"`); the same meshlet is accepted under a plan
  whose caps allow it, so the limit is the plan's, not a hard-coded 64; a meshlet exactly at the cap
  still expands and validates; and a limit set one group could never emit
  (`max_meshlet_vertices > threads_per_group`, `max_meshlet_indices > 2 * threads_per_group`) is
  refused by `sc_mesh_compute_plan_buffers` instead of producing an unsatisfiable plan.
- **Order independence.** Reversing the group order changes the bytes but not the triangle multiset.
  This is the check a GPU capture will have to pass, because its order will not be ours.
- **Corruption.** A single flipped vertex byte, an out-of-range index, an argument list longer than
  the buffer, and `instanceCount = 2` must each fail the validator.

## 5. How to run it

```bat
tests\shader\build_mesh_compute_test.bat        rem -> 97 checks, 0 failed, exit 0
tests\shader\build_all_no_dxc.bat               rem -> standalone gate, includes this module
tests\shader\build_link_smoke.bat               rem -> no-DXC path still links without the emitter
```

## 6. What is still open

- **No GPU pass yet.** The dispatch, the atomics and the indirect draw are Freebuff's side; this
  module is the contract and the oracle they will be checked against.
- **No real meshlet data.** Every fixture here is synthetic and labelled as such. When the layer's
  PSO slots 86/16 create PSOs and a capture yields a real mesh shader, the next step is to run its
  meshlets through this contract and see whether the guest's format maps onto `sc_meshlet` at all;
  if it does not, the format belongs here as a parser, not as a guess.
- **No performance claim.** The reference model is single-threaded and exists to be correct.
