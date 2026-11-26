# Mesh-as-compute — Retargeted contract (replaces Phase 8)

## Measured facts (from the captures, not assumed)

### Output signatures — the per-vertex interpolant set

Measured with `dxc -dumpbin` on the captured DXIL:

**Menu shader `d4111a6e6ea49d91` — 3 rows, 9 floats/vertex:**
- SV_Position 0, xyzw, float
- TEXCOORD 1, xyz, float
- TEXCOORD 2, xy, float

**World shader `093c64546c55554e` — 20 rows, 56 scalars/vertex:**
- SV_Position 0, xyzw, float
- TEXCOORD 0–4, xyzw / xyzw / xyzw / xyzw / xyz, float
- TEXCOORD 5–8, xyz / xyz / xyz / w (split across registers 5–7), float
- TEXCOORD 11–19, xyzw / xyz / xyz / xyzw / xyzw / xyz, float
- COLOR 0, x, uint
- Total: 55 float components + 1 uint = 56 scalars = **224 bytes/vertex
  packed** (15 registers x 16 bytes = 240 bytes register-aligned).

Note: an earlier report said "57 outputs" for this shader. Measured: 20
signature rows, 56 used scalar components. The magnitude is what matters:
a world vertex carries ~56 scalars, not 7.

### Meshlet geometry (confirmed by buffer bytes)

- 128 threads/group, one meshlet per group. Group count = meshlet count N
  (from the table header). `setMeshOutputCounts(128, 128)` is constant.
- Caps are fixed 128 verts / 128 prims. No atomic claim, no counters, no
  drop rule — overrun is structurally impossible.
- Records: 16 bytes at 12 + 16*groupId. w0/w1 = counts + table offsets
  (17-bit each). w2/w3 = culling data, NOT read by the mesh shader; the
  translator may carry them or ignore them.
- Vertex-index table: u16 per vertex. Primitive table: one u32 per
  primitive, **three 7-bit indices packed contiguously in the low 21 bits
  (v0 = bits 0–6, v1 = bits 7–13, v2 = bits 14–20)** — NOT 10-bit. The
  Phase 2a report's "3 × 10-bit indices after Bfrev" was a misreading:
  the shader's Bfrev + 1023-mask sequence (after a repack that spreads the
  7-bit fields to 10-bit spacing) extracts these same 7-bit values, but
  the stored format is 3×7-bit. Verified: the 7-bit unpack yields clean
  sequential triangles (0,1,2), (2,3,0), (4,5,6) with 0 out-of-range
  indices across three buffers (N=1, N=21, N=105; 39,747 indices total),
  while the 10-bit unpack fails almost everywhere. The reference model
  (`mesh_refmodel.py`) implements the 7-bit unpack and passes on all three
  buffers. The translator MUST use the 7-bit unpack.

## The contract (replaces Phase 8's 28-byte record)

### Dispatch

- One compute dispatch per mesh pipeline (per draw). 128 threads per
  group, one meshlet per group, group count = N (meshlet count from the
  table header). No indirect dispatch needed (N is known from the header
  before dispatch).

### Inputs (per meshlet)

- The meshlet table buffer (header + records + payload), as captured.
- The vertex attribute buffer(s), as captured (the shader's attribute
  descriptors give format id, stride, base offset; address = base +
  stride x index).
- The mesh shader's DXIL (for the attribute-fetch and interpolation
  logic the translator lifts).

### Compute shader behaviour (per thread)

- Thread `t` (0..127) in group `g` (0..N-1) reads meshlet record `g`.
- If `t < vertexCount`: reads vertex-index entry `t` from the
  vertex-index table (u16, parity-selected from a u32 load), fetches that
  vertex's full attribute set from the vertex buffer, and writes the full
  interpolant set (per the pipeline's output signature) to the output
  vertex buffer at slot `(g * 128) + t`.
- If `t < primitiveCount`: reads primitive entry `t` from the primitive
  table (u32, Bfrev, 3 x 10-bit indices into the meshlet's vertex table),
  and writes the triangle's three indices to the output index buffer at
  slot `(g * 128 + t) * 3`, each index rebased by the meshlet's vertex
  base (`g * 128`).

### Outputs (per meshlet, concatenated across groups)

- **Vertex buffer:** `N * 128` vertices max, each of per-vertex size =
  the pipeline's output-signature used components x 4 bytes (224 bytes
  for the 093c pipeline; 36 bytes for the menu pipeline). Layout:
  register-aligned float4 slots matching the output-signature registers,
  so Phase 3 can draw it with a normal vertex-input pipeline whose input
  layout is the mesh shader's output signature.
- **Index buffer:** `N * 128` triangles max, each 3 x u32. Indices are
  the meshlet-local 10-bit values rebased by the meshlet vertex base.
  (u32, not u16: the rebased index can exceed 65535 across many meshlets.)
- **Draw args:** one `D3D12_DRAW_INDEXED_ARGUMENTS` (20 bytes) per
  pipeline, filled by the compute shader (or by the host after the
  dispatch) from the actual counts: IndexCountPerInstance = total
  primitives x 3, InstanceCount = 1, StartIndexLocation = 0,
  BaseVertexLocation = 0, StartInstanceLocation = 0. Phase 3 issues a
  `DrawIndexedInstanced` (or indirect) with these args.

### What Phase 3 does with it

Phase 3 (victory-gpu wiring) takes the compute shader's vertex buffer,
index buffer, and draw args, and draws them with a normal vertex-input
graphics pipeline whose input layout is the mesh shader's output
signature (position + TEXCOORDs + COLOR as measured). The pixel shader is
the pipeline's own pixel shader, unchanged. No mesh shader is created on
the host.

## Verification

- A CPU reference model (update the Phase 8 model to this contract:
  128 threads, 128/128 caps, full interpolant set) must produce the same
  vertex buffer, index buffer, and draw args as the translator for the
  same meshlet-table + vertex-buffer inputs, compared as
  order-independent multisets of triangles and per-vertex interpolant
  tuples.
- The translator must build with `cargo build` clean (zero warnings) and
  must not break the menu.
- The meshlet record format comes ONLY from the Phase 2a layout document
  plus the buffer-byte findings (MESH-PHASE2B-STEP1-FINDINGS.md). Never
  invent a field.

## Open questions (do not block the translator)

- Exact per-vertex packing: register-aligned float4 slots (240 bytes for
  093c) vs packed used-components (224 bytes). The contract specifies
  register-aligned, matching the output-signature registers; confirm
  against the pixel shader's expected input layout when Phase 3 wires it.
- Vertex-attribute format ids (0–15, with 2 and 6 taking special paths)
  are the title's own enum. Resolve each by matching the shader's
  attribute loads against the captured vertex-buffer bytes.
- w2/w3 (record words +8/+12) are culling data; their exact semantic is
  still undecoded. They are not needed for the geometry path.
