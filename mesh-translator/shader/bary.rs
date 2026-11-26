//! Barycentrics on a host without `SV_Barycentrics` (`shader::HOST_BARYCENTRICS`): the
//! translated pixel shader reads the weights as the ordinary input `BARY_SEMANTIC`
//! (index `i` at register `BARY_ROW - i`), and the vertex side puts out (1, 0, 0),
//! (0, 1, 0) and (0, 0, 1) at a triangle's three corners. Interpolated in the pixel
//! shader's own mode, those are the weights the hardware would give: corner 0 is the
//! provoking vertex, as `SV_Barycentrics` orders them.
//!
//! A pipeline with a vertex shader gets [`bary_gs`] behind it; the reader of a mesh
//! shader's compute form puts the weights out itself (`mesh_compute::reader_vs`).

use super::bitcode::{Attr, Term, V, pred};
use super::container::SigElement;
use super::shader::{BARY_ROW, BARY_SEMANTIC, Geometry, Shader, host_barycentrics};
use crate::ShaderError;
use crate::dxbc::{Container, FourCC};

/// The weight outputs to add to `outputs` (none with host barycentrics): index 0 and 1,
/// each where no output already is (one pixel shader may read both modes).
pub(crate) fn bary_outputs(outputs: &[SigElement]) -> Vec<SigElement> {
    if host_barycentrics() {
        return Vec::new();
    }
    (0..2u32)
        .filter(|&i| !outputs.iter().any(|e| e.row == Some(BARY_ROW - i)))
        .map(|i| SigElement {
            name: BARY_SEMANTIC.into(),
            index: i,
            kind: 0,
            comp: 9,
            interp: 2,
            row: Some(BARY_ROW - i),
            col: 0,
            cols: 3,
            usage: 7,
            half: false,
        })
        .collect()
}

/// Stores the weights of corner `k` (an i32 value, 0 to 2) to the `count` weight outputs
/// from output `first` on.
pub(crate) fn store_weights(s: &mut Shader, b: usize, first: usize, count: usize, k: V) {
    let (one, zero) = (s.m.f32(1.0f32.to_bits()), s.m.f32(0));
    let void = s.t.void;
    for id in first..first + count {
        for col in 0..3u32 {
            let c = s.m.i32(col);
            let here = s.m.cmp(b, pred::EQ, k, c);
            let w = s.m.select(b, here, one, zero);
            let args = [s.m.i32(id as u32), s.m.i32(0), s.m.i8(col as u8), w];
            s.op(b, "storeOutput.f32", Attr::NoUnwind, void, 5, &args);
        }
    }
}

/// Whether the translated pixel shader `ps` reads emulated barycentrics.
pub fn reads_emulated_barycentrics(ps: &[u8]) -> bool {
    sig(ps, FourCC::ISG1).is_ok_and(|inputs| inputs.iter().any(|e| e.name == BARY_SEMANTIC))
}

fn sig(dxil: &[u8], fourcc: FourCC) -> Result<Vec<SigElement>, ShaderError> {
    let c = Container::parse(dxil)?;
    match c.part(fourcc) {
        Some(part) => super::signature(part.data),
        None => Ok(Vec::new()),
    }
}

/// The geometry shader behind the translated vertex shader `vs` that adds the weights for
/// the translated pixel shader `ps` (both as [`super::HostShader::dxil`]): each triangle
/// as it came, every output passed on, register for register, and the weights. A pixel
/// shader that declares a primitive id gets the triangle's number, which the host leaves
/// to the geometry stage when there is one.
pub fn bary_gs(vs: &[u8], ps: &[u8]) -> Result<Vec<u8>, ShaderError> {
    let unsupported = |what| ShaderError::Unsupported { offset: 0, what };
    let mut outputs = sig(vs, FourCC::OSG1)?;
    let ps_inputs = sig(ps, FourCC::ISG1)?;
    if outputs.iter().any(|e| e.row.is_none()) {
        return Err(unsupported("a vertex output without a register, for emulated barycentrics"));
    }
    for e in &mut outputs {
        // Interpolation as `emit_program` declares a vertex shader's outputs; a geometry
        // shader's inputs carry the same modes.
        e.interp = match (e.kind, e.comp) {
            (3, _) => 4,
            (0 | 6 | 7, 9) => 2,
            (0, _) => 1,
            _ => 0,
        };
        e.usage = (1 << e.cols) - 1;
    }
    super::container::one_mode_per_register(&mut outputs, false);
    let mut s = Shader::new(false);
    s.geometry = Some(Geometry { max_vertices: 3 });
    s.inputs = outputs.clone();
    let passed = outputs.len();
    let bary = bary_outputs(&outputs);
    outputs.extend(bary.iter().cloned());
    // The primitive id the pixel shader declares, as one more output.
    let primitive = match ps_inputs.iter().find(|e| e.kind == 10 && e.row.is_some()) {
        Some(e) if !outputs.iter().any(|o| o.kind == 10) => {
            let clash = outputs.iter().any(|o| {
                let overlap = o.col < e.col + e.cols && e.col < o.col + o.cols;
                o.row == e.row && (o.interp != 1 || overlap)
            });
            if clash {
                return Err(unsupported("a primitive id sharing a register, for emulated barycentrics"));
            }
            outputs.push(SigElement {
                interp: 1,
                usage: (1 << e.cols) - 1,
                ..e.clone()
            });
            Some(outputs.len() - 1)
        }
        _ => None,
    };
    let scalars: u32 = outputs.iter().map(|e| u32::from(e.cols)).sum();
    if scalars * 3 > 1024 {
        return Err(unsupported("more output components than a geometry shader emits, for emulated barycentrics"));
    }
    s.outputs = outputs;
    let b = s.entry;
    let number = primitive.map(|id| (id, s.op(b, "primitiveID.i32", Attr::ReadNone, s.t.i32, 108, &[])));
    let stream = s.m.i8(0);
    let void = s.t.void;
    for vertex in 0..3u32 {
        for id in 0..passed {
            let float = s.inputs[id].comp == 9;
            let (load, store, ty) = if float {
                ("loadInput.f32", "storeOutput.f32", s.t.f32)
            } else {
                ("loadInput.i32", "storeOutput.i32", s.t.i32)
            };
            for col in 0..s.inputs[id].cols {
                let args = [s.m.i32(id as u32), s.m.i32(0), s.m.i8(col), s.m.i32(vertex)];
                let x = s.op(b, load, Attr::ReadNone, ty, 4, &args);
                let args = [s.m.i32(id as u32), s.m.i32(0), s.m.i8(col), x];
                s.op(b, store, Attr::NoUnwind, void, 5, &args);
            }
        }
        let k = s.m.i32(vertex);
        store_weights(&mut s, b, passed, bary.len(), k);
        if let Some((id, n)) = number {
            let args = [s.m.i32(id as u32), s.m.i32(0), s.m.i8(0), n];
            s.op(b, "storeOutput.i32", Attr::NoUnwind, void, 5, &args);
        }
        s.op(b, "emitStream", Attr::NoUnwind, void, 97, &[stream]);
    }
    s.m.terminate(b, Term::Ret);
    Ok(s.finish())
}

#[cfg(test)]
mod tests {
    /// Pixel shaders that read barycentrics, translated as on a host without them, with
    /// their vertex shaders and the geometry shader between, through the validator
    /// (`VICTORY_BARY_PAIRS=<dir>;<vs hash>:<ps hash>,...`, the dir holding the runtime's
    /// `VICTORY_SHADER_SAVE` files). Ignored: it needs those and `dxil.dll`.
    #[test]
    #[ignore]
    fn emulated_barycentrics_on_saved_shaders() {
        use super::super::validator::Validator;
        let spec = std::env::var("VICTORY_BARY_PAIRS").expect("VICTORY_BARY_PAIRS");
        let (dir, pairs) = spec.split_once(';').expect("dir;pairs");
        super::super::set_host_barycentrics(false);
        let mut v = Validator::load(Validator::default_path().as_deref()).expect("dxil.dll");
        let read = |hash: &str, stage: &str| std::fs::read(format!("{dir}/{hash}.{stage}.cdxil")).expect("saved container");
        for pair in pairs.split(',') {
            let (vs, ps) = pair.split_once(':').expect("vs:ps");
            let mut p = super::super::translate(&read(ps, "pixel")).expect("pixel translates");
            assert!(super::reads_emulated_barycentrics(&p.dxil), "{ps}: no emulated barycentrics");
            v.validate(&mut p.dxil).unwrap_or_else(|e| panic!("{ps} pixel: {e}"));
            if vs == "-" {
                continue;
            }
            let mut s = super::super::translate(&read(vs, "vertex")).expect("vertex translates");
            v.validate(&mut s.dxil).unwrap_or_else(|e| panic!("{vs} vertex: {e}"));
            let mut gs = super::bary_gs(&s.dxil, &p.dxil).expect("geometry shader");
            v.validate(&mut gs).unwrap_or_else(|e| panic!("{vs} geometry: {e}"));
            eprintln!("{vs} + {ps}: pixel, vertex and geometry shaders valid");
        }
        super::super::set_host_barycentrics(true);
    }
}
