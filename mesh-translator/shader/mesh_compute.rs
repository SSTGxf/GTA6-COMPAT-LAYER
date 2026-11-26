//! The shaders around a mesh program's compute form
//! ([`super::lower::with_mesh_compute`]) on a host without mesh shaders.
//!
//! The compute form writes each group's vertices (16 bytes per output element) and
//! triangles (three indices among the group's vertices) to raw buffers. The draw of them
//! is instanced: instance `g` is mesh group `g`, vertex `3 t + k` corner `k` of its triangle
//! `t`. The reader vertex shader ([`reader_vs`]) fetches the corner's index and then the
//! vertex, and puts out the same elements the mesh shader would, so the title's pixel
//! shader links to it unchanged. Nothing about the draw depends on the number of groups
//! but its instance count, which an indirect mesh dispatch's argument writer
//! ([`args_writer`]) computes on the GPU.

use super::bitcode::{Attr, Bin, Cast, Term, V};
use super::container::SigElement;
use super::lower::McLayout;
use super::shader::{SLOT_MESH_ARGS, SLOT_MESH_INDICES, SLOT_MESH_VERTS, Shader};

/// A dword of the raw buffer `h` at byte `at`, as an integer.
fn load_dword(s: &mut Shader, b: usize, h: V, at: V) -> V {
    let (i32t, res) = (s.t.i32, s.t.res_ret);
    let (iu, one, align) = (s.m.undef(i32t), s.m.i8(1), s.m.i32(4));
    let r = s.op(b, "rawBufferLoad.f32", Attr::ReadOnly, res, 139, &[h, at, iu, one, align]);
    let f = s.m.extract(b, r, 0);
    s.m.cast(b, Cast::Bitcast, f, i32t)
}

/// The reader for vertex outputs `outputs` (as the mesh shader declares them) of a compute
/// form with `layout`.
pub(crate) fn reader_vs(outputs: &[SigElement], layout: McLayout) -> Result<Vec<u8>, &'static str> {
    if outputs.iter().any(|e| e.row.is_none()) {
        return Err("a mesh output without a register, for the compute form's reader");
    }
    if outputs.iter().any(|e| !matches!(e.comp, 4 | 5 | 9)) {
        return Err("a mesh output neither 32-bit float nor integer, for the compute form's reader");
    }
    let mut s = Shader::new(false);
    let system = |name: &str, kind: u8, row: u32| SigElement {
        name: name.into(),
        index: 0,
        kind,
        comp: 5,
        interp: 0,
        row: Some(row),
        col: 0,
        cols: 1,
        usage: 1,
        half: false,
    };
    s.inputs = vec![system("SV_VertexID", 1, 0), system("SV_InstanceID", 2, 1)];
    s.outputs = outputs
        .iter()
        .map(|e| SigElement {
            usage: (1 << e.cols) - 1,
            half: false,
            ..e.clone()
        })
        .collect();
    let read = s.outputs.len();
    // Without host barycentrics: the corners' weights (`shader::HOST_BARYCENTRICS`).
    let bary = super::bary::bary_outputs(&s.outputs);
    s.outputs.extend(bary.iter().cloned());
    let b = s.entry;
    let (i32t, void) = (s.t.i32, s.t.void);
    let input = |s: &mut Shader, id: u32| {
        let args = [s.m.i32(id), s.m.i32(0), s.m.i8(0), s.m.undef(i32t)];
        s.op(b, "loadInput.i32", Attr::ReadNone, i32t, 4, &args)
    };
    let (corner, group) = (input(&mut s, 0), input(&mut s, 1));
    // The corner's index among the group's vertices.
    let k = s.m.i32(layout.triangles * 3);
    let at = s.m.bin(b, Bin::Mul, group, k);
    let at = s.m.bin(b, Bin::Add, at, corner);
    let four = s.m.i32(4);
    let at = s.m.bin(b, Bin::Mul, at, four);
    // (through unordered access views: the buffers stay in the state the compute form
    // writes them in, and a draw needs no state change, only a UAV barrier)
    let indices = s.root_uav_handle(SLOT_MESH_INDICES);
    s.flags |= super::shader::flags::UAVS_AT_EVERY_STAGE;
    let local = load_dword(&mut s, b, indices, at);
    // The vertex.
    let k = s.m.i32(layout.vertices);
    let vertex = s.m.bin(b, Bin::Mul, group, k);
    let vertex = s.m.bin(b, Bin::Add, vertex, local);
    let k = s.m.i32(layout.stride);
    let base = s.m.bin(b, Bin::Mul, vertex, k);
    let verts = s.root_uav_handle(SLOT_MESH_VERTS);
    if !bary.is_empty() {
        // Corner `vertex % 3` of its triangle.
        let three = s.m.i32(3);
        let k = s.m.bin(b, Bin::URem, corner, three);
        super::bary::store_weights(&mut s, b, read, bary.len(), k);
    }
    for id in 0..read {
        let (cols, float) = (s.outputs[id].cols, s.outputs[id].comp == 9);
        for col in 0..cols {
            let within = s.m.i32(16 * id as u32 + 4 * u32::from(col));
            let at = s.m.bin(b, Bin::Add, base, within);
            let x = load_dword(&mut s, b, verts, at);
            let (name, x) = if float {
                ("storeOutput.f32", s.m.cast(b, Cast::Bitcast, x, s.t.f32))
            } else {
                ("storeOutput.i32", x)
            };
            let args = [s.m.i32(id as u32), s.m.i32(0), s.m.i8(col), x];
            s.op(b, name, Attr::NoUnwind, void, 5, &args);
        }
    }
    s.m.terminate(b, Term::Ret);
    Ok(s.finish())
}

/// The argument writer of an indirect mesh dispatch in the compute form: one thread reads
/// the title's `{x, y, z}` (root SRV `t<SLOT_MESH_ARGS>`) and writes ours (root UAV
/// `u<SLOT_MESH_ARGS>`): at byte 0 the instanced draw `{3 * triangles, n, 0, 0}`, at byte
/// 16 the dispatch of the compute form `{n, 1, 1}`, `n` the title's `x` but at most the
/// groups the scratch buffers hold (0 for an empty `y` or `z`). User data (root constants
/// `b0`): dword 0 that most, dword 1 the triangles per group.
// shortcut: a title dispatch with `y` or `z` over 1 draws only its first row of groups
// (the title's mesh programs read the group number in x alone).
pub fn args_writer() -> Vec<u8> {
    let mut s = Shader::new(false);
    s.threads = Some([1, 1, 1]);
    s.user_data = 2;
    let b = s.entry;
    let (i32t, void) = (s.t.i32, s.t.void);
    let ud = s.user_data_handle();
    let zero = s.m.i32(0);
    let row = s.op(b, "cbufferLoadLegacy.i32", Attr::ReadOnly, s.t.cbuf_ret, 59, &[ud, zero]);
    let (most, triangles) = (s.m.extract(b, row, 0), s.m.extract(b, row, 1));
    let title = s.root_srv_handle(SLOT_MESH_ARGS);
    let xyz: Vec<V> = (0..3).map(|i| { let at = s.m.i32(4 * i); load_dword(&mut s, b, title, at) }).collect();
    let n = s.op(b, "binary.i32", Attr::ReadNone, i32t, 40, &[xyz[0], most]);
    let y_empty = s.m.cmp(b, super::bitcode::pred::EQ, xyz[1], zero);
    let z_empty = s.m.cmp(b, super::bitcode::pred::EQ, xyz[2], zero);
    let empty = s.m.bin(b, Bin::Or, y_empty, z_empty);
    let n = s.m.select(b, empty, zero, n);
    let three = s.m.i32(3);
    let corners = s.m.bin(b, Bin::Mul, triangles, three);
    let ours = s.root_uav_handle(SLOT_MESH_ARGS);
    let (one, all, xyz_mask, align) = (s.m.i32(1), s.m.i8(15), s.m.i8(7), s.m.i32(4));
    let iu = s.m.undef(i32t);
    s.op(b, "rawBufferStore.i32", Attr::NoUnwind, void, 140, &[ours, zero, iu, corners, n, zero, zero, all, align]);
    let sixteen = s.m.i32(16);
    s.op(b, "rawBufferStore.i32", Attr::NoUnwind, void, 140, &[ours, sixteen, iu, n, one, one, iu, xyz_mask, align]);
    s.m.terminate(b, Term::Ret);
    s.finish()
}

#[cfg(test)]
mod tests {
    /// Every mesh program captured on a host without mesh shaders
    /// (`VICTORY_MESH_CAPTURES=<dir>`, the `.mesh.cdxil` files the runtime writes),
    /// translated in the compute form, the compute shader and its reader through the
    /// validator, and the argument writer. Ignored: it needs the captures and `dxil.dll`.
    #[test]
    #[ignore]
    fn captured_mesh_programs_in_the_compute_form() {
        use super::super::validator::Validator;
        let dir = std::env::var_os("VICTORY_MESH_CAPTURES").expect("VICTORY_MESH_CAPTURES");
        // As on the RDNA host: 64-lane waves (the compute forms of wave64 programs ask for them).
        super::super::lower::TEST_HOST_WAVE.set(Some(64));
        // `VICTORY_NO_BARY=1`: as on a host without barycentrics (readers put out weights).
        if std::env::var_os("VICTORY_NO_BARY").is_some() {
            super::super::set_host_barycentrics(false);
        }
        let mut v = Validator::load(Validator::default_path().as_deref()).expect("dxil.dll");
        let mut writer = super::args_writer();
        v.validate(&mut writer).expect("the argument writer validates");
        let (mut ok, mut refused, mut rejected) = (0, 0, 0);
        let mut why: std::collections::BTreeMap<String, u32> = Default::default();
        for entry in std::fs::read_dir(dir).expect("dir") {
            let path = entry.expect("entry").path();
            if !path.to_string_lossy().ends_with(".mesh.cdxil") {
                continue;
            }
            let bytes = std::fs::read(&path).expect("read");
            match super::super::translate_mesh_compute(&bytes) {
                Err(e) => {
                    refused += 1;
                    *why.entry(format!("refused: {e}")).or_default() += 1;
                }
                Ok(mut mc) => {
                    // `VICTORY_MESH_READERS=<dir>`: each reader as `<hash>.reader.dxil`.
                    if let Some(out) = std::env::var_os("VICTORY_MESH_READERS") {
                        let name = path.file_name().unwrap().to_string_lossy().replace(".mesh.cdxil", ".reader.dxil");
                        std::fs::write(std::path::Path::new(&out).join(name), &mc.reader).expect("write");
                    }
                    let a = v.validate(&mut mc.shader.dxil);
                    let b = v.validate(&mut mc.reader);
                    match (a, b) {
                        (Ok(()), Ok(())) => ok += 1,
                        (a, b) => {
                            rejected += 1;
                            let e = a.err().or(b.err()).unwrap_or_default();
                            let first = e.lines().find(|l| l.contains("error")).unwrap_or(&e).to_owned();
                            *why.entry(format!("rejected: {first}")).or_default() += 1;
                        }
                    }
                }
            }
        }
        eprintln!("compute form: {ok} valid, {refused} refused, {rejected} rejected by the validator");
        for (w, n) in &why {
            eprintln!("  {n:4}  {w}");
        }
        assert!(ok > 0);
    }
}
