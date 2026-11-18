//! Stage C3: Skate's skater pose retargeted onto V's own rig. A port of `stage0/retarget/retarget.py`, the Stage 0
//! prototype that was checked over 1,747 frames (investigation §6, "Retarget onto V's rigs"):
//! - Bind fit: each mapped bone wears its Skate joint's bind->posed rotation, carried through a fit that swings V's bind
//!   segment onto Skate's bind segment. Feet are fitted about the vertical only; end bones take their parent's fit.
//! - V keeps her own bone lengths (rigid bones). The pelvis is placed so V's hip joints straddle Skate's.
//! - Two-bone IK puts V's ankles and wrists on Skate's, bending in the plane Skate's limb bends in.
//!
//! The rig is V's player skeleton (`player_woman_skeleton.rig` / `player_man_skeleton.rig`, 152 bones), read from
//! WolvenKit's JSON of the user's own game files. It is not `woman_base.rig`, which Stage 0 used: the 24 body joints
//! have the same names, but the spine and shoulders differ by up to 4 cm.
use bevy_math::{Mat3, Mat4, Quat, Vec3};
use std::collections::HashMap;

/// The bones whose orientation the animation graph overrides (`tools/wkprobe/Patch.cs`, same order and names).
pub const BONES: [&str; 24] = [
    "Hips", "Spine", "Spine1", "Spine2", "Spine3", "Neck", "Neck1", "Head",
    "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand", "RightShoulder", "RightArm", "RightForeArm", "RightHand",
    "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase", "RightUpLeg", "RightLeg", "RightFoot", "RightToeBase",
];
/// Bone -> the child that defines its bind segment.
const SEGMENT: [(&str, &str); 19] = [
    ("Hips", "Spine"), ("Spine", "Spine1"), ("Spine1", "Spine2"), ("Spine2", "Spine3"), ("Spine3", "Neck"), ("Neck", "Neck1"),
    ("Neck1", "Head"), ("LeftShoulder", "LeftArm"), ("LeftArm", "LeftForeArm"), ("LeftForeArm", "LeftHand"),
    ("RightShoulder", "RightArm"), ("RightArm", "RightForeArm"), ("RightForeArm", "RightHand"),
    ("LeftUpLeg", "LeftLeg"), ("LeftLeg", "LeftFoot"), ("RightUpLeg", "RightLeg"), ("RightLeg", "RightFoot"),
    ("LeftFoot", "LeftToeBase"), ("RightFoot", "RightToeBase"),
];
/// End bones and the bone whose fit they take.
const TERMINAL: [(&str, &str); 5] = [
    ("Head", "Neck1"), ("LeftHand", "LeftForeArm"), ("RightHand", "RightForeArm"), ("LeftToeBase", "LeftFoot"), ("RightToeBase", "RightFoot"),
];
const GROUND: [&str; 2] = ["LeftFoot", "RightFoot"];
const LIMBS: [(&str, &str, &str); 4] = [
    ("LeftUpLeg", "LeftLeg", "LeftFoot"), ("RightUpLeg", "RightLeg", "RightFoot"),
    ("LeftArm", "LeftForeArm", "LeftHand"), ("RightArm", "RightForeArm", "RightHand"),
];

/// Skate (Y up) -> game (Z up): SK8V's `coords::basis`, as a 4x4.
fn bz() -> Mat4 { Mat4::from_mat3(SkateVRuntime::coords::basis()) }
/// Skate's skinning joints went through Blender: native bone * basis(X, -Z, Y) (SK8V worker.rs).
fn blender() -> Mat4 { Mat4::from_cols(bevy_math::Vec4::X, -bevy_math::Vec4::Z, bevy_math::Vec4::Y, bevy_math::Vec4::W) }

fn unit(v: Vec3) -> Vec3 { let n = v.length(); if n > 1e-9 { v / n } else { v } }

/// The rotation taking direction `a` onto direction `b` (shortest arc).
fn rot_between(a: Vec3, b: Vec3) -> Mat3 {
    let (a, b) = (unit(a), unit(b));
    let (v, c) = (a.cross(b), a.dot(b));
    if c < -0.999999 {
        let axis = unit(a.cross(if a.x.abs() < 0.9 { Vec3::X } else { Vec3::Y }));
        return Mat3::from_cols(axis * axis.x * 2.0, axis * axis.y * 2.0, axis * axis.z * 2.0) - Mat3::IDENTITY;
    }
    let k = Mat3::from_cols(Vec3::new(0.0, v.z, -v.y), Vec3::new(-v.z, 0.0, v.x), Vec3::new(v.y, -v.x, 0.0));
    Mat3::IDENTITY + k + k * k * (1.0 / (1.0 + c))
}

/// Nearest rotation (the polar factor; equals the SVD orthonormalisation for det > 0). Skin matrices are rotations up
/// to rounding and a little scale, so the iteration converges in a few steps.
fn orthonormal(m: Mat3) -> Mat3 {
    let mut r = m;
    for _ in 0..20 {
        let next = (r + r.inverse().transpose()) * 0.5;
        let d = (next - r).to_cols_array().iter().fold(0f32, |a, x| a.max(x.abs()));
        r = next;
        if d < 1e-7 { break; }
    }
    r
}

fn rot(m: &Mat4) -> Mat3 { Mat3::from_mat4(*m) }
fn pos(m: &Mat4) -> Vec3 { m.w_axis.truncate() }
fn compose(r: Mat3, p: Vec3) -> Mat4 { let mut m = Mat4::from_mat3(r); m.w_axis = p.extend(1.0); m }

/// Middle and end joint positions for the chain a-b-c reaching `target`, bending toward `pole`. True: out of reach.
fn two_bone(a: Vec3, b: Vec3, c: Vec3, target: Vec3, pole: Vec3) -> (Vec3, Vec3, bool) {
    let (l1, l2) = ((b - a).length(), (c - b).length());
    let d = target - a;
    let dist = d.length().clamp(1e-6, l1 + l2 - 1e-6);
    let dir = unit(d);
    let cos_a = ((l1 * l1 + dist * dist - l2 * l2) / (2.0 * l1 * dist)).clamp(-1.0, 1.0);
    let side = unit(pole - a - dir * (pole - a).dot(dir));
    let mid = a + dir * (l1 * cos_a) + side * (l1 * (1.0 - cos_a * cos_a).max(0.0).sqrt());
    (mid, a + dir * dist, d.length() > l1 + l2)
}

/// C3b: the board joint's transform in game axes (the session's coordinates): BZ * root * bone * BLENDER, the frame
/// `tools/board/board_glb.py` models the board mesh in. Position, and orientation (i, j, k, r).
pub fn board_transform(root: Mat4, bone: Mat4) -> (Vec3, [f32; 4]) {
    let m = bz() * root * bone * blender();
    let q = Quat::from_mat3(&orthonormal(rot(&m))).normalize();
    (pos(&m), [q.x, q.y, q.z, q.w])
}

/// V's rig: names, parents (parents precede children) and the reference pose in local space.
pub struct Rig {
    pub path: String,
    pub names: Vec<String>,
    pub parents: Vec<i32>,
    local: Vec<Mat4>,
}

impl Rig {
    /// WolvenKit JSON of a `.rig` (`wkprobe json <path>.rig out.json`).
    pub fn from_json(path: &str, text: &str) -> Result<Self, String> {
        let j: serde_json::Value = serde_json::from_str(text).map_err(|e| format!("{path}: {e}"))?;
        let r = &j["Data"]["RootChunk"];
        let names: Vec<String> = r["boneNames"].as_array().ok_or("no boneNames")?.iter()
            .map(|n| n["$value"].as_str().unwrap_or("").to_string()).collect();
        let parents: Vec<i32> = r["boneParentIndexes"].as_array().ok_or("no boneParentIndexes")?.iter()
            .map(|p| p.as_i64().unwrap_or(-1) as i32).collect();
        let f = |v: &serde_json::Value, k: &str| v[k].as_f64().unwrap_or(0.0) as f32;
        let local: Vec<Mat4> = r["boneTransforms"].as_array().ok_or("no boneTransforms")?.iter().map(|t| {
            let (tr, q, s) = (&t["Translation"], &t["Rotation"], &t["Scale"]);
            let q = Quat::from_xyzw(f(q, "i"), f(q, "j"), f(q, "k"), f(q, "r")).normalize();
            Mat4::from_scale_rotation_translation(Vec3::new(f(s, "X"), f(s, "Y"), f(s, "Z")), q, Vec3::new(f(tr, "X"), f(tr, "Y"), f(tr, "Z")))
        }).collect();
        if names.is_empty() || names.len() != parents.len() || names.len() != local.len() {
            return Err(format!("{path}: {} names, {} parents, {} transforms", names.len(), parents.len(), local.len()));
        }
        if parents.iter().enumerate().any(|(i, &p)| p >= i as i32) { return Err(format!("{path}: a parent follows its child")); }
        Ok(Self { path: path.to_string(), names, parents, local })
    }

    /// Model-space reference pose (Z up, V facing the rig's own forward).
    pub fn bind_world(&self) -> Vec<Mat4> {
        let mut w: Vec<Mat4> = Vec::with_capacity(self.local.len());
        for (i, l) in self.local.iter().enumerate() {
            let m = if self.parents[i] < 0 { *l } else { w[self.parents[i] as usize] * *l };
            w.push(m);
        }
        w
    }

    pub fn index(&self, name: &str) -> Option<usize> { self.names.iter().position(|n| n == name) }
}

/// The fitted retarget for one rig and Skate's skater skeleton.
pub struct Retarget {
    pub rig: Rig,
    bind_al: Vec<Mat4>,
    ci: HashMap<&'static str, usize>,
    fit: HashMap<&'static str, Mat3>,
    ibm: HashMap<&'static str, Mat4>,
    hipmid_local: Vec3,
    children: Vec<Vec<usize>>,
    pub bind_height: (f32, f32),  // (V, Skate), head over the lower foot, metres
}

/// One retargeted pose: every rig bone's world matrix (game axes, the session's coordinates, Z up).
pub struct Solved {
    pub world: Vec<Mat4>,
    pub unreachable: [bool; 4],
}

impl Retarget {
    /// `ibm`: Skate's inverse bind matrix (Skate space) for an upper-case joint name, from the skater GLB.
    pub fn new(rig: Rig, ibm: impl Fn(&str) -> Option<Mat4>) -> Result<Self, String> {
        let mut ci = HashMap::new();
        let mut inv = HashMap::new();
        for b in BONES {
            ci.insert(b, rig.index(b).ok_or_else(|| format!("{}: no bone {b}", rig.path))?);
            inv.insert(b, ibm(&b.to_uppercase()).ok_or_else(|| format!("skater GLB: no joint {}", b.to_uppercase()))?);
        }
        let sk_bind: HashMap<&str, Vec3> = BONES.iter().map(|&b| (b, pos(&(bz() * inv[b].inverse())))).collect();
        let sb = |b: &str| sk_bind[b];
        // Turn V's bind about the vertical so its hips' lateral axis matches Skate's (both stand upright, Z up).
        let bind = rig.bind_world();
        let flat = |v: Vec3| Vec3::new(v.x, v.y, 0.0);
        let lat_s = unit(flat(sb("LeftUpLeg") - sb("RightUpLeg")));
        let cpb = |b: &str| pos(&bind[ci[b]]);
        let lat_c = unit(flat(cpb("LeftUpLeg") - cpb("RightUpLeg")));
        let align = Mat4::from_mat3(rot_between(lat_c, lat_s));
        let bind_al: Vec<Mat4> = bind.iter().map(|m| align * *m).collect();
        let cpa = |b: &str| pos(&bind_al[ci[b]]);
        let lowest = |f: &dyn Fn(&str) -> Vec3| f("LeftFoot").z.min(f("RightFoot").z);
        let bind_height = (cpa("Head").z - lowest(&cpa), sb("Head").z - lowest(&sb));
        let mut fit = HashMap::new();
        for (b, seg) in SEGMENT {
            let f = if GROUND.contains(&b) {
                rot_between(flat(cpa(seg) - cpa(b)), flat(sb(seg) - sb(b)))  // about the vertical only
            } else {
                rot_between(cpa(seg) - cpa(b), sb(seg) - sb(b))
            };
            fit.insert(b, f);
        }
        for (b, src) in TERMINAL { let f = fit[src]; fit.insert(b, f); }
        let hips = ci["Hips"];
        let hipmid_local = rot(&bind_al[hips]).inverse() * ((cpa("LeftUpLeg") + cpa("RightUpLeg")) / 2.0 - cpa("Hips"));
        let mut children = vec![Vec::new(); rig.names.len()];
        for (i, &p) in rig.parents.iter().enumerate() { if p >= 0 { children[p as usize].push(i); } }
        Ok(Self { rig, bind_al, ci, fit, ibm: inv, hipmid_local, children, bind_height })
    }

    /// Indices of the mapped Skate joints in a pose's joint list (upper-case names), or None if one is missing.
    pub fn skate_indices(names: &[String]) -> Option<[usize; 24]> {
        let mut out = [0usize; 24];
        for (k, b) in BONES.iter().enumerate() { out[k] = names.iter().position(|n| n.eq_ignore_ascii_case(b))?; }
        Some(out)
    }

    /// Retargets one Skate pose (`root`, `bones` in Skate space, as `Session::pose` gives them).
    pub fn solve(&self, root: Mat4, bones: &[Mat4], idx: &[usize; 24]) -> Option<Solved> {
        let (bz, bl) = (bz(), blender());
        let mut sp: HashMap<&str, Vec3> = HashMap::new();
        let mut r: HashMap<&str, Mat3> = HashMap::new();
        for (k, &b) in BONES.iter().enumerate() {
            let posed = root * *bones.get(idx[k])? * bl;
            sp.insert(b, pos(&(bz * posed)));
            let skin = bz * posed * self.ibm[b] * bz.transpose();
            r.insert(b, orthonormal(rot(&skin)) * self.fit[b] * rot(&self.bind_al[self.ci[b]]));
        }
        // Forward kinematics with V's own bone lengths.
        let n = self.rig.names.len();
        let mut w: Vec<Mat4> = Vec::with_capacity(n);
        let hips = self.ci["Hips"];
        for i in 0..n {
            let p = self.rig.parents[i];
            let m = if i == hips {
                let rh = r["Hips"];
                compose(rh, (sp["LeftUpLeg"] + sp["RightUpLeg"]) / 2.0 - rh * self.hipmid_local)
            } else if p < 0 {
                self.bind_al[i]
            } else {
                let p = p as usize;
                let (pr, pp, pbind) = (rot(&w[p]), pos(&w[p]), rot(&self.bind_al[p]).transpose());
                let mapped = BONES.iter().find(|&&b| self.ci[b] == i).map(|b| r[b]);
                let ri = mapped.unwrap_or_else(|| pr * (pbind * rot(&self.bind_al[i])));
                compose(ri, pp + pr * (pbind * (pos(&self.bind_al[i]) - pos(&self.bind_al[p]))))
            };
            w.push(m);
        }
        // Two-bone IK: ankles and wrists onto Skate's, bending in Skate's limb plane.
        let mut unreachable = [false; 4];
        for (k, (up, mid, end)) in LIMBS.into_iter().enumerate() {
            let (iu, im, ie) = (self.ci[up], self.ci[mid], self.ci[end]);
            let (a, b, c) = (pos(&w[iu]), pos(&w[im]), pos(&w[ie]));
            let (nb, nc, far) = two_bone(a, b, c, sp[end], sp[mid]);
            unreachable[k] = far;
            w[iu] = compose(Mat3::from(rot_between(b - a, nb - a)) * rot(&w[iu]), a);
            w[im] = compose(rot_between(c - b, nc - nb) * rot(&w[im]), nb);
            let shift = nc - c;
            let mut stack = vec![ie];
            while let Some(j) = stack.pop() {
                w[j].w_axis += shift.extend(0.0);
                stack.extend(self.children[j].iter().copied());
            }
        }
        if w.iter().any(|m| !m.is_finite()) { return None; }
        Some(Solved { world: w, unreachable })
    }

    /// The mapped bones' orientations and the Hips position from a set of world matrices, for the plugin.
    pub fn export(&self, world: &[Mat4]) -> ([[f32; 4]; 24], Vec3) {
        let mut q = [[0f32; 4]; 24];
        for (k, b) in BONES.iter().enumerate() {
            let r = Quat::from_mat3(&orthonormal(rot(&world[self.ci[b]]))).normalize();
            q[k] = [r.x, r.y, r.z, r.w];
        }
        (q, pos(&world[self.ci["Hips"]]))
    }

    /// The rig's own reference pose (model space, unaligned), for the plugin's convention test.
    pub fn reference(&self) -> ([[f32; 4]; 24], Vec3) { self.export(&self.rig.bind_world()) }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The Rust port against the Stage 0 prototype on the same recorded ride. Not a regular test: it needs lab data.
    /// DKR_RIG=<rig json> DKR_ASSETS=skate-data/assets DKR_POSE=<stage0 run dir> DKR_OUT=<world.bin>
    /// cargo test --release -- --ignored retarget_recorded --nocapture
    /// DKR_OUT receives every frame's world matrices (frames x bones x 16 f32, column-major) for comparison with
    /// retarget.py's cp_pose.npz.
    #[test]
    #[ignore]
    fn retarget_recorded() {
        let rig_path = std::env::var("DKR_RIG").expect("DKR_RIG");
        let assets = std::path::PathBuf::from(std::env::var("DKR_ASSETS").expect("DKR_ASSETS"));
        let run = std::path::PathBuf::from(std::env::var("DKR_POSE").expect("DKR_POSE"));
        let rig = Rig::from_json(&rig_path, &std::fs::read_to_string(&rig_path).unwrap()).unwrap();
        let mesh = SkateVRuntime::skin::SkinMesh::load(&assets.join("private").join("skater.glb"), 0).unwrap();
        let r = Retarget::new(rig, |n| mesh.inverse_bind_for(n)).unwrap();
        let meta: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(run.join("pose.json")).unwrap()).unwrap();
        let names: Vec<String> = meta["names"].as_array().unwrap().iter().map(|n| n.as_str().unwrap().to_string()).collect();
        let per = meta["matrices_per_frame"].as_u64().unwrap() as usize;
        let raw: Vec<f32> = std::fs::read(run.join("pose.bin")).unwrap().chunks_exact(4).map(|b| f32::from_le_bytes([b[0], b[1], b[2], b[3]])).collect();
        let frames = raw.len() / (per * 16);
        let idx = Retarget::skate_indices(&names).expect("mapped joints");
        let board = names.iter().position(|n| n == "SKATEBOARD_ROOT").expect("board joint");
        let trace: Vec<serde_json::Value> = std::fs::read_to_string(run.join("trace.jsonl")).unwrap().lines().map(|l| serde_json::from_str(l).unwrap()).collect();
        let (mut up_min, mut toe_above, mut root_off) = (1f32, Vec::<f32>::new(), Vec::<f32>::new());
        let toes = [r.rig.index("LeftToeBase").unwrap(), r.rig.index("RightToeBase").unwrap()];
        let mut out = Vec::<u8>::new();
        let (mut unreachable, mut cost) = (0u32, 0f64);
        for f in 0..frames {
            let m = |k: usize| Mat4::from_cols_slice(&raw[(f * per + k) * 16..(f * per + k + 1) * 16]);
            let bones: Vec<Mat4> = (1..per).map(m).collect();
            let t0 = std::time::Instant::now();
            let s = r.solve(m(0), &bones, &idx).expect("solved");
            cost += t0.elapsed().as_secs_f64();
            unreachable += s.unreachable.iter().filter(|&&u| u).count() as u32;
            // C3b: the board under the feet while rolling.
            let (bp, bq) = board_transform(m(0), bones[board]);
            let bq = Quat::from_xyzw(bq[0], bq[1], bq[2], bq[3]);
            if trace.get(f).and_then(|t| t["state"].as_str()).is_some_and(|st| st.starts_with("PhysicsGround")) {
                let up = bq * Vec3::Z;
                up_min = up_min.min(up.z);
                for t in toes { toe_above.push((pos(&s.world[t]) - bp).dot(up)); }
                root_off.push((Vec3::new(m(0).w_axis.x, -m(0).w_axis.z, m(0).w_axis.y) - bp).dot(up));
            }
            for w in &s.world { for x in w.to_cols_array() { out.extend_from_slice(&x.to_le_bytes()); } }
        }
        if let Ok(p) = std::env::var("DKR_OUT") { std::fs::write(p, out).unwrap(); }
        println!("{frames} frames, {} bones, {unreachable} unreachable limb-frames, {:.1} us per solve; bind height V {:.3} Skate {:.3}",
                 r.rig.names.len(), 1e6 * cost / frames as f64, r.bind_height.0, r.bind_height.1);
        let pct = |v: &mut Vec<f32>, p: f32| { v.sort_by(|a, b| a.partial_cmp(b).unwrap()); v[((v.len() - 1) as f32 * p) as usize] };
        println!("board while rolling ({} frames): up axis z >= {up_min:.3}; V's toe joints above the board joint p1 {:.3} p50 {:.3} p99 {:.3} m; skater root above it p50 {:.3} m",
                 root_off.len(), pct(&mut toe_above, 0.01), pct(&mut toe_above, 0.5), pct(&mut toe_above, 0.99), pct(&mut root_off, 0.5));
    }
}
