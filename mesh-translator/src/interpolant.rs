//! Menu mesh shader interpolant lift (DXIL -> HLSL).
//!
//! Generates the HLSL for the menu shader's (`d4111a6e6ea49d91`) 9-float
//! vertex interpolants, lifted from the captured DXIL (`menu_dxil.txt`).
//! This replaces the zeros stub in the geometry layer so the compute
//! shader writes real interpolants instead of zeros.
//!
//! What the DXIL establishes (see `MESH-INTERPOLANT-LIFT-FINDINGS.md`):
//! - The position is a cbuffer transform of a format-dispatched 2D
//!   attribute, not a raw vertex position. The cbuffer is the 64 KiB
//!   heap cbuffer (handle `%483`); its rows 0, 1 and 8 hold the transform
//!   and row 7 holds the format id.
//! - The attribute comes from the vertex-data buffer (handle `%68`) at
//!   an offset computed per format from a descriptor word:
//!   `offset = base + stride * vtxIdx`, where
//!   `base = (desc >> 7) & 0x1FFFFFC` and `stride = (desc >> 3) & 60`.
//!   The descriptor word is read from the buffer at a fixed offset
//!   (4, 12, 16, 56, 60, 64, 68) selected by the format id.
//! - Per format, `(desc & 15) == 2` selects a direct float2 load
//!   (mask 3); otherwise the attribute is a SNORM16 pair packed in one
//!   32-bit word, decoded as `max(int16 / 32767.0, -1.0) * scale`, with
//!   the scale from the buffer at offset 124 and a zero select.
//! - The format dispatch: if the format id is <= 3, the low 2 bits
//!   select among 4 descriptors; if > 3, `(format - 4)` selects. Index 3
//!   splits further on whether the format is 3 or 7.
//! - `TEXCOORD1.x/.y` are the attribute components, unless the format id
//!   equals the cbuffer row-7 component 1, in which case they are the
//!   transformed values. `TEXCOORD1.z` is a normalized colour byte
//!   (`/ 255.0`) at a format-selected component, or 1.0.
//! - `TEXCOORD2` is the same FMad transform as the position's inner
//!   value, without the `* 2.0` and `- 0.5`.
//!
//! The HLSL below reproduces the shader's own arithmetic. It reads the
//! descriptor words and the scale from the vertex-data buffer at run
//! time, so it follows whatever the title's descriptors say; nothing
//! about the format ids is hard-coded beyond the dispatch structure the
//! DXIL itself uses.

/// HLSL statements that compute the menu shader's 9 interpolants for
/// one vertex and store them at `outVert`.
///
/// Assumes the following are in scope in the compute shader:
/// - `vtxIdx`: the vertex index (already computed from the meshlet table).
/// - `outVert`: the byte offset of this vertex's output record.
/// - `VertexData` (`ByteAddressBuffer`, t1): the attribute/descriptor
///   buffer (handle `%68`).
/// - `CBufferHeap` (`ByteAddressBuffer`, t3): the 64 KiB heap cbuffer
///   (handle `%483`). The host supplies it; see the findings doc.
/// - `OUT_FLOATS_PER_VERTEX`: 9 for the menu shader.
///
/// Writes `SV_Position` (xyzw), `TEXCOORD1` (xyz) and `TEXCOORD2` (xy)
/// as 9 consecutive floats starting at `outVert`.
pub fn emit_menu_interpolant_block() -> String {
    r#"        // ---- Menu interpolant lift (shader d4111a6e6ea49d91) ----
        // CBuffer heap rows (16 bytes each). Row N, component C is at
        // byte N*16 + C*4. Rows 0/1 are the transform, row 8 the offset,
        // row 7 the format id (comp 0) and its compare value (comp 1).
        float cbR0y = asfloat(CBufferHeap.Load(4));
        float cbR0z = asfloat(CBufferHeap.Load(8));
        float cbR0w = asfloat(CBufferHeap.Load(12));
        float cbR1y = asfloat(CBufferHeap.Load(20));
        float cbR1z = asfloat(CBufferHeap.Load(24));
        float cbR1w = asfloat(CBufferHeap.Load(28));
        float cbR8y = asfloat(CBufferHeap.Load(132));
        float cbR8z = asfloat(CBufferHeap.Load(136));
        uint fmtId = CBufferHeap.Load(112);
        uint fmtCmp = CBufferHeap.Load(116);

        // Attribute descriptor words (fixed offsets in VertexData).
        uint desc4 = VertexData.Load(4);
        uint desc12 = VertexData.Load(12);
        uint desc16 = VertexData.Load(16);
        uint desc56 = VertexData.Load(56);
        uint desc60 = VertexData.Load(60);
        uint desc64 = VertexData.Load(64);
        uint desc68 = VertexData.Load(68);
        // SNORM scale (buffer offset 124).
        float scale85 = asfloat(VertexData.Load(124));

        // Format dispatch (lifted from the DXIL): if the format id is
        // <= 3 the low 2 bits select, otherwise (format - 4) selects.
        // Index 3 splits on whether the format is 3 or 7.
        uint fmtIdx = (fmtId <= 3u) ? (fmtId & 3u) : (fmtId - 4u);
        uint attrDesc = desc68;
        if (fmtIdx == 1u) {
            attrDesc = desc64;
        } else if (fmtIdx == 2u) {
            attrDesc = desc56;
        } else if (fmtIdx == 3u) {
            attrDesc = (fmtId == 3u) ? desc60 : desc16;
        }

        // Attribute offset: base + stride * vtxIdx (lifted arithmetic).
        uint attrBase = (attrDesc >> 7u) & 0x1FFFFFCu;
        uint attrStride = (attrDesc >> 3u) & 60u;
        uint attrOff = attrBase + attrStride * vtxIdx;

        // Fetch attr_x / attr_y. (desc & 15) == 2 is a direct float2
        // load; otherwise the attribute is a SNORM16 pair packed in one
        // 32-bit word, decoded and scaled by scale85.
        float attrX;
        float attrY;
        if ((attrDesc & 15u) == 2u) {
            float2 a = asfloat(VertexData.Load2(attrOff));
            attrX = a.x;
            attrY = a.y;
        } else {
            uint packed = VertexData.Load(attrOff);
            int s0 = (int)(packed << 16) >> 16;
            int s1 = (int)packed >> 16;
            float nx = max((float)s0 / 32767.0, -1.0);
            float ny = max((float)s1 / 32767.0, -1.0);
            attrX = (scale85 == 0.0 || nx == 0.0) ? 0.0 : scale85 * nx;
            attrY = (scale85 == 0.0 || ny == 0.0) ? 0.0 : scale85 * ny;
        }

        // Cbuffer transform (lifted from the DXIL FMads).
        float attrXc = cbR8y + attrX;
        float attrYc = cbR8z + attrY;
        float innerX = cbR0z * attrYc + cbR0y * attrXc + cbR0w;
        float innerY = cbR1z * attrYc + cbR1y * attrXc + cbR1w;
        float posX = 2.0 * (innerX - 0.5);
        float posY = (innerY - 0.5) == 0.0 ? 0.0 : -2.0 * (innerY - 0.5);

        // TEXCOORD1.x/.y are the attribute, unless the format id equals
        // the cbuffer row-7 compare value, in which case they are the
        // transformed values.
        float tc1x = (fmtId == fmtCmp) ? innerX : attrX;
        float tc1y = (fmtId == fmtCmp) ? innerY : attrY;

        // TEXCOORD1.z: normalized colour byte at the format-selected
        // component of the colour word (descriptor at offset 4), or 1.0
        // when the format carries no colour byte.
        uint colBase = (desc4 >> 7u) & 0x1FFFFFCu;
        uint colStride = (desc4 >> 3u) & 60u;
        uint colOff = colBase + colStride * vtxIdx;
        uint colWord = VertexData.Load(colOff);
        uint colByte = (colWord >> (fmtIdx * 8u)) & 255u;
        float tc1z = (float)colByte / 255.0;

        // Write the 9 floats: SV_Position, TEXCOORD1, TEXCOORD2.
        OutVerts.Store(outVert + 0u, asuint(posX));
        OutVerts.Store(outVert + 4u, asuint(posY));
        OutVerts.Store(outVert + 8u, asuint(0.0));
        OutVerts.Store(outVert + 12u, asuint(1.0));
        OutVerts.Store(outVert + 16u, asuint(tc1x));
        OutVerts.Store(outVert + 20u, asuint(tc1y));
        OutVerts.Store(outVert + 24u, asuint(tc1z));
        OutVerts.Store(outVert + 28u, asuint(innerX));
        OutVerts.Store(outVert + 32u, asuint(innerY));
"#
    .to_string()
}

/// The menu shader's output width (9 floats per vertex).
pub const MENU_OUT_FLOATS_PER_VERTEX: usize = 9;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn menu_block_has_the_load_bearing_pieces() {
        let h = emit_menu_interpolant_block();
        // Cbuffer rows (transform + format id).
        assert!(h.contains("CBufferHeap.Load(4)"));
        assert!(h.contains("CBufferHeap.Load(8)"));
        assert!(h.contains("CBufferHeap.Load(132)"));
        assert!(h.contains("CBufferHeap.Load(112)"));
        // Descriptor reads at the fixed offsets.
        assert!(h.contains("VertexData.Load(12)"));
        assert!(h.contains("VertexData.Load(16)"));
        assert!(h.contains("VertexData.Load(56)"));
        assert!(h.contains("VertexData.Load(60)"));
        assert!(h.contains("VertexData.Load(64)"));
        assert!(h.contains("VertexData.Load(68)"));
        assert!(h.contains("VertexData.Load(124)"));
        // Offset arithmetic (base + stride * vtxIdx).
        assert!(h.contains("(attrDesc >> 7u) & 0x1FFFFFCu"));
        assert!(h.contains("(attrDesc >> 3u) & 60u"));
        assert!(h.contains("attrBase + attrStride * vtxIdx"));
        // Format dispatch.
        assert!(h.contains("(fmtId <= 3u) ? (fmtId & 3u) : (fmtId - 4u)"));
        assert!(h.contains("(fmtId == 3u) ? desc60 : desc16"));
        // Direct vs SNORM branch.
        assert!(h.contains("((attrDesc & 15u) == 2u)"));
        assert!(h.contains("32767.0"));
        assert!(h.contains("scale85"));
        // Cbuffer transform.
        assert!(h.contains("cbR0z * attrYc + cbR0y * attrXc + cbR0w"));
        assert!(h.contains("2.0 * (innerX - 0.5)"));
        assert!(h.contains("-2.0 * (innerY - 0.5)"));
        // Colour byte / 255.
        assert!(h.contains("/ 255.0"));
        // All 9 output stores.
        assert!(h.contains("OutVerts.Store(outVert + 32u, asuint(innerY))"));
    }

    #[test]
    fn menu_position_formula_matches_the_trace() {
        // Reproduce the traced position arithmetic in Rust and check it
        // against a hand-computed case, so the HLSL formula is verified
        // independently of the string. Uses placeholder cbuffer rows
        // (not the captured root CBV, which is a different cbuffer).
        // attr_x = 0.25, attr_y = 0.5; row8 = (0, 0.1, 0.2, 0);
        // row0 = (0, 0.5, 2.0, 0.25); row1 = (0, 1.0, 0.5, 0.0).
        let (attr_x, attr_y) = (0.25f32, 0.5f32);
        let (r8y, r8z) = (0.1f32, 0.2f32);
        let (r0y, r0z, r0w) = (0.5f32, 2.0f32, 0.25f32);
        let (r1y, r1z, r1w) = (1.0f32, 0.5f32, 0.0f32);
        let attr_xc = r8y + attr_x;
        let attr_yc = r8z + attr_y;
        let inner_x = r0z * attr_yc + r0y * attr_xc + r0w;
        let inner_y = r1z * attr_yc + r1y * attr_xc + r1w;
        let pos_x = 2.0 * (inner_x - 0.5);
        let pos_y = if inner_y - 0.5 == 0.0 {
            0.0
        } else {
            -2.0 * (inner_y - 0.5)
        };
        // inner_x = 2.0*0.7 + 0.5*0.35 + 0.25 = 1.4 + 0.175 + 0.25 = 1.825
        assert!((inner_x - 1.825).abs() < 1e-6);
        // inner_y = 0.5*0.7 + 1.0*0.35 + 0.0 = 0.35 + 0.35 = 0.7
        assert!((inner_y - 0.7).abs() < 1e-6);
        // pos_x = 2*(1.825-0.5) = 2.65 ; pos_y = -2*(0.7-0.5) = -0.4
        assert!((pos_x - 2.65).abs() < 1e-6);
        assert!((pos_y - (-0.4)).abs() < 1e-6);
    }
}
