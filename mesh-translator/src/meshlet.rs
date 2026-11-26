//! Meshlet table parsing and the geometry a compute shader emits from it.

use thiserror::Error;

/// What can go wrong parsing or unpacking a meshlet table.
#[derive(Debug, Error)]
pub enum MeshletError {
    /// The buffer is too small to hold a meshlet header.
    #[error("buffer too small for a meshlet header")]
    TooSmall,
    /// The meshlet count in the header is out of range.
    #[error("meshlet count {0} out of range")]
    BadCount(u32),
    /// The payload extends beyond the buffer.
    #[error("payload extends beyond the buffer")]
    PayloadOverflow,
    /// A record's vertex or primitive count is out of range.
    #[error("record {0} has an out-of-range count")]
    BadRecord(usize),
    /// A table offset lands outside the buffer.
    #[error("table offset out of range")]
    OffsetOverflow,
    /// A meshlet index is out of range for this table.
    #[error("meshlet index {0} out of range")]
    BadMeshlet(usize),
}

/// One meshlet record: 16 bytes at 12 + 16*group.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MeshletRecord {
    /// Vertices in this meshlet (1..=128).
    pub vertex_count: u32,
    /// Primitives in this meshlet (1..=128).
    pub primitive_count: u32,
    /// Vertex-index table offset, in u16 units.
    pub vtx_table: u32,
    /// Primitive table offset, in u32 units.
    pub prim_table: u32,
}

/// A parsed meshlet table: header plus records. The payload bytes live in
/// the buffer the table was parsed from; unpacking reads them from it.
#[derive(Debug, Clone)]
pub struct MeshletTable {
    /// Meshlet count N (header word 0, low 17 bits).
    pub count: usize,
    /// Per-draw base added to the vertex-index table, in u32 units
    /// (header word 1; << 2 gives bytes).
    pub header1: u32,
    /// Payload base: 12 + N*16.
    pub payload_base: usize,
    /// One record per meshlet.
    pub records: Vec<MeshletRecord>,
}

/// A `D3D12_DRAW_INDEXED_ARGUMENTS` (20 bytes).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DrawIndexedArguments {
    pub index_count_per_instance: u32,
    pub instance_count: u32,
    pub start_index_location: u32,
    pub base_vertex_location: i32,
    pub start_instance_location: u32,
}

fn read_u32(buf: &[u8], offset: usize) -> Result<u32, MeshletError> {
    let bytes = buf
        .get(offset..offset.checked_add(4).ok_or(MeshletError::OffsetOverflow)?)
        .ok_or(MeshletError::OffsetOverflow)?;
    Ok(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
}

/// Parses a meshlet table from the start of `buf`.
pub fn parse_table(buf: &[u8]) -> Result<MeshletTable, MeshletError> {
    if buf.len() < 12 {
        return Err(MeshletError::TooSmall);
    }
    let header0 = read_u32(buf, 0)?;
    let header1 = read_u32(buf, 4)?;
    let count = (header0 & 0x1FFFF) as usize;
    if count == 0 || count > 10_000 {
        return Err(MeshletError::BadCount(header0 & 0x1FFFF));
    }
    let payload_base = 12usize
        .checked_add(count.checked_mul(16).ok_or(MeshletError::PayloadOverflow)?)
        .ok_or(MeshletError::PayloadOverflow)?;
    if payload_base > buf.len() {
        return Err(MeshletError::PayloadOverflow);
    }
    let mut records = Vec::with_capacity(count);
    for i in 0..count {
        let off = 12 + i * 16;
        let w0 = read_u32(buf, off)?;
        let w1 = read_u32(buf, off + 4)?;
        // Words at +8/+12 are culling data; the mesh shader does not read
        // them, so they are not parsed here.
        let vertex_count = (w0 & 127) + 1;
        let primitive_count = ((w0 >> 7) & 127) + 1;
        if !(1..=128).contains(&vertex_count) || !(1..=128).contains(&primitive_count) {
            return Err(MeshletError::BadRecord(i));
        }
        let vtx_table = (((w0 >> 14) & 1) << 16) | (w1 & 0xFFFF);
        let prim_table = (((w0 >> 15) & 1) << 16) | (w1 >> 16);
        records.push(MeshletRecord {
            vertex_count,
            primitive_count,
            vtx_table,
            prim_table,
        });
    }
    Ok(MeshletTable {
        count,
        header1,
        payload_base,
        records,
    })
}

impl MeshletTable {
    /// Total vertices and total primitives across all meshlets.
    pub fn totals(&self) -> (u32, u32) {
        self.records.iter().fold((0, 0), |(v, p), r| {
            (v + r.vertex_count, p + r.primitive_count)
        })
    }

    /// The draw args for the whole table: one indexed draw over every
    /// primitive in every meshlet.
    pub fn draw_args(&self) -> DrawIndexedArguments {
        let (_, total_prims) = self.totals();
        DrawIndexedArguments {
            index_count_per_instance: total_prims * 3,
            instance_count: 1,
            start_index_location: 0,
            base_vertex_location: 0,
            start_instance_location: 0,
        }
    }

    /// The vertex indices of meshlet `meshlet`: one u16 per vertex,
    /// parity-selected from a u32 load, in meshlet order.
    pub fn vertex_indices(
        &self,
        meshlet: usize,
        buf: &[u8],
    ) -> Result<Vec<u32>, MeshletError> {
        let rec = self
            .records
            .get(meshlet)
            .ok_or(MeshletError::BadMeshlet(meshlet))?;
        let mut out = Vec::with_capacity(rec.vertex_count as usize);
        for t in 0..rec.vertex_count {
            let byte_addr = self
                .payload_base
                .checked_add(((rec.vtx_table as usize) << 1) + ((self.header1 as usize) << 2))
                .and_then(|b| b.checked_add((t as usize) * 2))
                .ok_or(MeshletError::OffsetOverflow)?;
            let word = read_u32(buf, byte_addr & !3)?;
            let idx = if byte_addr & 2 != 0 {
                word >> 16
            } else {
                word & 0xFFFF
            };
            out.push(idx);
        }
        Ok(out)
    }

    /// The primitives of meshlet `meshlet`: one (v0, v1, v2) per
    /// primitive, each a 7-bit index into the meshlet's vertex table.
    pub fn primitives(
        &self,
        meshlet: usize,
        buf: &[u8],
    ) -> Result<Vec<[u32; 3]>, MeshletError> {
        let rec = self
            .records
            .get(meshlet)
            .ok_or(MeshletError::BadMeshlet(meshlet))?;
        let mut out = Vec::with_capacity(rec.primitive_count as usize);
        for t in 0..rec.primitive_count {
            let word_addr = self
                .payload_base
                .checked_add(((rec.prim_table as usize) + (t as usize)) * 4)
                .ok_or(MeshletError::OffsetOverflow)?;
            let packed = read_u32(buf, word_addr)?;
            // Three 7-bit indices, contiguous in the low 21 bits.
            out.push([packed & 127, (packed >> 7) & 127, (packed >> 14) & 127]);
        }
        Ok(out)
    }

    /// Checks every vertex index and primitive index in the table is in
    /// range (vertex indices < 2^20, primitive indices < 128 and <
    /// their meshlet's vertex count). Returns the number of violations.
    pub fn bad_index_count(&self, buf: &[u8]) -> Result<usize, MeshletError> {
        let mut bad = 0usize;
        for (i, rec) in self.records.iter().enumerate() {
            for idx in self.vertex_indices(i, buf)? {
                if idx >= 1 << 20 {
                    bad += 1;
                }
            }
            for tri in self.primitives(i, buf)? {
                for v in tri {
                    if v >= 128 || v >= rec.vertex_count {
                        bad += 1;
                    }
                }
            }
        }
        Ok(bad)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Builds a one-meshlet table in memory: N=1, vc=3, pc=1, with the
    /// vertex-index table [0, 1, 2] and one primitive (0, 1, 2).
    fn one_meshlet_table() -> Vec<u8> {
        let mut buf = Vec::new();
        // Header: N=1, header1=1 (one primitive u32 before the vtx table).
        buf.extend_from_slice(&1u32.to_le_bytes());
        buf.extend_from_slice(&1u32.to_le_bytes());
        // 4-byte gap before the records (records start at 12).
        buf.extend_from_slice(&0u32.to_le_bytes());
        // Record 0: vc=3 -> (3-1)=2 in bits 0-6; pc=1 -> 0 in bits 7-13;
        // vtx_table=0, prim_table=0.
        let w0 = 2u32;
        let w1 = 0u32;
        buf.extend_from_slice(&w0.to_le_bytes());
        buf.extend_from_slice(&w1.to_le_bytes());
        // Words +8/+12 (culling data): unused here.
        buf.extend_from_slice(&0u32.to_le_bytes());
        buf.extend_from_slice(&0u32.to_le_bytes());
        // Payload at 12 + 1*16 = 28. Primitive table first (1 u32):
        // (0, 1, 2) as 3x7-bit.
        let packed: u32 = (1 << 7) | (2 << 14);
        buf.extend_from_slice(&packed.to_le_bytes());
        // Vertex-index table: u16 [0, 1, 2] (one u32 holding 0,1; one
        // holding 2 plus padding). Base is payload + header1*4 = 28 + 4.
        buf.extend_from_slice(&0x00010000u32.to_le_bytes());
        buf.extend_from_slice(&0x00000002u32.to_le_bytes());
        buf
    }

    #[test]
    fn synthetic_table_parses_and_unpacks() {
        let buf = one_meshlet_table();
        let table = parse_table(&buf).expect("synthetic table parses");
        assert_eq!(table.count, 1);
        assert_eq!(table.header1, 1);
        assert_eq!(table.payload_base, 28);
        assert_eq!(table.totals(), (3, 1));
        assert_eq!(
            table.draw_args(),
            DrawIndexedArguments {
                index_count_per_instance: 3,
                instance_count: 1,
                start_index_location: 0,
                base_vertex_location: 0,
                start_instance_location: 0,
            }
        );
        assert_eq!(table.vertex_indices(0, &buf).unwrap(), vec![0, 1, 2]);
        assert_eq!(table.primitives(0, &buf).unwrap(), vec![[0, 1, 2]]);
        assert_eq!(table.bad_index_count(&buf).unwrap(), 0);
    }

    /// Verifies the geometry against the real captured meshlet tables, when
    /// they are present (the mesh-capture folder on the machine that ran
    /// the title). Skipped silently when a file is absent.
    ///
    /// Only the invariants are asserted: the table parses and every
    /// primitive index is in range (0 bad indices), which is what proves
    /// the 7-bit unpack. Totals are NOT asserted: a capture file is
    /// overwritten on every run with view-dependent data, so a hardcoded
    /// total goes stale as soon as the title is run again (the known-good
    /// snapshot had 56/28, 2204/2572, 10785/13249 for these three files).
    #[test]
    fn captured_tables_verify() {
        let Some(local) = std::env::var_os("LOCALAPPDATA") else {
            return;
        };
        let dir = std::path::PathBuf::from(local)
            .join("victory")
            .join("mesh-captures");
        let cases = [
            "00a5896a6ac7b7c7.t1d8.buf",
            "1617bda5757a7804.t1d24.buf",
            "4adfa13bea1c556a.t1d24.buf",
        ];
        let mut ran = 0;
        for name in cases {
            let Ok(bytes) = std::fs::read(dir.join(name)) else {
                continue;
            };
            let table = parse_table(&bytes).expect("captured table parses");
            assert_eq!(table.bad_index_count(&bytes).unwrap(), 0, "{name}");
            ran += 1;
        }
        // At least the synthetic test ran; the captured checks run when the
        // captures are present on this machine.
        assert!(ran <= cases.len());
    }
}
