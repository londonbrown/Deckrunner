//! Deckrunner Stage B runtime: SK8V's unchanged Skate session (pinned mashup engine + SK8V patches 0001-0049)
//! behind a small C ABI, fed with triangles supplied by the host (Cyberpunk's live PhysX collision).
//!
//! Threads (the SK8V pattern): the host calls in from the game's main thread and only posts jobs or reads the last
//! published state. One worker thread owns the Session and steps it at Skate's own fixed period from the game's
//! frame time. A builder thread prepares re-centred collision off the simulation. Every entry point and every
//! thread body is wrapped in catch_unwind; a panic is logged and sets the error status.
//!
//! Coordinates: the host passes game world coordinates (Z up, metres). The runtime re-centres on the first spawn's
//! XY (`origin`) so the Skate world stays near zero, then maps to Skate space with SK8V's `coords::to_skate`.
use bevy_math::Vec3;
use skate_host::bridge::{CollisionBuilder, Controls, InputFrame, PreparedCollision, Session};
use std::collections::HashMap;
use std::ffi::{CStr, c_char, c_void};
use std::io::Write;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::PathBuf;
use std::sync::{Arc, Mutex, mpsc};
use SkateVRuntime::coords;

pub mod retarget;
pub mod hud;  // the plugin's HUD: Skate's scorer and its words (ABI 13), and its session marker (ABI 14)

// Skate 3's gameplay audio, exactly as SK8V runs it (skatev-runtime's `audio`: Skate's AEMS sound scripts on their own
// thread, rendered dry at 48 kHz into one mono ring per emitter: deck, body, head). The host pulls the rings and plays
// them. Emitter positions are published in the session's coordinates; `AUDIO_ORIGIN` turns them back into the game's.
use SkateVRuntime::audio;
pub use SkateVRuntime::{SvAudioEmitter, SvAudioStatus};
static AUDIO_ORIGIN: Mutex<Vec3> = Mutex::new(Vec3::ZERO);

/// Skate 3's d-pad gestures [up, down, left, right] as catalog indices, SK8V's own (`skate-gta` worker `GESTURES`).
/// The engine has none by default, and any d-pad press then stops it: "CharacterGesture needs the actual skater
/// gesture selections" (first in-game ride, 2026-11-16; SK8V hit the same on 10-10).
pub const GESTURES: [u32; 4] = [23, 7, 24, 22];

/// Deckrunner's settings on a freshly loaded session, before its first activate.
/// #2: puts the skater at a start's spawn. Players start on foot with the board in hand (SK8V's lifecycle entry, Skate's
/// own off-board checkpoint return); Y gets on the board. The autopilot (lab tests) starts on the board, rolling.
fn enter(s: &mut Session, spawn_s: [f32; 3], heading: f32, autopilot: bool) -> Result<(), String> {
    let heading = coords::skate_heading(heading);
    if autopilot { s.activate(spawn_s, heading) } else { s.activate_offboard(spawn_s, heading) }.map(|_| ())
}

/// #6: Skate 3's physics_mode names, by index (`dkr_set_difficulty`).
const DIFFICULTIES: [&str; 4] = ["easy", "normal", "hardcore", "motorized"];

fn configure(s: &mut Session) {
    s.set_gestures(Some(GESTURES));
}

pub const ABI_VERSION: u32 = 14;  // 2: renamed from Skaterpunk (spk_* exports became dkr_*); 3: dkr_load_rig and the pose (C3); 4: the board (C3b); 5: the camera (C4); 6: dkr_park and warm restarts (C5); 7: Skate audio; 8: the worker's clock in DkrState (sub_tick, period, steps); 9: dkr_capture (P1-11); 10: dkr_set_bodies (P1-14); 11: dkr_set_difficulty (#6); 12: dkr_preload (#2); 13: dkr_get_hud; 14: the session marker in DkrHud
pub const STATUS_IDLE: u32 = 0;
pub const STATUS_LOADING: u32 = 1;
pub const STATUS_ACTIVE: u32 = 2;
pub const STATUS_ERROR: u32 = 3;
const A: u16 = 0x1000;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct DkrPad {
    pub connected: u32,
    pub buttons: u16,
    pub left_trigger: u8,
    pub right_trigger: u8,
    pub lx: i16,
    pub ly: i16,
    pub rx: i16,
    pub ry: i16,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct DkrState {
    pub size: u32,
    pub status: u32,
    pub tick: u64,
    pub pos: [f32; 3],       // skater root, game world
    pub velocity: [f32; 3],  // game world
    pub speed: f32,          // horizontal m/s
    pub state: [u8; 48],
    pub trick: [u8; 48],
    pub step_ms_avg: f32,
    pub step_ms_max: f32,
    pub ticks_per_s: f32,
    pub triangles: u32,
    pub rails: u32,
    pub collision_builds: u32,
    pub collision_center: [f32; 3],
    pub heading: f32,        // autopilot line heading (deg, game convention: 0 = +Y, counter-clockwise)
    pub grind_ticks: u32,
    pub air_ticks: u32,
    pub error: [u8; 160],
    // ABI 8 (P1-6): the worker's clock, published with every tick and every step handled. `tick + sub_tick` is the
    // simulation time in ticks through step `steps` (every dkr_step the worker has taken off its queue, used or
    // skipped). The host times its interpolation from this, not from when it happened to see a tick.
    pub sub_tick: f32,       // time left in the accumulator, in ticks (usually 0..1)
    pub period: f32,         // Skate's tick period, seconds
    pub _pad: u32,
    pub steps: u64,
}

impl Default for DkrState {
    fn default() -> Self {
        Self { size: std::mem::size_of::<Self>() as u32, status: 0, tick: 0, pos: [0.; 3], velocity: [0.; 3], speed: 0., state: [0; 48],
               trick: [0; 48], step_ms_avg: 0., step_ms_max: 0., ticks_per_s: 0., triangles: 0, rails: 0, collision_builds: 0,
               collision_center: [0.; 3], heading: 0., grind_ticks: 0, air_ticks: 0, error: [0; 160], sub_tick: 0., period: 1. / 60.,
               _pad: 0, steps: 0 }
    }
}

/// P1-14 (ABI 10): one moving host body (a car) for Skate, game world, from the host's physics. Its triangles are in
/// the buffer passed beside it (`first_tri`, `tri_count`; 9 floats each, game world, outward). The dynamics are the
/// body's own: centre of mass and the orientation of its mass frame, velocities, inverse mass and the principal
/// inverse inertia in that frame (0 for a body Skate should treat as immovable).
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct DkrBody {
    pub size: u32,
    pub id: u32,              // stable while the host tracks the body (28 bits used)
    pub kind: u32,            // 0 vehicle, 2 object
    pub first_tri: u32,
    pub tri_count: u32,
    pub com: [f32; 3],
    pub com_rot: [f32; 4],    // (i, j, k, r)
    pub lin_vel: [f32; 3],    // m/s
    pub ang_vel: [f32; 3],    // rad/s
    pub inv_mass: f32,
    pub inv_inertia: [f32; 3],
}

// Mirrored by stageA/probe/src/StageB.hpp (asserted there too).
const _: () = assert!(std::mem::size_of::<DkrBody>() == 88 && std::mem::offset_of!(DkrBody, com) == 20 && std::mem::offset_of!(DkrBody, inv_mass) == 72);

/// Stage C3: Skate's pose retargeted onto V's rig, for the animation graph's override (`retarget::BONES` order).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct DkrPose {
    pub size: u32,
    pub valid: u32,           // 1 when the rest holds a pose
    pub tick: u64,            // the skater tick it belongs to (DkrState.tick)
    pub hips: [f32; 3],       // Hips position: game world (dkr_get_pose) or model space (dkr_reference_pose)
    pub unreachable: u32,     // bit per limb (left leg, right leg, left arm, right arm): Skate's ankle/wrist out of V's reach
    pub rot: [[f32; 4]; 24],  // orientation (i, j, k, r) of each bone: game-world axes, or model space for the reference
    pub root: [f32; 3],       // skater root, game world (DkrState.pos of the same tick)
    pub bones: u32,           // bones in V's rig
    pub board_pos: [f32; 3],  // C3b: the board joint (SKATEBOARD_ROOT), game world; the board mesh is modelled in its frame
    pub board_valid: u32,     // 1 when board_pos and board_rot hold this tick's board
    pub board_rot: [f32; 4],  // (i, j, k, r), game-world axes
    pub cam_pos: [f32; 3],    // C4: Skate's camera, game world
    pub cam_valid: u32,       // 1 when cam_pos/cam_fwd/cam_fov hold this tick's camera
    pub cam_fwd: [f32; 3],    // its look direction (unit, game axes); Skate's camera does not roll (SK8V uses the same)
    pub cam_fov: f32,         // Skate's field of view, degrees
}

// Mirrored by stageA/probe/src/StageB.hpp (asserted there too).
const _: () = assert!(std::mem::size_of::<DkrPose>() == 496 && std::mem::offset_of!(DkrPose, rot) == 32 && std::mem::offset_of!(DkrPose, root) == 416
                      && std::mem::offset_of!(DkrPose, board_pos) == 432 && std::mem::offset_of!(DkrPose, board_rot) == 448
                      && std::mem::offset_of!(DkrPose, cam_pos) == 464 && std::mem::offset_of!(DkrPose, cam_fwd) == 480);

impl Default for DkrPose {
    fn default() -> Self {
        Self { size: std::mem::size_of::<Self>() as u32, valid: 0, tick: 0, hips: [0.; 3], unreachable: 0, rot: [[0., 0., 0., 1.]; 24], root: [0.; 3], bones: 0,
               board_pos: [0.; 3], board_valid: 0, board_rot: [0., 0., 0., 1.], cam_pos: [0.; 3], cam_valid: 0, cam_fwd: [0., 1., 0.], cam_fov: 0. }
    }
}

/// Real input on a connected pad: a stick past a third of its travel, a trigger past a sixth, or any button but Start
/// and Back (the game's pause menu and map, which would also pause Skate through the game clock).
fn pad_active(p: &DkrPad) -> bool {
    const START: u16 = 0x0010;
    const BACK: u16 = 0x0020;
    let stick = |v: i16| (v as i32).abs() > 11000;
    p.connected != 0
        && (p.buttons & !(START | BACK) != 0 || stick(p.lx) || stick(p.ly) || stick(p.rx) || stick(p.ry) || p.left_trigger > 40 || p.right_trigger > 40)
}

fn put(dst: &mut [u8], s: &str) {
    let n = s.len().min(dst.len() - 1);
    dst[..n].copy_from_slice(&s.as_bytes()[..n]);
    dst[n] = 0;
}

#[derive(Clone, Copy)]
struct Tri {
    p: [Vec3; 3],
    physical: bool,
}

/// True when some triangle lies under `at` (XY inside it) between 15 m below and 2 m above. A re-centred collision
/// set without one would drop the skater through the world, which happened when the game loaded a save mid-session
/// and the live scene no longer held the area (in-game Stage B, session 1).
fn supported(tris: &[Tri], at: Vec3) -> bool {
    tris.iter().any(|t| {
        let [a, b, c] = t.p;
        let d = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
        if d.abs() < 1e-6 { return false; }
        let u = ((b.x - at.x) * (c.y - at.y) - (c.x - at.x) * (b.y - at.y)) / d;
        let v = ((c.x - at.x) * (a.y - at.y) - (a.x - at.x) * (c.y - at.y)) / d;
        let w = 1.0 - u - v;
        if u < 0.0 || v < 0.0 || w < 0.0 { return false; }
        let z = u * a.z + v * b.z + w * c.z;
        z >= at.z - 15.0 && z <= at.z + 2.0
    })
}

/// Winding check on host triangles: share of near-horizontal faces whose normal points down. Box shapes give 50%
/// (tops and bottoms); 0.6.0's reversed heightfields pushed it to 90% in Glen.
fn floor_facing(tris: &[Tri]) -> String {
    let (mut flat, mut down) = (0u32, 0u32);
    for t in tris {
        let n = (t.p[1] - t.p[0]).cross(t.p[2] - t.p[0]).normalize_or_zero();
        if n.z.abs() > 0.95 { flat += 1; if n.z < 0.0 { down += 1; } }
    }
    format!("{down} of {flat} near-horizontal faces point down ({:.0}%)", 100.0 * down as f32 / flat.max(1) as f32)
}

/// Writes host triangles (game world) as OBJ in the probe's format, so `stageb_harness` can replay them offline.
/// Lab-local diagnostics only: the file holds Cyberpunk collision and must never be committed or shared.
fn write_obj(path: &std::path::Path, tris: &[Tri], header: &str) -> std::io::Result<()> {
    let mut f = std::io::BufWriter::new(std::fs::File::create(path)?);
    writeln!(f, "# {header}")?;
    let mut n = 0u32;
    for (name, physical) in [("physical", true), ("detail", false)] {
        writeln!(f, "o {name}\n# shapeFlags=0x0 query=00000000,00000000,00000000,00000000 sim={:08x},00000000,00000000,00000000", physical as u32)?;
        for t in tris.iter().filter(|t| t.physical == physical) {
            for v in t.p { writeln!(f, "v {} {} {}", v.x, v.y, v.z)?; }
            writeln!(f, "f {} {} {}", n + 1, n + 2, n + 3)?;
            n += 3;
        }
    }
    f.flush()
}

/// Z of triangle `t` straight above or below (x, y), if (x, y) lies inside its XY projection.
fn z_at(t: &[Vec3; 3], x: f32, y: f32) -> Option<f32> {
    let [a, b, c] = *t;
    let d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    if d.abs() < 1e-9 { return None; }
    let l1 = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / d;
    let l2 = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / d;
    let l3 = 1.0 - l1 - l2;
    (l1.min(l2).min(l3) >= -1e-4).then(|| l1 * a.z + l2 * b.z + l3 * c.z)
}

/// A sloped face counts as a stair ramp over real steps when at least this share of its sample points has a detail
/// surface from `STEP_BELOW` under it to `STEP_ABOVE` over it (a tread sits up to one riser, about 0.2 m, under the
/// ramp laid over the nosings).
const STEP_SHARE: f32 = 0.6;
const STEP_BELOW: f32 = 0.5;
const STEP_ABOVE: f32 = 0.2;
/// Detail faces covering more 1 m cells than this (64 m x 64 m) are floors, not steps, and aren't indexed.
const STEP_MAX_CELLS: i64 = 4096;
/// Only detail faces facing up this much count as steps under a ramp (treads; a detail copy of the ramp itself).
const STEP_UP_Z: f32 = 0.7;

/// Layer blend (investigation §7.2g): physical collision, plus detail geometry, minus sloped physical faces with real
/// detail steps right under them (stair ramps over real steps). Each sloped face (8 to 50 degrees) is sampled at seven
/// points inside it and dropped when `STEP_SHARE` of them have an up-facing detail surface just under them.
///
/// P0-12 (0.19.0): up to 0.18.0 the test was one lowest-to-highest height per 1 m cell over all detail vertices. A
/// detail mesh 23 m tall beside Megabuilding H10's stacked walkways made every walkway ramp count as "on steps"; their
/// top faces went, and a fallen skater sank through the underside left behind. That version stays as `blend_cells`
/// for offline comparison.
fn blend(tris: &[Tri]) -> Vec<[Vec3; 3]> {
    let ok = |t: &Tri| (t.p[1] - t.p[0]).cross(t.p[2] - t.p[0]).length() > 1e-6 && t.p.iter().all(|v| v.is_finite());
    // Treads face up; a slab's own underside 0.2 m under its top must not count as steps (P0-12).
    let up = |t: &Tri| (t.p[1] - t.p[0]).cross(t.p[2] - t.p[0]).normalize_or_zero().z > STEP_UP_Z;
    let detail: Vec<&Tri> = tris.iter().filter(|t| !t.physical && ok(t) && up(t)).collect();
    let mut cells: HashMap<(i32, i32), Vec<u32>> = HashMap::new();
    for (i, t) in detail.iter().enumerate() {
        let (lo, hi) = t.p.iter().fold((Vec3::splat(f32::MAX), Vec3::splat(f32::MIN)), |(a, b), v| (a.min(*v), b.max(*v)));
        let (x0, x1, y0, y1) = (lo.x.floor() as i32, hi.x.floor() as i32, lo.y.floor() as i32, hi.y.floor() as i32);
        if (x1 - x0 + 1) as i64 * (y1 - y0 + 1) as i64 > STEP_MAX_CELLS { continue; }
        for x in x0..=x1 { for y in y0..=y1 { cells.entry((x, y)).or_default().push(i as u32); } }
    }
    let on_steps = |t: &Tri| {
        let n = (t.p[1] - t.p[0]).cross(t.p[2] - t.p[0]).normalize();
        let slope = n.z.abs().acos().to_degrees();
        if !(8.0..50.0).contains(&slope) { return false; }
        let [a, b, c] = t.p;
        let m = (a + b + c) / 3.0;
        // The centroid, halfway to each corner, and halfway to each edge's midpoint: all inside the face, on its plane.
        let samples = [m, (m + a) * 0.5, (m + b) * 0.5, (m + c) * 0.5, m * 0.5 + (a + b) * 0.25, m * 0.5 + (b + c) * 0.25, m * 0.5 + (c + a) * 0.25];
        let hits = samples.iter().filter(|s| {
            cells.get(&(s.x.floor() as i32, s.y.floor() as i32)).is_some_and(|ids| ids.iter().any(|&i| {
                z_at(&detail[i as usize].p, s.x, s.y).is_some_and(|z| z >= s.z - STEP_BELOW && z <= s.z + STEP_ABOVE)
            }))
        }).count();
        hits as f32 >= STEP_SHARE * samples.len() as f32
    };
    tris.iter().filter(|t| ok(t) && (!t.physical || !on_steps(t))).map(|t| t.p).collect()
}

/// 0.18.0's layer blend, kept for offline comparison (`blend_census`): a sloped physical face is dropped when, in any
/// of the 3 x 3 cells around its centroid, the lowest-to-highest height of all detail vertices overlaps the face's own
/// height range widened by 0.5 m.
#[cfg(test)]
fn blend_cells(tris: &[Tri]) -> Vec<[Vec3; 3]> {
    let ok = |t: &Tri| (t.p[1] - t.p[0]).cross(t.p[2] - t.p[0]).length() > 1e-6 && t.p.iter().all(|v| v.is_finite());
    let mut cells: HashMap<(i32, i32), (f32, f32)> = HashMap::new();
    for t in tris.iter().filter(|t| !t.physical && ok(t)) {
        for v in t.p {
            let e = cells.entry((v.x.floor() as i32, v.y.floor() as i32)).or_insert((f32::MAX, f32::MIN));
            e.0 = e.0.min(v.z);
            e.1 = e.1.max(v.z);
        }
    }
    let ramp_over_detail = |t: &Tri| {
        let n = (t.p[1] - t.p[0]).cross(t.p[2] - t.p[0]).normalize();
        let slope = n.z.abs().acos().to_degrees();
        if !(8.0..50.0).contains(&slope) { return false; }
        let c = (t.p[0] + t.p[1] + t.p[2]) / 3.0;
        let (lo, hi) = t.p.iter().fold((f32::MAX, f32::MIN), |(a, b), v| (a.min(v.z), b.max(v.z)));
        (-1..=1).any(|dx| (-1..=1).any(|dy| {
            cells.get(&(c.x.floor() as i32 + dx, c.y.floor() as i32 + dy)).is_some_and(|&(zl, zh)| zh >= lo - 0.5 && zl <= hi + 0.5)
        }))
    };
    tris.iter().filter(|t| ok(t) && (!t.physical || !ramp_over_detail(t))).map(|t| t.p).collect()
}

/// Sorts triangles along a 3D Morton curve of their centroids (0.25 m cells).
///
/// Skate bounds portable collision in runs of 64 triangles *in supplied order* and its BVH rejects whole runs
/// (skate-host `collision_world`). PhysX actor order is spatially random, so each run spanned tens of metres and
/// every contact query walked thousands of distant triangles: 13-14 ms per tick stuck at an H10 curb in-game, about
/// 4 ms offline on the same spot (investigation §7.2m). Any order is equally valid for Skate; this one keeps runs
/// compact. 0.18.0: SK8V's area build sorts the same way; this one serves `prepare_rails_only`.
#[cfg(test)]
fn spatial_order(tris: &mut [[Vec3; 3]]) {
    fn spread(v: u64) -> u64 {
        let mut x = v & 0x1f_ffff;
        x = (x | x << 32) & 0x1f_0000_0000_ffff;
        x = (x | x << 16) & 0x1f_0000_ff00_00ff;
        x = (x | x << 8) & 0x100f_00f0_0f00_f00f;
        x = (x | x << 4) & 0x10c3_0c30_c30c_30c3;
        (x | x << 2) & 0x1249_2492_4924_9249
    }
    let cell = |v: f32| ((v * 4.0).floor() as i64 + (1 << 20)).clamp(0, (1 << 21) - 1) as u64;
    let key = |t: &[Vec3; 3]| { let c = (t[0] + t[1] + t[2]) / 3.0; spread(cell(c.x)) | spread(cell(c.y)) << 1 | spread(cell(c.z)) << 2 };
    tris.sort_by_cached_key(key);
}

/// Re-centring (P1-11), SK8V's `world::needs_recentre_moving` and `lead_centre` at the plugin's 40 m gather radius
/// (SK8V's own constants are for its 128 m areas): a new area is due 32 m from the current centre, or as soon as the
/// skater would be 34 m out (0.85 of the radius) by the time it is built; it is centred ahead of the skater by the
/// distance travelled meanwhile, at most 24 m (0.6 of the radius). Builds are assumed 1.3 times their recent average.
const RECENTRE_M: f32 = 32.0;
const COVER_M: f32 = 34.0;
const MAX_LEAD_M: f32 = 24.0;
const BUILD_LEAD: f32 = 1.3;

/// P1-11 (0.19.0) captures, for bails and moments that feel wrong: the skater's last 5 s at every tick (plus 1 s after
/// a bail, 0.5 s after a request), and the host collision within 15 m of that path. Automatic on the first 10 wipeouts
/// of a runtime, and up to 20 on request (`dkr_capture`, Ctrl+Shift+F9 in the plugin). Lab-local diagnostics like the
/// fall dumps: the files hold Cyberpunk collision and must never be committed or shared.
const CAPTURE_TICKS: usize = 360;
const CAPTURE_AFTER_BAIL: u64 = 60;
const CAPTURE_AFTER_KEY: u64 = 30;
const CAPTURE_RADIUS: f32 = 15.0;
const CAPTURE_BAILS: u32 = 10;
/// 0.24.2 (release): the output folder stays bounded. A player's folder grew by a capture per bail (up to 10 a session,
/// 5-30 MB each) and a trace per session, never removed (630 MB in the lab). At start-up only the newest of each kind
/// are kept, and the log starts afresh past `LOG_ROTATE_BYTES`. Captures on bail are the lab's (set
/// DECKRUNNER_BAIL_CAPTURES); Ctrl+Shift+F12 captures and fall dumps, what a bug report needs, stay on.
const KEEP: [(&str, usize); 3] = [("capture-", 20), ("fall-", 3), ("stageb-trace-", 5)];
const LOG_ROTATE_BYTES: u64 = 8 << 20;

fn prune(dir: &std::path::Path) -> usize {
    let mut removed = 0;
    let Ok(entries) = std::fs::read_dir(dir) else { return 0 };
    let files: Vec<(String, std::time::SystemTime, PathBuf)> = entries.flatten()
        .filter_map(|e| Some((e.file_name().to_string_lossy().into_owned(), e.metadata().ok()?.modified().ok()?, e.path())))
        .collect();
    for (prefix, keep) in KEEP {
        let mut group: Vec<_> = files.iter().filter(|f| f.0.starts_with(prefix)).collect();
        group.sort_by(|a, b| b.1.cmp(&a.1));
        for f in group.into_iter().skip(keep) {
            if std::fs::remove_file(&f.2).is_ok() { removed += 1; }
        }
    }
    let log = dir.join("deckrunner-runtime.log");
    if std::fs::metadata(&log).map(|m| m.len() > LOG_ROTATE_BYTES).unwrap_or(false) {
        let old = dir.join("deckrunner-runtime.old.log");
        let _ = std::fs::remove_file(&old);
        let _ = std::fs::rename(&log, &old);
    }
    removed
}
const CAPTURE_KEYS: u32 = 20;

/// P0-15 (0.20.0): after a wipeout Skate respawns the skater where its body came to rest. In the fourth ride a bail on
/// the cement handrail threw the body over the wall's outer side, 25 m down into H10's closed courtyard; Skate respawned
/// it there, V was teleported down to follow, and the courtyard's kill floor killed her a few seconds later (the user:
/// an inaccessible area, normally a lethal fall). Drops like that lead where V must not go. A respawn this far below
/// where the skater last rolled is moved back there instead, facing the way it was rolling. The spot is kept once it has rolled on the ground for
/// `SAFE_TICKS`; stair flights (4 m here) stay below `RESPAWN_DROP`, so a bail down a flight respawns at its foot.
const RESPAWN_DROP: f32 = 6.0;
const SAFE_TICKS: u32 = 30;

/// P1-13 (0.21.0): Skate's session marker. Its controls are Skate 3's (LB + d-pad down places, LB + d-pad up held
/// returns); Skate does the rest once its input goes through `Session::collect` and `advance`. A return lands up to
/// 1,000 m away, past the installed collision, so the host triangles installed when LB + d-pad down is pressed are
/// kept, and a return outside the collision's cover builds from them while Skate waits (at most `MARKER_WAIT_S`).
/// Skate respawns a wiped-out skater by the same teleport. One beginning within `OWN_TELEPORT_TICKS` of the runtime's own
/// `Session::activate` (a start, a resume, the autopilot turning, the respawn guard) is the runtime's; one beginning
/// within `MARKER_HOLD_TICKS` of LB + d-pad up being held is a marker return; any other is a respawn (P0-15).
/// 0.24.0: up to 0.23.1 the split was by time instead (a teleport within 60 ticks of a wipeout was a respawn, any other a
/// marker return), so a return soon after a bail, or a respawn more than a second after the wipeout, was taken for the
/// other.
/// The release version and the commit this build is from, in its log: DKR_VERSION and DKR_COMMIT at build time
/// (tools/release/ci-build.ps1 sets both; the version comes from the tag). A lab or local build says 0.0.0-dev, so it
/// can't pass for a release. Up to 0.1.3 this was a count of lab builds (0.26.0 last).
const RUNTIME_VERSION: &str = match option_env!("DKR_VERSION") { Some(v) => v, None => "0.0.0-dev" };
const RUNTIME_COMMIT: &str = match option_env!("DKR_COMMIT") { Some(v) => v, None => "local" };
const LB: u16 = 0x0100;
const DPAD_UP: u16 = 0x0001;
const DPAD_DOWN: u16 = 0x0002;
const MARKER_WAIT_S: f32 = 3.0;
const MARKER_HOLD_TICKS: u64 = 60;
/// P1-13 (0.23.1): a resume keeps the ride's origin while its spawn is this close to it. Skate keeps a placed marker in
/// its own space, measured from the origin, and kept it through a park; the ninth test resumed 330 m away with a new
/// origin, and the marker's return then went 335 m off, to a spot nobody had been. Further than this the origin moves
/// (Skate's floats stay fine at 2 km), and a return to a marker placed before that is undone.
const ORIGIN_KEEP_M: f32 = 2000.0;
const OWN_TELEPORT_TICKS: u64 = 10;

/// A collision build's result for the worker: the prepared collision, its centre, triangle and rail counts, whether
/// the area had ground under the skater, and the host triangles it came from.
type Built = (Result<PreparedCollision, String>, Vec3, usize, usize, bool, Arc<Vec<Tri>>);

/// Builds Skate collision from host triangles on its own thread (`prepare`, then the session's builder); the result
/// goes to the worker's channel. `check` is where the ground must be.
#[allow(clippy::too_many_arguments)]
fn start_build(b: CollisionBuilder, send: mpsc::Sender<Built>, log: Arc<Log>, origin: Vec3, tris: Arc<Vec<Tri>>, center: Vec3, check: Vec3,
               cache: Arc<Mutex<SkateVRuntime::reuse::AreaCache>>) {
    let _ = std::thread::Builder::new().name("deckrunner-collision".into()).spawn(move || {
        let r = catch_unwind(AssertUnwindSafe(|| {
            if !supported(&tris, check) { return (Err("no ground under the skater".into()), 0, 0, false); }
            let t0 = std::time::Instant::now();
            let prep = prepare(&tris, origin, &mut cache.lock().unwrap_or_else(|e| e.into_inner()));
            let (nt, nr) = (prep.tris.len(), prep.rails.len());
            log.line(&format!("collision built in {} ms: {}", t0.elapsed().as_millis(), prep.note));
            (b.build_tagged(prep.tris, prep.tags, prep.rails), nt, nr, true)
        }));
        match r {
            Ok((res, nt, nr, ok)) => { let _ = send.send((res, center, nt, nr, ok, tris)); }
            Err(_) => { log.line("collision builder panicked"); let _ = send.send((Err("builder panicked".into()), center, 0, 0, true, tris)); }
        }
    });
}

/// One tick of the skater for a capture: where it was (for the collision cut) and the JSON line written for it.
struct Sample {
    pos: Vec3,
    line: String,
}

/// A capture waiting for its tick: written once the session reaches `write_at`.
struct PendingCapture {
    write_at: u64,
    kind: &'static str,
    tick: u64,
    what: String,
}

/// Writes capture `name` (.obj with the host triangles near the path, .jsonl with the samples) on its own thread.
fn write_capture(out_dir: &std::path::Path, name: String, header: String, tris: Arc<Vec<Tri>>, samples: Vec<Sample>, log: Arc<Log>) {
    let dir = out_dir.to_path_buf();
    let _ = std::thread::Builder::new().name("deckrunner-capture".into()).spawn(move || {
        let done = catch_unwind(AssertUnwindSafe(|| -> std::io::Result<usize> {
            let pts: Vec<Vec3> = samples.iter().step_by(10).chain(samples.last()).map(|s| s.pos).collect();
            // A triangle is kept when its XY bounds come within the radius of a path point (large floors included).
            let near: Vec<Tri> = tris.iter().copied().filter(|t| {
                let (lo, hi) = t.p.iter().fold((Vec3::splat(f32::MAX), Vec3::splat(f32::MIN)), |(a, b), v| (a.min(*v), b.max(*v)));
                pts.iter().any(|q| {
                    let dx = (lo.x - q.x).max(0.0).max(q.x - hi.x);
                    let dy = (lo.y - q.y).max(0.0).max(q.y - hi.y);
                    dx * dx + dy * dy <= CAPTURE_RADIUS * CAPTURE_RADIUS && lo.z <= q.z + CAPTURE_RADIUS && hi.z >= q.z - CAPTURE_RADIUS
                })
            }).collect();
            write_obj(&dir.join(format!("{name}.obj")), &near, &header)?;
            let mut f = std::io::BufWriter::new(std::fs::File::create(dir.join(format!("{name}.jsonl")))?);
            for smp in &samples { writeln!(f, "{}", smp.line)?; }
            f.flush()?;
            Ok(near.len())
        }));
        match done {
            Ok(Ok(n)) => log.line(&format!("capture written: {name}.obj ({n} host triangles within {CAPTURE_RADIUS:.0} m of the path) and {name}.jsonl ({} ticks)", samples.len())),
            Ok(Err(e)) => log.line(&format!("capture {name} failed: {e}")),
            Err(_) => log.line(&format!("capture {name} failed (panic)")),
        }
    });
}

/// Skate collision for one build: Skate-space triangles with a tag each, and rails.
struct Prepared {
    tris: Vec<[[f32; 3]; 3]>,
    tags: Vec<u32>,
    rails: Vec<Vec<[f32; 3]>>,
    /// What SK8V's cleanup changed, for the log.
    note: String,
}

/// Host triangles (game world) -> Skate collision, re-centred on `origin` (P1-11, 0.18.0). Deckrunner's layer blend,
/// then SK8V's own area build (`world::build_area_reusing`): the rails, plus the cleanup SK8V gives GTA's collision
/// before Skate sees it. That removes the end caps pressed together between abutting boxes (grinds bailed at the
/// joints), the interior and buried faces of stacked boxes (a fast board stops dead on them), and fillets ramp toes
/// (tagged so they can't be ground). 0.17.0 and earlier handed Skate the blend with rails only (`prepare_rails_only`).
/// `cache` keeps what the last build found, so a rebuild redoes only what changed.
fn prepare(world: &[Tri], origin: Vec3, cache: &mut SkateVRuntime::reuse::AreaCache) -> Prepared {
    prepare_with(world, origin, cache, 21)
}

/// `prepare` as a given plugin version built it, for comparison: 19 is 0.19.0's collision; 20 adds the handrail fixes
/// (Deckrunner's tall end-cap pass, `tall_caps`, before SK8V's area build, and the rail join, `join_rails`, after it);
/// 21 adds the speed bump fillets (`floating_edge_fillets`).
fn prepare_with(world: &[Tri], origin: Vec3, cache: &mut SkateVRuntime::reuse::AreaCache, level: u32) -> Prepared {
    let fixes = level >= 20;
    let mut local: Vec<[Vec3; 3]> = blend(world).into_iter().map(|t| t.map(|v| v - origin)).collect();
    let mut tall_removed = 0;
    if fixes {
        let caps = tall_caps(&local);
        tall_removed = caps.len();
        let mut drop = vec![false; local.len()];
        for i in caps { drop[i] = true; }
        let mut k = drop.into_iter();
        local.retain(|_| !k.next().unwrap_or(false));
    }
    // P1-11 (0.21.0): speed bumps and other slabs floating just above the ground.
    let (mut undersides, mut floats) = (0, Vec::new());
    if level >= 21 {
        let (drop, f) = floating_slabs(&local);
        (undersides, floats) = (drop.len(), f);
        let mut gone = vec![false; local.len()];
        for i in drop { gone[i] = true; }
        let mut k = gone.into_iter();
        local.retain(|_| !k.next().unwrap_or(false));
    }
    let n_in = local.len();
    let area = SkateVRuntime::world::build_area_reusing(origin, local.iter().map(|t| (t.map(|v| v.to_array()), 0u32)), cache);
    let fillets = area.tags.iter().filter(|t| **t & skate_host::bridge::NO_GRIND_TAG != 0).count();
    let removed = (n_in + fillets).saturating_sub(area.triangles.len());
    let note = format!("{tall_removed} tall end caps removed; SK8V cleanup removed {removed} of {n_in} faces and added {fillets} ramp-toe fillet triangles ({} ramp toes, {} curbs kept){}",
                       area.bevels.bevels, area.bevels.curbs,
                       area.changed.map(|c| format!("; {c} triangles changed since the last build")).unwrap_or_default());
    let (found, rails) = if fixes { let n = area.rails.len(); let r = join_rails(area.rails); (n, r) } else { (area.rails.len(), area.rails) };
    let note = if fixes { format!("{note}; {} rails joined into {}", found, rails.len()) } else { note };
    let (mut tris, mut tags) = (area.triangles, area.tags);
    let note = if level >= 21 {
        let n = floats.len();
        if n > 0 {
            tris.extend(floats.iter().map(|t| t.map(|v| coords::to_skate(v).to_array())));
            tags.extend(std::iter::repeat_n(skate_host::bridge::NO_GRIND_TAG, n));
            sort_spatially(&mut tris, &mut tags);
        }
        format!("{note}; {undersides} floating undersides removed and {n} fillet triangles added under their edges")
    } else { note };
    Prepared { tris, tags, rails, note }
}

/// P1-11 (0.20.0): end caps pressed back to back between abutting boxes, as SK8V's `plies::abutting_caps` finds
/// them, for caps taller than SK8V's 0.75 m limit. GTA builds a kinked handrail or ledge from one box per straight
/// run, and SK8V drops the two caps facing each other inside the solid at each joint: a board meeting one at speed
/// reads a contact facing back along the rail and Skate's grind check bails. GTA's handrail and ledge cross-sections
/// are under 0.75 m; Cyberpunk's are not (the cement handrail by V's apartment is a 1.1 m wall, its caps 0.26 by
/// 1.1 m), so its stair handrails bailed at the bottom of every flight, where the flat stretch's box starts (all
/// three handrail bails of the fourth ride). This is SK8V's test (caps within 3 cm of each other's plane, facing,
/// both boxes running more than 8 cm behind them, the partners covering the cap) with the edge limit raised to
/// `TALL_CAP_EDGE`, the search cells widened to match, and the coverage relaxed to two corners and the centroid, for
/// near-vertical faces only (`TALL_CAP_MAX_NZ`).
const TALL_CAP_EDGE: f32 = 2.0;
const TALL_CAP_CELL: f32 = 1.0;
/// Only near-vertical faces (|normal z| at most this) are caps here: a floor under a box keeps SK8V's strict test, so
/// the relaxed coverage never opens a hole anyone could fall through.
const TALL_CAP_MAX_NZ: f32 = 0.5;
const CAP_GAP: f32 = 0.03;
const CAP_FACING: f32 = -0.7;
const CAP_OVERLAP: f32 = 0.02;
const CAP_DEPTH: f32 = 0.08;

fn tall_caps(tris: &[[Vec3; 3]]) -> Vec<usize> {
    let key = |p: Vec3| ((p.x / TALL_CAP_CELL).floor() as i32, (p.y / TALL_CAP_CELL).floor() as i32, (p.z / TALL_CAP_CELL).floor() as i32);
    let mut small: HashMap<(i32, i32, i32), Vec<u32>> = HashMap::new();
    let mut normals = vec![Vec3::ZERO; tris.len()];
    for (i, t) in tris.iter().enumerate() {
        let longest = (0..3).map(|e| (t[(e + 1) % 3] - t[e]).length()).fold(0.0, f32::max);
        if longest > TALL_CAP_EDGE { continue; }
        normals[i] = (t[1] - t[0]).cross(t[2] - t[0]).normalize_or_zero();
        if normals[i] == Vec3::ZERO || normals[i].z.abs() > TALL_CAP_MAX_NZ { normals[i] = Vec3::ZERO; continue; }
        small.entry(key((t[0] + t[1] + t[2]) / 3.0)).or_default().push(i as u32);
    }
    // Point `c` projected onto `b`'s plane lies inside `b`.
    let inside = |c: Vec3, b: usize| {
        let n = normals[b];
        let [p0, p1, p2] = tris[b];
        let q = c - n * (c - p0).dot(n);
        (0..3).all(|e| {
            let (u, v) = ([p0, p1, p2][e], [p0, p1, p2][(e + 1) % 3]);
            (q - u).dot(n.cross(v - u).normalize_or_zero()) >= -CAP_OVERLAP
        })
    };
    let near_plane = |a: usize, b: usize| tris[a].iter().all(|&p| (p - tris[b][0]).dot(normals[b]).abs() <= CAP_GAP);
    // Edge adjacency over positions welded at 1 mm, for the candidate faces' edges only.
    let weld = |p: Vec3| [(p.x * 1000.0).round() as i64, (p.y * 1000.0).round() as i64, (p.z * 1000.0).round() as i64];
    let mut ids: HashMap<[i64; 3], u32> = HashMap::new();
    let corners: Vec<[u32; 3]> = tris.iter().map(|t| t.map(|p| { let next = ids.len() as u32; *ids.entry(weld(p)).or_insert(next) })).collect();
    let edge = |i: usize, e: usize| { let (a, b) = (corners[i][e] as u64, corners[i][(e + 1) % 3] as u64); a.min(b) << 32 | a.max(b) };
    let mut slot: HashMap<u64, u32> = HashMap::new();
    for &i in small.values().flatten() {
        for e in 0..3 { let n = slot.len() as u32; slot.entry(edge(i as usize, e)).or_insert(n); }
    }
    let mut head = vec![u32::MAX; slot.len()];
    let mut on: Vec<(u32, u32)> = Vec::new();
    for i in 0..tris.len() {
        for e in 0..3 {
            if let Some(&k) = slot.get(&edge(i, e)) { on.push((i as u32, head[k as usize])); head[k as usize] = on.len() as u32 - 1; }
        }
    }
    // How far the faces sharing an edge with candidate `i` reach behind it.
    let mut depth = vec![0.0f32; tris.len()];
    for &i in small.values().flatten() {
        let i = i as usize;
        let mut deepest = 0.0f32;
        for e in 0..3 {
            let mut at = head[slot[&edge(i, e)] as usize];
            while at != u32::MAX {
                let (j, next) = on[at as usize];
                if j as usize != i { for &v in &tris[j as usize] { deepest = deepest.max(-(v - tris[i][0]).dot(normals[i])); } }
                at = next;
            }
        }
        depth[i] = deepest;
    }
    let mut found = vec![false; tris.len()];
    for (&(x, y, z), list) in &small {
        for &i in list {
            let i = i as usize;
            if depth[i] <= CAP_DEPTH { continue; }
            let mut partners = Vec::new();
            for dx in -1..=1 { for dy in -1..=1 { for dz in -1..=1 {
                let Some(others) = small.get(&(x + dx, y + dy, z + dz)) else { continue };
                partners.extend(others.iter().map(|&j| j as usize).filter(|&j| {
                    j != i && normals[i].dot(normals[j]) <= CAP_FACING && near_plane(i, j) && near_plane(j, i) && depth[j] > CAP_DEPTH
                }));
            }}}
            if partners.is_empty() { continue; }
            let t = tris[i];
            let centroid = (t[0] + t[1] + t[2]) / 3.0;
            let samples = t.map(|p| p + (centroid - p).normalize_or_zero() * CAP_OVERLAP);
            // SK8V wants all three corners covered. Here two of them and the centroid: abutting boxes in Cyberpunk
            // differ by a few centimetres at the foot (the cement handrail's flat box is 3 cm wider at its base than
            // the sloped one), and the strict test kept the cap that carries the joint's top edge. What is left
            // open is a sliver along the wall's foot, beside the partner's own cap.
            let covered = |p: &Vec3| partners.iter().any(|&j| inside(*p, j));
            found[i] = covered(&centroid) && samples.iter().filter(|p| covered(p)).count() >= 2;
        }
    }
    (0..tris.len()).filter(|&i| found[i]).collect()
}

/// P1-11 (0.21.0): speed bumps. Cyberpunk's bump is a slab 2-3 cm tall whose underside floats 8-9 mm above the road,
/// so its sloped edges start in the air (fifth test: 12.9 -> 7.9 m/s in one tick, then a wipeout). SK8V fillets a
/// ramp toe only where a riser face runs down to the ground; these edges have none. Offline, a wheel reaching the edge
/// lost 1-3 m/s in one tick before the board rose, fillet or not: it met the underside, a downward face in a gap no
/// wheel fits. So two changes, both for a slab `FLOAT_GAP_MIN` to `FLOAT_GAP_MAX` over the ground:
/// - a downward face with ground that close under its centroid and corners is dropped (nothing fits under it);
/// - an up-facing face's edge that no other face continues (or only a downward one), that close over the ground all
///   along, and not dropping away from its face, gets a fillet from the ground up to it at `FLOAT_SLOPE_DEG`, tagged
///   no-grind like SK8V's.
/// Mats, plates and covers lying on the ground get the same; an edge whose ground rises beyond it gets nothing. Returns
/// the faces to drop and the fillets.
const FLOAT_UP_Z: f32 = 0.7;
const FLOAT_GAP_MIN: f32 = 0.001;
const FLOAT_GAP_MAX: f32 = 0.03;
const FLOAT_SLOPE_DEG: f32 = 10.0;
/// Ground faces wider than this many 1 m cells are kept in one list, checked for every query.
const FLOAT_BIG_CELLS: usize = 1024;

fn floating_slabs(tris: &[[Vec3; 3]]) -> (Vec<usize>, Vec<[Vec3; 3]>) {
    let normals: Vec<Vec3> = tris.iter().map(|t| (t[1] - t[0]).cross(t[2] - t[0]).normalize_or_zero()).collect();
    let weld = |p: Vec3| [(p.x * 1000.0).round() as i64, (p.y * 1000.0).round() as i64, (p.z * 1000.0).round() as i64];
    let mut ids: HashMap<[i64; 3], u32> = HashMap::new();
    let corners: Vec<[u32; 3]> = tris.iter().map(|t| t.map(|p| { let next = ids.len() as u32; *ids.entry(weld(p)).or_insert(next) })).collect();
    let key = |a: u32, b: u32| (a.min(b) as u64) << 32 | a.max(b) as u64;
    let mut on_edge: HashMap<u64, Vec<u32>> = HashMap::new();
    for (i, c) in corners.iter().enumerate() {
        for e in 0..3 { on_edge.entry(key(c[e], c[(e + 1) % 3])).or_default().push(i as u32); }
    }
    // Up-facing faces by 1 m cell (the ground), very large ones apart.
    let mut cells: HashMap<(i32, i32), Vec<u32>> = HashMap::new();
    let mut big: Vec<u32> = Vec::new();
    for (i, t) in tris.iter().enumerate() {
        if normals[i].z <= FLOAT_UP_Z { continue; }
        let (lo, hi) = t.iter().fold((Vec3::splat(f32::MAX), Vec3::splat(f32::MIN)), |(a, b), v| (a.min(*v), b.max(*v)));
        let (x0, x1, y0, y1) = (lo.x.floor() as i32, hi.x.floor() as i32, lo.y.floor() as i32, hi.y.floor() as i32);
        if ((x1 - x0 + 1) as usize) * ((y1 - y0 + 1) as usize) > FLOAT_BIG_CELLS { big.push(i as u32); continue; }
        for x in x0..=x1 { for y in y0..=y1 { cells.entry((x, y)).or_default().push(i as u32); } }
    }
    // The highest up-facing surface under `p` (other than face `skip`), and how far below `p` it lies.
    let gap_under = |p: Vec3, skip: u32| -> Option<f32> {
        let list = cells.get(&(p.x.floor() as i32, p.y.floor() as i32)).map(|v| v.as_slice()).unwrap_or(&[]);
        list.iter().chain(&big).filter(|&&j| j != skip)
            .filter_map(|&j| z_at(&tris[j as usize], p.x, p.y))
            .filter(|&z| z < p.z - FLOAT_GAP_MIN * 0.5)
            .fold(None, |m: Option<f32>, z| Some(m.map_or(z, |m| m.max(z))))
            .map(|z| p.z - z)
    };
    let floating = |g: Option<f32>| g.is_some_and(|g| (FLOAT_GAP_MIN..=FLOAT_GAP_MAX).contains(&g));
    let undersides: Vec<usize> = (0..tris.len()).filter(|&i| {
        if normals[i].z >= -FLOAT_UP_Z { return false; }
        let t = tris[i];
        let c = (t[0] + t[1] + t[2]) / 3.0;
        [c, t[0].lerp(c, 0.1), t[1].lerp(c, 0.1), t[2].lerp(c, 0.1)].iter().all(|&q| floating(gap_under(q, i as u32)))
    }).collect();
    let tan = FLOAT_SLOPE_DEG.to_radians().tan();
    let mut done: std::collections::HashSet<u64> = std::collections::HashSet::new();
    let mut out = Vec::new();
    for (i, t) in tris.iter().enumerate() {
        if normals[i].z <= FLOAT_UP_Z { continue; }
        for e in 0..3 {
            let k = key(corners[i][e], corners[i][(e + 1) % 3]);
            if !done.insert(k) { continue; }
            if on_edge[&k].iter().any(|&j| j as usize != i && normals[j as usize].z > -0.3) { continue; }
            let (a, b, c) = (t[e], t[(e + 1) % 3], t[(e + 2) % 3]);
            let mid = (a + b) * 0.5;
            if (b - a).length() < 0.05 || c.z < mid.z - 0.005 { continue; }
            let gaps = [a.lerp(b, 0.05), mid, a.lerp(b, 0.95)].map(|q| gap_under(q, i as u32));
            if !gaps.iter().all(|&g| floating(g)) { continue; }
            let (ga, gb) = (gap_under(a, i as u32).unwrap_or(gaps[0].unwrap()), gap_under(b, i as u32).unwrap_or(gaps[2].unwrap()));
            let mut dir = Vec3::new(b.y - a.y, a.x - b.x, 0.0).normalize_or_zero();
            if dir.dot(c - mid) > 0.0 { dir = -dir; }
            let run = (ga.max(gb) / tan).clamp(0.02, 0.25);
            // The ground must also lie that close under the fillet's foot: an edge over ground that rises beyond it
            // (a handrail's flat box standing 7 mm over the end of its sloped one) is not a slab on the ground.
            if ![a + dir * run, b + dir * run].iter().all(|&q| gap_under(q, i as u32).is_some_and(|g| g >= FLOAT_GAP_MIN * 0.5 && g <= FLOAT_GAP_MAX + 0.01)) { continue; }
            let (a2, b2) = (a + dir * run - Vec3::Z * ga, b + dir * run - Vec3::Z * gb);
            for f in [[a, a2, b2], [a, b2, b]] {
                let n = (f[1] - f[0]).cross(f[2] - f[0]);
                out.push(if n.z < 0.0 { [f[0], f[2], f[1]] } else { f });
            }
        }
    }
    (undersides, out)
}

/// Skate-space triangles and their tags in Morton order of their centroids (0.25 m cells), as SK8V's area build
/// leaves them: Skate bounds its collision in runs of 64 triangles in the order given (§7.2m).
fn sort_spatially(tris: &mut Vec<[[f32; 3]; 3]>, tags: &mut Vec<u32>) {
    fn spread(v: u64) -> u64 {
        let mut x = v & 0x1f_ffff;
        x = (x | x << 32) & 0x1f_0000_0000_ffff;
        x = (x | x << 16) & 0x1f_0000_ff00_00ff;
        x = (x | x << 8) & 0x100f_00f0_0f00_f00f;
        x = (x | x << 4) & 0x10c3_0c30_c30c_30c3;
        (x | x << 2) & 0x1249_2492_4924_9249
    }
    let cell = |v: f32| ((v * 4.0).floor() as i64 + (1 << 20)).clamp(0, (1 << 21) - 1) as u64;
    let keys: Vec<u64> = tris.iter().map(|t| {
        let c = [0, 1, 2].map(|k| (t[0][k] + t[1][k] + t[2][k]) / 3.0);
        spread(cell(c[0])) | spread(cell(c[1])) << 1 | spread(cell(c[2])) << 2
    }).collect();
    let mut order: Vec<usize> = (0..tris.len()).collect();
    order.sort_by_key(|&i| keys[i]);
    *tris = order.iter().map(|&i| tris[i]).collect();
    *tags = order.iter().map(|&i| tags[i]).collect();
}

/// P1-11 (0.20.0): rail ends that meet joined into one polyline. SK8V chains two runs only where exactly two meet at
/// a joint, and only through a turn under about 35 degrees. A cement ledge's outer top edge at the bottom of a stair
/// flight has a third run ending there, so its sloped and flat stretches stayed apart and a grind on that edge ended
/// at the kink. Here ends within `JOIN_GAP` of each other join through a turn of up to SK8V's 35 degrees, or up to
/// `JOIN_KINK_DEG` where the turn is a kink in the vertical plane (the plan heading changes by under
/// `JOIN_PLAN_DEG`: a flight steeper than 35 degrees meeting the flat). Straightest pairings first; each end joins
/// once, and never into a loop.
const JOIN_GAP: f32 = 0.03;
const JOIN_TURN_DEG: f32 = 35.0;
const JOIN_KINK_DEG: f32 = 50.0;
const JOIN_PLAN_DEG: f32 = 10.0;

fn join_rails(rails: Vec<Vec<[f32; 3]>>) -> Vec<Vec<[f32; 3]>> {
    let w: Vec<Vec<Vec3>> = rails.into_iter().map(|r| r.into_iter().map(|p| coords::from_skate(Vec3::from_array(p))).collect()).collect();
    // End e of rail i is 2i (first point) or 2i+1 (last point); `out` points away from the rail.
    let end = |e: usize| { let r = &w[e / 2]; let n = r.len(); if e % 2 == 0 { (r[0], (r[0] - r[1]).normalize_or_zero()) } else { (r[n - 1], (r[n - 1] - r[n - 2]).normalize_or_zero()) } };
    let cell = |p: Vec3| ((p.x / 0.25).floor() as i32, (p.y / 0.25).floor() as i32, (p.z / 0.25).floor() as i32);
    let mut cells: HashMap<(i32, i32, i32), Vec<usize>> = HashMap::new();
    for e in 0..w.len() * 2 { if w[e / 2].len() >= 2 { cells.entry(cell(end(e).0)).or_default().push(e); } }
    let (turn_cos, kink_cos, plan_cos) = (JOIN_TURN_DEG.to_radians().cos(), JOIN_KINK_DEG.to_radians().cos(), JOIN_PLAN_DEG.to_radians().cos());
    let mut pairs: Vec<(f32, usize, usize)> = Vec::new();
    for e in 0..w.len() * 2 {
        if w[e / 2].len() < 2 { continue; }
        let (p, out) = end(e);
        let c = cell(p);
        for dx in -1..=1 { for dy in -1..=1 { for dz in -1..=1 {
            for &f in cells.get(&(c.0 + dx, c.1 + dy, c.2 + dz)).map(|v| v.as_slice()).unwrap_or(&[]) {
                if f / 2 <= e / 2 { continue; }
                let (q, out2) = end(f);
                if p.distance(q) > JOIN_GAP { continue; }
                // Leaving through end e, then along rail f/2 away from end f.
                let cos = out.dot(-out2);
                let (h1, h2) = (Vec3::new(out.x, out.y, 0.0).normalize_or_zero(), Vec3::new(-out2.x, -out2.y, 0.0).normalize_or_zero());
                if cos >= turn_cos || (cos >= kink_cos && h1.dot(h2) >= plan_cos) { pairs.push((cos, e, f)); }
            }
        }}}
    }
    pairs.sort_by(|a, b| b.0.total_cmp(&a.0));
    // Accepted links per end, and a union-find over rails so no chain closes on itself.
    let mut link = vec![usize::MAX; w.len() * 2];
    let mut parent: Vec<usize> = (0..w.len()).collect();
    fn find(parent: &mut [usize], mut i: usize) -> usize { while parent[i] != i { parent[i] = parent[parent[i]]; i = parent[i]; } i }
    for &(_, e, f) in &pairs {
        if link[e] != usize::MAX || link[f] != usize::MAX { continue; }
        let (ra, rb) = (find(&mut parent, e / 2), find(&mut parent, f / 2));
        if ra == rb { continue; }
        parent[ra] = rb;
        link[e] = f;
        link[f] = e;
    }
    // Walk each chain from a rail whose first or last end is free.
    let mut used = vec![false; w.len()];
    let mut out = Vec::new();
    for start in 0..w.len() {
        if used[start] || w[start].len() < 2 { continue; }
        let free = if link[2 * start] == usize::MAX { 2 * start } else if link[2 * start + 1] == usize::MAX { 2 * start + 1 } else { continue };
        let mut points: Vec<Vec3> = Vec::new();
        let (mut rail, mut entered) = (start, free);
        loop {
            used[rail] = true;
            let mut seg = w[rail].clone();
            if entered % 2 == 1 { seg.reverse(); }
            if let Some(last) = points.last_mut() { *last = (*last + seg[0]) * 0.5; points.extend(seg.into_iter().skip(1)); } else { points = seg; }
            let leave = entered ^ 1;
            let next = link[leave];
            if next == usize::MAX { break; }
            (rail, entered) = (next / 2, next);
        }
        out.push(points);
    }
    out.into_iter().map(|r| r.into_iter().map(|p| coords::to_skate(p).to_array()).collect()).collect()
}

/// 0.17.0's collision, kept for offline comparison: the blend with SK8V's rail finder only, no cleanup.
#[cfg(test)]
fn prepare_rails_only(world: &[Tri], origin: Vec3, sort: bool) -> (Vec<[[f32; 3]; 3]>, Vec<Vec<[f32; 3]>>) {
    let mut local: Vec<[Vec3; 3]> = blend(world).into_iter().map(|t| t.map(|v| v - origin)).collect();
    if sort { spatial_order(&mut local); }
    let (rails_z, _) = SkateVRuntime::rails::find_metres(&local);
    let to_s = |p: Vec3| coords::to_skate(p).to_array();
    (local.iter().map(|t| t.map(to_s)).collect(), rails_z.iter().map(|r| r.iter().map(|p| to_s(*p)).collect()).collect())
}

enum Job {
    /// `preload` (#2): load Skate and stay parked, so the first start is a resume.
    Start { tris: Vec<Tri>, spawn: Vec3, heading: f32, autopilot: bool, preload: bool },
    Step { dt: f32, pad: DkrPad, posted: std::time::Instant },
    Collision { tris: Vec<Tri>, center: Vec3 },
    Rig { path: String },
    Park,
    Capture,
    Bodies { bodies: Vec<DkrBody>, tris: Vec<[Vec3; 3]> },
    /// #6: Skate 3's physics_mode (0 easy, 1 normal, 2 hardcore, 3 motorized), for the session and the ones after it.
    Difficulty(u32),
}

/// P1-14: the host's moving bodies as Skate takes them: triangles tagged per body (`SkateVRuntime::dynamic::DYNAMIC_TAG`,
/// the kind in bits 28-30 as SK8V packs it) and contact-exchange proxies, in the session's Skate space.
fn skate_bodies(bodies: &[DkrBody], tris: &[[Vec3; 3]], origin: Vec3) -> (Vec<([[f32; 3]; 3], u32, u32)>, Vec<skate_host::bridge::HostBody>) {
    let to_s = |p: Vec3| coords::to_skate(p - origin).to_array();
    let (mut surfaces, mut proxies) = (Vec::new(), Vec::new());
    for b in bodies {
        let tag = SkateVRuntime::dynamic::DYNAMIC_TAG | (b.kind & 0x7) << 28 | (b.id & 0x0fff_ffff);
        let range = (b.first_tri as usize)..(b.first_tri as usize + b.tri_count as usize).min(tris.len());
        surfaces.extend(tris.get(range).unwrap_or(&[]).iter().map(|t| (t.map(to_s), tag, 0u32)));
        let q = bevy_math::Quat::from_xyzw(b.com_rot[0], b.com_rot[1], b.com_rot[2], b.com_rot[3]).normalize();
        let com = Vec3::from_array(b.com);
        if !q.is_finite() || !com.is_finite() { continue; }
        let basis = bevy_math::Mat3::from_cols(coords::to_skate(q * Vec3::X), coords::to_skate(q * Vec3::Y), coords::to_skate(q * Vec3::Z));
        let finite = |v: [f32; 3]| if v.iter().all(|x| x.is_finite()) { v } else { [0.0; 3] };
        proxies.push(skate_host::bridge::HostBody {
            tag,
            position: to_s(com),
            orientation: bevy_math::Quat::from_mat3(&basis).normalize().to_array(),
            linear_velocity: coords::to_skate(Vec3::from_array(finite(b.lin_vel))).to_array(),
            angular_velocity: coords::to_skate(Vec3::from_array(finite(b.ang_vel))).to_array(),
            inverse_mass: if b.inv_mass.is_finite() { b.inv_mass.max(0.0) } else { 0.0 },
            inverse_inertia: finite(b.inv_inertia),
            collision_group: if b.kind == 0 { skate_host::bridge::VEHICLE_GROUP } else { 0 },
            volumes: Vec::new(),
        });
    }
    (surfaces, proxies)
}

struct Shared {
    state: DkrState,
    wants_collision: Option<Vec3>,
    pose: DkrPose,
    reference: DkrPose,
    hud: hud::DkrHud,
}

struct Runtime {
    send: mpsc::Sender<Job>,
    shared: Arc<Mutex<Shared>>,
    log: Arc<Log>,
}

struct Log(Mutex<Option<std::fs::File>>);
impl Log {
    fn line(&self, s: &str) {
        if let Ok(mut f) = self.0.lock() {
            if let Some(f) = f.as_mut() {
                let _ = writeln!(f, "[{:.3}] {s}", std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs_f64()).unwrap_or(0.));
                let _ = f.flush();
            }
        }
    }
}

/// The autopilot's flip flick: sideways component of the up-flick. Offline on a flat road (autopilot_flips): +1 (up and
/// right) is Skate's ID_TRICK_FLIP_KICKFLIP, the board rolling 171 degrees; -1 is a heelflip (165); straight up an ollie (41).
const FLIP_X: f32 = 1.0;

/// Unattended driver for lab runs: rides a straight line from the spawn, pushes, ollies, and turns when blocked.
struct Autopilot {
    heading: f32,
    line_from: Vec3,
    phase_tick: u64,
    ollie_at: u64,
    check_at: u64,
    check_pos: Vec3,
    turns: u32,
    /// Set when blocked: the worker turns the stopped board in place with Skate's own teleport (`Session::activate`).
    turn_to: Option<f32>,
    /// Pops started so far; every other one is a flip trick (C3b run 2: "make it do a kickflip so we can see the board
    /// spin"). `flip_x`: the flick's sideways direction, +1 or -1 (DKR_FLIP_X overrides it for offline checks).
    pops: u32,
    flip_x: f32,
}

impl Autopilot {
    fn new(heading: f32, at: Vec3) -> Self {
        let flip_x = std::env::var("DKR_FLIP_X").ok().and_then(|v| v.parse().ok()).unwrap_or(FLIP_X);
        Self { heading, line_from: at, phase_tick: 0, ollie_at: 0, check_at: 0, check_pos: at, turns: 0, turn_to: None, pops: 0, flip_x }
    }
    fn controls(&mut self, tick: u64, at: Vec3, v: Vec3, state: &str) -> Controls {
        let mut c = Controls::default();
        let speed = Vec3::new(v.x, v.y, 0.0).length();
        let ground = state == "PhysicsGround";
        if self.phase_tick == 0 { self.phase_tick = tick; self.check_at = tick; }
        let t = tick - self.phase_tick;
        // Push whenever below cruising speed. Run 4 pushed only below 3 m/s after the first 3 s and stalled on a 14%
        // climb: Skate takes about one push a second, worth about 0.8 m/s from a crawl.
        if ground && t % 30 < 4 && speed < 4.5 { c.buttons |= A; }
        // Ollie every 1.5 s, only at speed and not while climbing: each ollie costs speed (run 4: 4.6 -> 1.7 m/s
        // over two ollies on the climb).
        let climbing = v.z > 0.05 * speed;
        if t >= 180 && ground && speed > 3.5 && !climbing && tick >= self.ollie_at + 90 { self.ollie_at = tick; self.pops += 1; }
        if self.ollie_at >= self.phase_tick && t >= 180 {
            // Flick-It: right stick pulled down, then flicked up for an ollie, or up and to one side for a flip.
            let d = tick - self.ollie_at;
            let flip = self.pops % 2 == 0;
            let diag = (32767.0 * std::f32::consts::FRAC_1_SQRT_2) as i16;
            if d < 8 { c.right = [0, -32767]; } else if d < 14 { c.right = if flip { [(self.flip_x * diag as f32) as i16, diag] } else { [0, 32767] }; }
        }
        let dir = Vec3::new(-self.heading.to_radians().sin(), self.heading.to_radians().cos(), 0.0);
        let right = Vec3::new(dir.y, -dir.x, 0.0);
        if ground && speed > 0.5 {
            let e = (at - self.line_from).dot(right);
            let cmd = (-(0.6 * e + 0.9 * v.dot(right))).clamp(-1.0, 1.0);
            c.left = [(cmd * 32767.0) as i16, 0];
        }
        // Blocked: less than 1.5 m of progress in the last 2 s. A stopped board cannot steer (the line-following above
        // needs 0.5 m/s), so the board is turned in place: back the way it came first (that way is open), then to
        // either side. Turns alternate +180 and +90, which visits back, one side, the other side, then ahead again.
        // Run 3 and 4's version (+90, -180, +90) only changed the target line and never faced back down a hill.
        // Only with the board on the ground: in a wipeout the root can sit below the floor (offline: z -0.6 under a
        // road at 0), and a teleport there drops the skater through the world. Skate respawns a wipeout by itself.
        if tick >= self.check_at + 120 {
            let moved = Vec3::new(at.x - self.check_pos.x, at.y - self.check_pos.y, 0.0).length();
            if moved < 1.5 && t > 120 && ground {
                self.turns += 1;
                let turn = if self.turns % 2 == 1 { 180.0 } else { 90.0 };
                self.heading = (self.heading + turn).rem_euclid(360.0);
                self.turn_to = Some(self.heading);
                self.line_from = at;
                self.phase_tick = tick;
            }
            self.check_at = tick;
            self.check_pos = at;
        }
        c
    }
}

fn worker(root: PathBuf, out_dir: PathBuf, receive: mpsc::Receiver<Job>, shared: Arc<Mutex<Shared>>, log: Arc<Log>) {
    let set_error = |shared: &Mutex<Shared>, msg: &str| {
        if let Ok(mut s) = shared.lock() { s.state.status = STATUS_ERROR; put(&mut s.state.error, msg); }
    };
    let mut session: Option<Session> = None;
    let mut active_since = std::time::Instant::now();
    let mut requested = false;  // a collision request is outstanding until its build is installed
    // P1-11: how long a re-centre takes from request to install (moving average), and where the skater last was. SK8V's
    // cleanup made builds about 0.4 s instead of 0.1 s, so the next area is asked for early enough, and ahead of the
    // skater, that a fast skater can't run off the current one first (SK8V's `world::needs_recentre_moving`).
    let (mut build_secs, mut requested_at, mut skater_at) = (0.6f32, std::time::Instant::now(), Vec3::ZERO);
    let mut origin = Vec3::ZERO;
    let mut autopilot: Option<Autopilot> = None;
    let mut parked = false;  // C5: the session stays loaded between rides; steps are ignored while parked
    let mut difficulty = 0u32;  // #6: Skate's physics_mode; Easy until the plugin sends the player's setting
    let mut audio_state = audio::map::State::default();  // Skate audio: the sound handlers' game variables
    let mut retarget: Option<retarget::Retarget> = None;  // C3: Skate's pose onto V's rig, once the plugin names the rig
    let mut skate_idx: Option<[usize; 24]> = None;
    let mut board_idx: Option<usize> = None;  // C3b: SKATEBOARD_ROOT in the skater pose's joint list
    let (mut posed, mut pose_failed) = (0u64, 0u64);
    let mut acc = 0f32;
    let mut steps_handled = 0u64;  // ABI 8: Step jobs taken off the queue (DkrState.steps)
    let mut current: Arc<Vec<Tri>> = Arc::new(Vec::new());  // host triangles of the installed collision
    let mut retry_after: Option<std::time::Instant> = None;  // back-off after a rejected re-centre
    let mut dropped = 0u32;  // stale steps skipped (the worker fell behind the game)
    let (mut falling, mut fall_dumps, mut last_ground) = (false, 0u32, Vec3::ZERO);
    // P0-15: the last spot the skater rolled on the ground (and its heading), and whether Skate is respawning it.
    let (mut safe, mut rolling_ticks) = (None::<(Vec3, f32)>, 0u32);
    // P1-13: Skate's teleports (respawns and marker returns), the host triangles kept at the last marker press, and
    // Skate held still while a marker return's collision is built. `packet` numbers the pad samples for `collect`.
    let (mut teleporting, mut marker_return, mut marker_held) = (false, false, None::<u64>);
    // The tick of the runtime's own last `Session::activate`, and whether the teleport under way is that one.
    let (mut own_activate, mut own_teleport) = (None::<u64>, false);
    // P1-16 (0.24.1): the kept host triangles with the centre they were gathered around. 0.24.0 kept the triangles only,
    // and a return built from them centred the collision on the marker: in the twenty-fourth test the set had been
    // gathered 22 m from the marker, re-centring measured from the marker, and the skater rode off the set's far edge
    // 42 m from where it was gathered (while re-centring thought 21 m) and fell through the world.
    let mut marker_host: Option<(Arc<Vec<Tri>>, Vec3)> = None;
    // P1-13 (0.23.1): Skate's marker was placed before the origin moved; and where the skater was before a teleport.
    let mut marker_stale = false;
    let mut before_teleport: Option<(Vec3, f32)> = None;
    let mut marker_wait: Option<std::time::Instant> = None;
    let (mut packet, mut prev_buttons) = (0u32, 0u16);
    // P1-14: the host's latest moving bodies, waiting for the next step; how many Skate has; car hits logged per body.
    let mut bodies_due: Option<(Vec<DkrBody>, Vec<[Vec3; 3]>)> = None;
    let mut bodies_now = 0usize;
    let mut hits: Vec<(u32, std::time::Instant)> = Vec::new();
    let mut dumped_build: Option<u32> = None;  // collision build already written by a fall dump
    let (built_send, built) = mpsc::channel::<Built>();
    let mut builder: Option<CollisionBuilder> = None;
    // P1-11: what the last collision build found, so a rebuild redoes only what changed (SK8V's `reuse`).
    let area_cache = Arc::new(Mutex::new(SkateVRuntime::reuse::AreaCache::default()));
    let mut building = false;
    // One trace per session (run 3 lost its first session's trace when the second overwrote it).
    let stamp = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    let bail_capture_on = std::env::var_os("DECKRUNNER_BAIL_CAPTURES").is_some();
    let mut trace = std::fs::File::create(out_dir.join(format!("stageb-trace-{stamp}.jsonl"))).ok().map(std::io::BufWriter::new);
    let (mut cost_sum, mut cost_max, mut cost_n) = (0f64, 0f64, 0u32);
    let mut window = std::time::Instant::now();
    let mut window_ticks = 0u32;
    let mut last_trick = String::new();
    // P1-11 (0.19.0): the last ticks for captures, a capture waiting for its tick, and how many have been taken.
    let mut recent: std::collections::VecDeque<Sample> = std::collections::VecDeque::with_capacity(CAPTURE_TICKS + 1);
    let mut capture_due: Option<PendingCapture> = None;
    let (mut bail_captures, mut key_captures, mut captures, mut was_down, mut last_tick) = (0u32, 0u32, 0u32, false, 0u64);
    // The HUD: Skate's words from the player's own data, loaded once, and the scorer's events counted per ride.
    let names = match hud::Names::load(&root) {
        Ok(n) => { log.line(&format!("hud: {} of Skate's strings from {}", n.len(), hud::Names::FILE)); n }
        Err(e) => { log.line(&format!("hud: Skate's strings unavailable ({e}); trick IDs are shown made readable")); hud::Names::default() }
    };
    let mut hud_tracker = hud::Tracker::default();
    for job in receive {
        match job {
            Job::Rig { path } => {
                let t0 = std::time::Instant::now();
                let glb = root.join("private").join("skater.glb");
                let made = std::fs::read_to_string(&path).map_err(|e| format!("{path}: {e}"))
                    .and_then(|text| retarget::Rig::from_json(&path, &text))
                    .and_then(|rig| {
                        let mesh = SkateVRuntime::skin::SkinMesh::load(&glb, 0)?;
                        retarget::Retarget::new(rig, |n| mesh.inverse_bind_for(n))
                    });
                match made {
                    Ok(r) => {
                        log.line(&format!("retarget: rig {} ({} bones) fitted to {} in {} ms; bind height V {:.3} m, Skate {:.3} m",
                            r.rig.path, r.rig.names.len(), glb.display(), t0.elapsed().as_millis(), r.bind_height.0, r.bind_height.1));
                        let (q, h) = r.reference();
                        if let Ok(mut sh) = shared.lock() {
                            sh.reference = DkrPose { valid: 1, hips: h.to_array(), rot: q, bones: r.rig.names.len() as u32, ..DkrPose::default() };
                        }
                        retarget = Some(r);
                        skate_idx = None;
                    }
                    Err(e) => log.line(&format!("retarget unavailable: {e}")),
                }
            }
            Job::Capture => {
                let busy = capture_due.is_some();
                if parked || session.is_none() || recent.is_empty() {
                    log.line("capture requested while not riding; nothing to capture");
                } else if busy {
                    log.line("capture requested while another is being taken; ignored");
                } else if key_captures >= CAPTURE_KEYS {
                    log.line(&format!("capture requested: the limit of {CAPTURE_KEYS} on request is reached for this session"));
                } else {
                    key_captures += 1;
                    let what = recent.back().map(|s| s.line.clone()).unwrap_or_default();
                    capture_due = Some(PendingCapture { write_at: last_tick + CAPTURE_AFTER_KEY, kind: "key", tick: last_tick, what });
                    log.line(&format!("capture requested at tick {last_tick}; written in 0.5 s"));
                }
            }
            Job::Bodies { bodies, tris } => { bodies_due = Some((bodies, tris)); }
            // #6: the engine picks its physics_mode tables by this index every frame, so a live session takes it at once.
            Job::Difficulty(index) => {
                difficulty = index;
                let applied = session.as_mut().map(|s| s.set_difficulty(index));
                log.line(&format!("difficulty {}{}", DIFFICULTIES.get(index as usize).copied().unwrap_or("?"),
                                  match applied { Some(true) => " (live session)", Some(false) => " refused by the engine", None => "" }));
            }
            Job::Park => {
                parked = true;
                capture_due = None;
                bodies_due = Some((Vec::new(), Vec::new()));
                autopilot = None;
                audio::deactivate();  // voices end, tails play out
                if let Ok(mut sh) = shared.lock() {
                    sh.state.status = STATUS_IDLE;
                    sh.pose = DkrPose::default();
                    sh.hud = hud::DkrHud::default();
                    sh.wants_collision = None;
                }
                log.line("parked: the session stays loaded for the next start");
            }
            // C5: a start with a session already loaded resumes it: collision built around the new spawn and the skater
            // put there with Skate's own activate. No reload (about 8 s), just the collision build (tens of ms).
            Job::Start { preload: true, .. } if session.is_some() => log.line("preload: Skate is already loaded"),
            Job::Start { tris, spawn, heading, autopilot: auto, preload: _ } if session.is_some() => {
                let t0 = std::time::Instant::now();
                if let Ok(mut sh) = shared.lock() {
                    sh.state.status = STATUS_LOADING;
                    sh.state.error = [0; 160];
                    sh.pose = DkrPose::default();
                    sh.wants_collision = None;
                }
                // A collision build from before the park must not land after the resume.
                if building { let _ = built.recv_timeout(std::time::Duration::from_secs(3)); building = false; }
                while built.try_recv().is_ok() {}
                requested = false;
                let keep_m = std::env::var("DKR_ORIGIN_KEEP_M").ok().and_then(|v| v.parse().ok()).unwrap_or(ORIGIN_KEEP_M);  // lab tests only
                let keep_origin = Vec3::new(spawn.x - origin.x, spawn.y - origin.y, 0.0).length() <= keep_m;
                if !keep_origin { origin = Vec3::new(spawn.x, spawn.y, 0.0); }
                let prep = prepare(&tris, origin, &mut area_cache.lock().unwrap_or_else(|e| e.into_inner()));
                let (nt, nr, note) = (prep.tris.len(), prep.rails.len(), prep.note);
                let spawn_s = coords::to_skate(spawn - origin).to_array();
                let s = session.as_mut().unwrap();
                let resumed = builder.ok_or_else(|| "no collision builder".to_string())
                    .and_then(|b| b.build_tagged(prep.tris, prep.tags, prep.rails))
                    .and_then(|prepared| s.install_collision(prepared))
                    .and_then(|_| enter(s, spawn_s, heading, auto));
                match resumed {
                    Ok(()) => {
                        current = Arc::new(tris);
                        (retry_after, dropped, falling, fall_dumps, last_ground, dumped_build) = (None, 0, false, 0, spawn, None);
                        (safe, rolling_ticks, teleporting, marker_return, marker_held, marker_wait, prev_buttons) = (None, 0, false, false, None, None, 0);
                        // P1-13 (0.23.1): Skate's marker survives the park; with the origin kept it still means the same place.
                        if !keep_origin {
                            marker_stale = marker_stale || marker_host.is_some();
                            marker_host = None;
                        }
                        before_teleport = None;
                        (own_activate, own_teleport) = (session.as_ref().map(|s| s.pose().tick), false);
                        active_since = std::time::Instant::now();
                        autopilot = auto.then(|| Autopilot::new(heading, spawn));
                        parked = false;
                        (recent, capture_due, was_down) = (Default::default(), None, false);
                        hud_tracker = hud::Tracker::default();
                        acc = 0.0;
                        audio_state = audio::map::State::default();
                        if let Ok(mut o) = AUDIO_ORIGIN.lock() { *o = origin; }
                        log.line(&format!("session resumed in {} ms (no reload): {nt} triangles, {nr} rails ({note}); spawn ({:.2}, {:.2}, {:.2}) heading {heading:.1}; autopilot {auto}; {}",
                            t0.elapsed().as_millis(), spawn.x, spawn.y, spawn.z,
                            if keep_origin { "origin kept".to_string() } else { format!("origin moved to ({:.0}, {:.0}){}", origin.x, origin.y, if marker_stale { "; the session marker from before no longer applies" } else { "" }) }));
                        if let Ok(mut sh) = shared.lock() {
                            sh.state.status = STATUS_ACTIVE;
                            sh.state.triangles = nt as u32;
                            sh.state.rails = nr as u32;
                            sh.state.collision_center = spawn.to_array();
                            sh.state.heading = heading;
                            // Until the first tick at the new spawn: no stale position from before the park (readers
                            // wait for tick > 0, as after a fresh start).
                            sh.state.pos = spawn.to_array();
                            sh.state.velocity = [0.; 3];
                            sh.state.tick = 0;
                            sh.state.sub_tick = 0.;
                        }
                    }
                    Err(e) => { log.line(&format!("resume failed: {e}")); set_error(&shared, &e); }
                }
            }
            Job::Start { tris, spawn, heading, autopilot: auto, preload } => {
                // A fresh load, also after an engine error dropped the last session: the old error must not show while
                // this one loads (the first in-game ride logged "loading Skate" beside the gesture error).
                if let Ok(mut s) = shared.lock() { s.state.status = STATUS_LOADING; s.state.error = [0; 160]; s.pose.valid = 0; }
                parked = false;
                origin = Vec3::new(spawn.x, spawn.y, 0.0);
                let t0 = std::time::Instant::now();
                let prep = prepare(&tris, origin, &mut area_cache.lock().unwrap_or_else(|e| e.into_inner()));
                let (nt, nr) = (prep.tris.len(), prep.rails.len());
                log.line(&format!("start: {} host triangles -> {nt} after blend and cleanup, {nr} rails ({} ms); {}; {}", tris.len(), t0.elapsed().as_millis(),
                                  prep.note, floor_facing(&tris)));
                let spawn_s = coords::to_skate(spawn - origin).to_array();
                match Session::new_tagged(&root, prep.tris, prep.tags, prep.rails, spawn_s, coords::skate_heading(heading))
                    .and_then(|mut s| {
                        configure(&mut s);
                        s.set_difficulty(difficulty);
                        enter(&mut s, spawn_s, heading, auto)?;
                        Ok(s)
                    })
                {
                    Ok(s) => {
                        builder = Some(s.collision_builder());
                        current = Arc::new(tris);
                        (retry_after, dropped, falling, fall_dumps, last_ground, dumped_build) = (None, 0, false, 0, spawn, None);
                        (safe, rolling_ticks, teleporting, marker_return, marker_held, marker_host, marker_wait, prev_buttons) = (None, 0, false, false, None, None, None, 0);
                        (marker_stale, before_teleport) = (false, None);  // a new Skate session has no marker
                        (own_activate, own_teleport) = (Some(s.pose().tick), false);
                        (recent, capture_due, was_down) = (Default::default(), None, false);
                        hud_tracker = hud::Tracker::default();
                        active_since = std::time::Instant::now();
                        session = Some(s);
                        acc = 0.0;
                        autopilot = auto.then(|| Autopilot::new(heading, spawn));
                        audio_state = audio::map::State::default();
                        if let Ok(mut o) = AUDIO_ORIGIN.lock() { *o = origin; }
                        if preload {
                            // #2: parked as after a ride (the skater stands at the spawn, off the board); no steps until a start.
                            parked = true;
                            autopilot = None;
                            if let Ok(mut s) = shared.lock() { s.state.status = STATUS_IDLE; s.pose = DkrPose::default(); s.wants_collision = None; }
                            log.line(&format!("preloaded in {} ms around ({:.0}, {:.0}, {:.0}); parked until the first start", t0.elapsed().as_millis(), spawn.x, spawn.y, spawn.z));
                            continue;
                        }
                        log.line(&format!("session ready in {} ms; spawn ({:.2}, {:.2}, {:.2}) heading {heading:.1}; autopilot {auto}", t0.elapsed().as_millis(), spawn.x, spawn.y, spawn.z));
                        if let Ok(mut s) = shared.lock() {
                            s.state.status = STATUS_ACTIVE;
                            s.state.triangles = nt as u32;
                            s.state.rails = nr as u32;
                            s.state.collision_center = spawn.to_array();
                            s.state.heading = heading;
                        }
                    }
                    // A failed preload leaves no session: the first start loads again, and shows its own error if any.
                    Err(e) if preload => {
                        log.line(&format!("preload failed (the first start loads again): {e}"));
                        if let Ok(mut s) = shared.lock() { s.state.status = STATUS_IDLE; s.state.error = [0; 160]; }
                    }
                    Err(e) => { log.line(&format!("session failed: {e}")); set_error(&shared, &e); }
                }
            }
            Job::Collision { tris, center } => {
                let Some(b) = builder else { continue };
                if building { continue; }
                building = true;
                // The area may be centred ahead of the skater: the ground check is under the skater (at the centre's
                // height, which is the last ground's while falling).
                let check = Vec3::new(skater_at.x, skater_at.y, center.z);
                start_build(b, built_send.clone(), log.clone(), origin, Arc::new(tris), center, check, area_cache.clone());
            }
            Job::Step { dt, pad, posted } => {
                // ABI 8: every step taken off the queue counts as handled, used or skipped, so the host never waits on one.
                steps_handled += 1;
                let mark = |sh: &Mutex<Shared>, n: u64| { if let Ok(mut s) = sh.lock() { s.state.steps = n; } };
                if parked { mark(&shared, steps_handled); continue; }
                let Some(s) = session.as_mut() else { mark(&shared, steps_handled); continue };
                if posted < active_since { mark(&shared, steps_handled); continue; }  // queued while the session was loading: no catch-up burst
                // The controller takes over from the autopilot on its first real input, for the rest of the session. The
                // pad is passed to Skate unchanged (XInput is Skate 3's own Xbox 360 layout, as in SK8V): left stick to
                // steer and lean, right stick to Flick-It, the face buttons and triggers as Skate maps them.
                if autopilot.is_some() && pad_active(&pad) {
                    autopilot = None;
                    log.line(&format!("autopilot off: the controller took over (buttons {:#06x}, left ({}, {}), right ({}, {}), triggers {} {})",
                        pad.buttons, pad.lx, pad.ly, pad.rx, pad.ry, pad.left_trigger, pad.right_trigger));
                    if let Ok(mut sh) = shared.lock() { sh.state.heading = f32::NAN; }  // DkrState.heading NaN: no autopilot
                }
                // Queued while a slow tick ran (the game hogging the CPU, e.g. a loading screen): skip it. The skater
                // slows down instead of bursting at 2-3x speed once the worker catches up (session 1: 165 ticks/s).
                if posted.elapsed() > std::time::Duration::from_millis(100) { dropped += 1; mark(&shared, steps_handled); continue; }
                if let Ok((res, center, nt, nr, ok, host)) = built.try_recv() {
                    building = false;
                    requested = false;
                    // P1-13: a build for where the skater was before a marker return is no use where it is now.
                    let off = Vec3::new(center.x - skater_at.x, center.y - skater_at.y, 0.0).length();
                    if ok && off > COVER_M + MAX_LEAD_M {
                        log.line(&format!("collision built around ({:.1}, {:.1}, {:.1}) dropped: the skater is {off:.0} m from it now", center.x, center.y, center.z));
                    } else if !ok {
                        if marker_wait.take().is_some() { log.line("session marker: no collision for the return (no ground there); Skate goes on without it"); }
                        retry_after = Some(std::time::Instant::now() + std::time::Duration::from_secs(3));
                        log.line(&format!("re-centre rejected: no ground under the skater at ({:.1}, {:.1}, {:.1}) among {} host triangles; keeping the current collision, retrying in 3 s", center.x, center.y, center.z, host.len()));
                    } else {
                        match res.and_then(|p| s.install_collision(p)) {
                            Ok(()) => {
                                current = host;
                                let took = requested_at.elapsed().as_secs_f32();
                                build_secs = (0.7 * build_secs + 0.3 * took).clamp(0.1, 2.0);
                                log.line(&format!("collision installed around ({:.1}, {:.1}, {:.1}): {nt} triangles, {nr} rails, {:.0} ms after the request (average {:.0} ms); {}",
                                                  center.x, center.y, center.z, took * 1000.0, build_secs * 1000.0, floor_facing(&current)));
                                if let Ok(mut sh) = shared.lock() {
                                    sh.state.collision_builds += 1;
                                    sh.state.collision_center = center.to_array();
                                    sh.state.triangles = nt as u32;
                                    sh.state.rails = nr as u32;
                                }
                                if marker_wait.take().is_some() { log.line("session marker: the collision for the return is in; Skate goes on"); }
                            }
                            Err(e) => log.line(&format!("collision install failed: {e}")),
                        }
                    }
                }
                // P1-14: the host's moving bodies as of this frame, for the ticks below.
                if let Some((bodies, tris)) = bodies_due.take() {
                    let (surfaces, proxies) = skate_bodies(&bodies, &tris, origin);
                    if (bodies.len() > 0) != (bodies_now > 0) {
                        log.line(&format!("moving bodies: {} from the host ({} triangles){}", bodies.len(), surfaces.len(),
                                          if bodies.is_empty() { "; none near now" } else { "" }));
                    }
                    bodies_now = bodies.len();
                    s.set_dynamic_surfaces(&surfaces);
                    s.set_host_bodies(proxies);
                }
                acc = (acc + dt.clamp(0.0, 0.25)).min(0.25);
                let period = s.period();
                // The clock moves with the step, before its ticks run; each tick below republishes what is left.
                if let Ok(mut sh) = shared.lock() {
                    (sh.state.sub_tick, sh.state.period, sh.state.steps) = (acc / period, period, steps_handled);
                }
                while acc >= period {
                    // P1-13: Skate holds still at a marker return until the collision there is in.
                    if let Some(until) = marker_wait {
                        if std::time::Instant::now() < until { acc = 0.0; break; }
                        marker_wait = None;
                        log.line(&format!("session marker: no collision for the return after {MARKER_WAIT_S:.0} s; Skate goes on without it"));
                    }
                    acc -= period;
                    let p = s.pose();
                    let at = coords::from_skate(p.root.w_axis.truncate()) + origin;
                    let v = coords::from_skate(p.velocity);
                    let c = match autopilot.as_mut() {
                        Some(a) => a.controls(p.tick, at, v, &p.state),
                        None if pad.connected != 0 => Controls { buttons: pad.buttons, triggers: [pad.left_trigger, pad.right_trigger], left: [pad.lx, pad.ly], right: [pad.rx, pad.ry] },
                        None => Controls::default(),
                    };
                    if let Some(h) = autopilot.as_mut().and_then(|a| a.turn_to.take()) {
                        let r = s.activate(coords::to_skate(at - origin + Vec3::new(0.0, 0.0, 0.05)).to_array(), coords::skate_heading(h));
                        own_activate = Some(p.tick);
                        log.line(&format!("autopilot: blocked at ({:.1}, {:.1}, {:.1}) in {}; turned in place to heading {h:.0}{}", at.x, at.y, at.z, p.state,
                                          r.err().map(|e| format!(" (failed: {e})")).unwrap_or_default()));
                    }
                    let ctl = c;
                    // P1-13: LB + d-pad down may place a session marker (Skate decides): keep the collision here.
                    if c.buttons & LB != 0 && c.buttons & DPAD_DOWN != 0 && prev_buttons & DPAD_DOWN == 0 && !current.is_empty() {
                        let gathered = shared.lock().map(|sh| Vec3::from_array(sh.state.collision_center)).unwrap_or(at);
                        marker_host = Some((current.clone(), gathered));
                        marker_stale = false;
                        log.line(&format!("session marker: LB + d-pad down at ({:.1}, {:.1}, {:.1}) in {}; the collision here is kept for a return", at.x, at.y, at.z, p.state));
                    }
                    prev_buttons = c.buttons;
                    // 0.24.0: LB + d-pad up held: a teleport beginning now is Skate's session marker return.
                    if c.buttons & LB != 0 && c.buttons & DPAD_UP != 0 { marker_held = Some(p.tick); }
                    let t0 = std::time::Instant::now();
                    // P1-13: the pad goes through `collect` (which also runs the session marker's clock) and the tick
                    // through `advance`, as SK8V's worker does. `Session::tick`, used up to 0.20.0, skips that clock.
                    packet = packet.wrapping_add(1);
                    s.collect(InputFrame::from_pad(c.buttons, c.triggers, c.left, c.right, packet), period);
                    if let Err(e) = s.advance() { log.line(&format!("tick failed: {e}")); set_error(&shared, &e); session = None; break; }
                    let ms = t0.elapsed().as_secs_f64() * 1000.0;
                    cost_sum += ms; cost_max = cost_max.max(ms); cost_n += 1; window_ticks += 1;
                    // P1-14: a car striking the rider, once per body per quarter second (SK8V's log line). The pushes the
                    // board gives the cars are dropped: Deckrunner doesn't move the game's cars.
                    for i in s.take_host_impacts() {
                        let now = std::time::Instant::now();
                        if hits.iter().any(|(t, at)| *t == i.tag && now.duration_since(*at).as_millis() < 250) { continue; }
                        hits.retain(|(t, at)| *t != i.tag && now.duration_since(*at).as_secs() < 5);
                        hits.push((i.tag, now));
                        let j = Vec3::from_array(i.impulse).length();
                        log.line(&format!("car hit: body {:07x} closing at {:.1} m/s, impulse {j:.0} N s on the {:.0} kg board and rider, {} skeleton parts struck; state {}",
                                          i.tag & 0x0fff_ffff, i.closing, i.assembly_mass, i.struck_parts, s.pose().state));
                    }
                    let _ = s.take_host_impulses();
                    let p = s.pose();
                    if audio::wanted() { audio::on_tick(s, &p, &mut audio_state); }
                    let at = coords::from_skate(p.root.w_axis.truncate()) + origin;
                    let v = coords::from_skate(p.velocity);
                    let sc = s.score();
                    let hud_now = hud_tracker.update(s, p.tick, &names);
                    if !sc.trick_name.is_empty() && sc.trick_name != last_trick { log.line(&format!("tick {}: trick {}", p.tick, sc.trick_name)); last_trick = sc.trick_name.clone(); }
                    if let Some(t) = trace.as_mut() {
                        if p.tick % 6 == 0 { let _ = writeln!(t, "{}", serde_json::json!({"tick": p.tick, "state": p.state, "pos": [at.x, at.y, at.z], "speed": Vec3::new(v.x, v.y, 0.).length()})); }
                    }
                    // C3: retarget this tick's pose onto V's rig (game axes, the session's coordinates, plus the origin).
                    let mut pose = None;
                    if let Some(r) = retarget.as_ref() {
                        if skate_idx.is_none() {
                            skate_idx = retarget::Retarget::skate_indices(&p.names);
                            if skate_idx.is_none() && pose_failed == 0 { log.line(&format!("retarget: the skater pose lacks a mapped joint ({} joints)", p.names.len())); }
                        }
                        match skate_idx.as_ref().and_then(|idx| r.solve(p.root, &p.bones, idx)) {
                            Some(solved) => {
                                let (q, h) = r.export(&solved.world);
                                let bits = solved.unreachable.iter().enumerate().fold(0u32, |b, (k, &u)| b | ((u as u32) << k));
                                pose = Some(DkrPose { valid: 1, tick: p.tick, hips: (h + origin).to_array(), unreachable: bits, rot: q, root: at.to_array(),
                                                      bones: r.rig.names.len() as u32, ..DkrPose::default() });
                                if posed == 0 { log.line(&format!("retarget: first pose at tick {} ({}); hips ({:.2}, {:.2}, {:.2}), {:.2} m above the skater root", p.tick, p.state, h.x + origin.x, h.y + origin.y, h.z, h.z - at.z)); }
                                posed += 1;
                            }
                            None => { if pose_failed == 0 { log.line(&format!("retarget: no pose at tick {} ({})", p.tick, p.state)); } pose_failed += 1; }
                        }
                    }
                    // C3b: the board, whether or not V's rig is loaded.
                    if board_idx.is_none() { board_idx = p.names.iter().position(|n| n.eq_ignore_ascii_case("SKATEBOARD_ROOT")); }
                    if let Some(b) = board_idx.and_then(|i| p.bones.get(i)) {
                        let (bp, bq) = retarget::board_transform(p.root, *b);
                        let pose = pose.get_or_insert_with(|| DkrPose { tick: p.tick, root: at.to_array(), ..DkrPose::default() });
                        pose.board_pos = (bp + origin).to_array();
                        pose.board_rot = bq;
                        pose.board_valid = 1;
                    }
                    // C4: Skate's camera, as SK8V hands it to GTA (position, look direction, field of view).
                    if let Some((cp, cb, fov)) = p.camera {
                        let fwd = (coords::basis() * cb.z_axis).normalize_or_zero();
                        if fwd != Vec3::ZERO && cp.is_finite() {
                            let pose = pose.get_or_insert_with(|| DkrPose { tick: p.tick, root: at.to_array(), ..DkrPose::default() });
                            pose.cam_pos = (coords::from_skate(cp) + origin).to_array();
                            pose.cam_fwd = fwd.to_array();
                            pose.cam_fov = fov;
                            pose.cam_valid = 1;
                        }
                    }
                    // P1-11: this tick for captures (state, trick, position, velocity, the board, and the pad that drove it).
                    let board = pose.as_ref().filter(|q| q.board_valid != 0).map(|q| (q.board_pos, q.board_rot));
                    recent.push_back(Sample { pos: at, line: serde_json::json!({
                        "tick": p.tick, "state": p.state, "trick": sc.trick_name, "pos": [at.x, at.y, at.z], "vel": [v.x, v.y, v.z],
                        "speed": Vec3::new(v.x, v.y, 0.).length(), "board": board.map(|b| b.0), "board_rot": board.map(|b| b.1),
                        "pad": {"buttons": ctl.buttons, "triggers": ctl.triggers, "left": ctl.left, "right": ctl.right},
                    }).to_string() });
                    while recent.len() > CAPTURE_TICKS { recent.pop_front(); }
                    last_tick = p.tick;
                    let down = p.state.starts_with("Wipeout");
                    if down && !was_down && capture_due.is_none() && bail_captures < CAPTURE_BAILS && bail_capture_on {
                        bail_captures += 1;
                        let what = format!("{} at ({:.2}, {:.2}, {:.2}), {:.1} m/s, last trick {}", p.state, at.x, at.y, at.z,
                                           Vec3::new(v.x, v.y, 0.).length(), if last_trick.is_empty() { "none" } else { &last_trick });
                        log.line(&format!("bail at tick {}: {what}; capture {} of {CAPTURE_BAILS} in 1 s", p.tick, bail_captures));
                        capture_due = Some(PendingCapture { write_at: p.tick + CAPTURE_AFTER_BAIL, kind: "bail", tick: p.tick, what });
                    }
                    was_down = down;
                    // P0-15: a respawn far below the last rolling spot goes back there.
                    let flat_speed = Vec3::new(v.x, v.y, 0.).length();
                    if p.state == "PhysicsGround" {
                        rolling_ticks += 1;
                        if rolling_ticks >= SAFE_TICKS && flat_speed > 0.5 { safe = Some((at, coords::gta_heading(v))); }
                    } else { rolling_ticks = 0; }
                    if p.state != "Teleporting" && !teleporting {
                        let h = if Vec3::new(v.x, v.y, 0.0).length() > 0.5 { coords::gta_heading(v) } else { before_teleport.map_or(0.0, |b| b.1) };
                        before_teleport = Some((at, h));
                    }
                    if p.state == "Teleporting" {
                        if !teleporting {
                            marker_return = marker_held.is_some_and(|t| p.tick.saturating_sub(t) <= MARKER_HOLD_TICKS);
                            own_teleport = own_activate.is_some_and(|t| p.tick.saturating_sub(t) <= OWN_TELEPORT_TICKS);
                        }
                        teleporting = true;
                    } else if teleporting && own_teleport {
                        teleporting = false;
                    } else if teleporting && marker_return && marker_stale {
                        // P1-13 (0.23.1): a return to a marker placed before the origin moved goes nowhere anyone has been:
                        // the skater is put back where it was.
                        teleporting = false;
                        if let Some((back, h)) = before_teleport {
                            let r = s.activate(coords::to_skate(back - origin + Vec3::new(0.0, 0.0, 0.05)).to_array(), coords::skate_heading(h));
                            own_activate = Some(p.tick);
                            log.line(&format!("session marker: return at tick {} to ({:.1}, {:.1}, {:.1}) undone: that marker was placed before the origin moved; put back at ({:.1}, {:.1}, {:.1}){}",
                                              p.tick, at.x, at.y, at.z, back.x, back.y, back.z, r.err().map(|e| format!(" (failed: {e})")).unwrap_or_default()));
                        }
                    } else if teleporting && marker_return {
                        // P1-13: a teleport while LB + d-pad up is held is a session marker return (0.24.0; up to 0.23.1, any
                        // teleport outside a wipeout).
                        teleporting = false;
                        let cc = shared.lock().map(|sh| Vec3::from_array(sh.state.collision_center)).unwrap_or(at);
                        let off = Vec3::new(at.x - cc.x, at.y - cc.y, 0.0).length();
                        log.line(&format!("session marker: return at tick {} to ({:.1}, {:.1}, {:.1}), {off:.0} m from the installed collision's centre", p.tick, at.x, at.y, at.z));
                        if off > COVER_M {
                            match (builder, marker_host.clone()) {
                                (Some(b), Some((host, gathered))) => {
                                    log.line(&format!("session marker: outside the collision; Skate waits while it is built from the {} host triangles kept at the marker \
                                                       (gathered around ({:.1}, {:.1}, {:.1}), {:.0} m from it)", host.len(), gathered.x, gathered.y, gathered.z,
                                                      Vec3::new(at.x - gathered.x, at.y - gathered.y, 0.0).length()));
                                    (building, requested, requested_at) = (true, true, std::time::Instant::now());
                                    marker_wait = Some(std::time::Instant::now() + std::time::Duration::from_secs_f32(MARKER_WAIT_S));
                                    // Centred where the triangles were gathered, so re-centring measures the skater's
                                    // distance from the set's real middle; the ground check stays at the marker.
                                    start_build(b, built_send.clone(), log.clone(), origin, host, gathered, at, area_cache.clone());
                                }
                                _ => log.line("session marker: outside the collision and none kept for it; the next re-centre has to catch up"),
                            }
                        }
                        (falling, last_ground, rolling_ticks) = (false, at, 0);
                    } else if teleporting {
                        teleporting = false;
                        if let Some((spot, heading)) = safe.filter(|(spot, _)| at.z < spot.z - RESPAWN_DROP) {
                            let r = s.activate(coords::to_skate(spot - origin + Vec3::new(0.0, 0.0, 0.05)).to_array(), coords::skate_heading(heading));
                            own_activate = Some(p.tick);
                            log.line(&format!("respawn at tick {} ({:.1}, {:.1}, {:.1}) is {:.1} m below where the skater last rolled: put back at ({:.2}, {:.2}, {:.2}) heading {heading:.0}{}",
                                              p.tick, at.x, at.y, at.z, spot.z - at.z, spot.x, spot.y, spot.z, r.err().map(|e| format!(" (failed: {e})")).unwrap_or_default()));
                            (falling, last_ground, rolling_ticks) = (false, spot, 0);
                        }
                    }
                    if capture_due.as_ref().is_some_and(|c| p.tick >= c.write_at) {
                        let c = capture_due.take().unwrap();
                        captures += 1;
                        let name = format!("capture-{captures}-{}-tick{}", c.kind, c.tick);
                        let header = format!("deckrunner capture {captures} ({}) at tick {}: {}; host triangles within {CAPTURE_RADIUS:.0} m of the skater's last {} ticks; game world, Z up; lab-local, do not share",
                                             c.kind, c.tick, c.what.replace('\n', " "), recent.len());
                        let samples: Vec<Sample> = recent.iter().map(|x| Sample { pos: x.pos, line: x.line.clone() }).collect();
                        write_capture(&out_dir, name, header, current.clone(), samples, log.clone());
                    }
                    if let Ok(mut sh) = shared.lock() {
                        if let Some(pose) = pose { sh.pose = pose; }
                        sh.hud = hud_now;
                        let st = &mut sh.state;
                        st.tick = p.tick;
                        (st.sub_tick, st.period, st.steps) = (acc / period, period, steps_handled);
                        st.pos = at.to_array();
                        st.velocity = v.to_array();
                        st.speed = Vec3::new(v.x, v.y, 0.0).length();
                        put(&mut st.state, &p.state);
                        put(&mut st.trick, &sc.trick_name);
                        if p.state.starts_with("Grind") { st.grind_ticks += 1; }
                        if p.state.contains("Air") { st.air_ticks += 1; }
                        st.heading = autopilot.as_ref().map_or(f32::NAN, |a| a.heading);
                        let cc = Vec3::from_array(st.collision_center);
                        let backoff = retry_after.is_some_and(|t| std::time::Instant::now() < t);
                        // SK8V's rule at Deckrunner's 40 m gather radius: due when 32 m out, or sooner when the build
                        // would finish with the skater past 34 m; centred ahead by the build's travel, at most 24 m.
                        let flat_v = Vec3::new(v.x, v.y, 0.0);
                        let d = Vec3::new(at.x - cc.x, at.y - cc.y, 0.0).length();
                        let due = d > RECENTRE_M || d + flat_v.length() * build_secs * BUILD_LEAD > COVER_M;
                        if due && !requested && !building && !backoff {
                            let c = at + (flat_v * build_secs * BUILD_LEAD).clamp_length_max(MAX_LEAD_M);
                            // While falling, gather at the last ground height: the 3D actor-bounds test found nothing
                            // around a centre 100 m under the street (in-game run 2: 0 triangles at z -93.6).
                            sh.wants_collision = Some(if at.z < last_ground.z - 3.0 { Vec3::new(c.x, c.y, last_ground.z) } else { c });
                            requested = true;
                            requested_at = std::time::Instant::now();
                        }
                    }
                    skater_at = at;
                    // Fell through the collision: record where, and keep the installed set for offline replay.
                    if p.state.starts_with("PhysicsGround") || p.state.starts_with("Grind") { last_ground = at; }
                    let (cc_z, builds) = shared.lock().map(|sh| (sh.state.collision_center[2], sh.state.collision_builds)).unwrap_or((at.z, 0));
                    if !falling && at.z < cc_z.min(last_ground.z) - 15.0 {
                        falling = true;
                        let dump = fall_dumps < 3 && dumped_build != Some(builds);  // one file per installed set
                        let name = format!("fall-{}-tick{}.obj", fall_dumps + 1, p.tick);
                        log.line(&format!("fell through at tick {} ({}): now ({:.1}, {:.1}, {:.1}), last on ground at ({:.2}, {:.2}, {:.2}), {} installed host triangles{}",
                            p.tick, p.state, at.x, at.y, at.z, last_ground.x, last_ground.y, last_ground.z, current.len(),
                            if dump { format!("; written to {name}") } else { String::new() }));
                        if dump {
                            fall_dumps += 1;
                            dumped_build = Some(builds);
                            let (tris, path, log2) = (current.clone(), out_dir.join(&name), log.clone());
                            let header = format!("deckrunner fall at tick {}; last ground ({:.3}, {:.3}, {:.3}); game world, Z up; lab-local, do not share", p.tick, last_ground.x, last_ground.y, last_ground.z);
                            let _ = std::thread::Builder::new().name("deckrunner-dump".into()).spawn(move || {
                                if let Err(e) = catch_unwind(AssertUnwindSafe(|| write_obj(&path, &tris, &header))).unwrap_or(Ok(())) { log2.line(&format!("dump failed: {e}")); }
                            });
                        }
                    } else if falling && at.z > last_ground.z - 3.0 { falling = false; }
                }
                if window.elapsed().as_secs_f32() >= 1.0 {
                    if dropped > 0 { log.line(&format!("worker behind the game: skipped {dropped} stale steps")); dropped = 0; }
                    if let Ok(mut sh) = shared.lock() {
                        sh.state.ticks_per_s = window_ticks as f32 / window.elapsed().as_secs_f32();
                        sh.state.step_ms_avg = if cost_n > 0 { (cost_sum / cost_n as f64) as f32 } else { 0. };
                        sh.state.step_ms_max = cost_max as f32;
                    }
                    if let Some(t) = trace.as_mut() { let _ = t.flush(); }
                    window = std::time::Instant::now();
                    window_ticks = 0; cost_sum = 0.; cost_max = 0.; cost_n = 0;
                }
            }
        }
    }
}

fn tris_from(ptr: *const f32, flags: *const u8, n: u32) -> Vec<Tri> {
    if ptr.is_null() || n == 0 || n > 4_000_000 { return Vec::new(); }
    let f = unsafe { std::slice::from_raw_parts(ptr, n as usize * 9) };
    let fl = if flags.is_null() { None } else { Some(unsafe { std::slice::from_raw_parts(flags, n as usize) }) };
    (0..n as usize).map(|i| {
        let v = |k: usize| Vec3::new(f[i * 9 + k * 3], f[i * 9 + k * 3 + 1], f[i * 9 + k * 3 + 2]);
        Tri { p: [v(0), v(1), v(2)], physical: fl.map(|x| x[i] != 0).unwrap_or(true) }
    }).collect()
}

fn guard<T>(fallback: T, f: impl FnOnce() -> T) -> T { catch_unwind(AssertUnwindSafe(f)).unwrap_or(fallback) }

#[unsafe(no_mangle)]
pub extern "C" fn dkr_abi_version() -> u32 { ABI_VERSION }

/// assets: converted Skate data (`skate-data/assets`); out_dir: where the runtime log and trace go.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dkr_create(assets: *const c_char, out_dir: *const c_char) -> *mut c_void {
    guard(std::ptr::null_mut(), || {
        if assets.is_null() || out_dir.is_null() { return std::ptr::null_mut(); }
        let root = PathBuf::from(unsafe { CStr::from_ptr(assets) }.to_string_lossy().into_owned());
        let out = PathBuf::from(unsafe { CStr::from_ptr(out_dir) }.to_string_lossy().into_owned());
        let _ = std::fs::create_dir_all(&out);
        let pruned = prune(&out);
        let log = Arc::new(Log(Mutex::new(std::fs::OpenOptions::new().create(true).append(true).open(out.join("deckrunner-runtime.log")).ok())));
        let hook_log = log.clone();
        std::panic::set_hook(Box::new(move |info| hook_log.line(&format!("PANIC: {info}"))));
        log.line(&format!("=== new session: deckrunner runtime {RUNTIME_VERSION} ({RUNTIME_COMMIT}), ABI {ABI_VERSION}; assets {}{}", root.display(),
                          if pruned > 0 { format!("; {pruned} old captures and traces removed") } else { String::new() }));
        let shared = Arc::new(Mutex::new(Shared { state: DkrState::default(), wants_collision: None, pose: DkrPose::default(), reference: DkrPose::default(), hud: hud::DkrHud::default() }));
        let (send, receive) = mpsc::channel();
        let (sh, lg) = (shared.clone(), log.clone());
        let spawned = std::thread::Builder::new().name("deckrunner-skate".into()).spawn(move || {
            if catch_unwind(AssertUnwindSafe(|| worker(root, out, receive, sh.clone(), lg.clone()))).is_err() {
                lg.line("worker panicked");
                if let Ok(mut s) = sh.lock() { s.state.status = STATUS_ERROR; put(&mut s.state.error, "worker panicked (see log)"); }
            }
        });
        if spawned.is_err() { return std::ptr::null_mut(); }
        Box::into_raw(Box::new(Runtime { send, shared, log })) as *mut c_void
    })
}

/// tris: 9 floats per triangle (game world), flags: 1 = physical layer, 0 = detail layer (may be null: all physical).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dkr_start(rt: *mut c_void, tris: *const f32, flags: *const u8, n: u32, spawn: *const f32,
                                   heading_deg: f32, autopilot: u32) -> u32 {
    guard(0, || {
        let Some(r) = (unsafe { (rt as *const Runtime).as_ref() }) else { return 0 };
        if spawn.is_null() { return 0; }
        let s = unsafe { std::slice::from_raw_parts(spawn, 3) };
        r.send.send(Job::Start { tris: tris_from(tris, flags, n), spawn: Vec3::new(s[0], s[1], s[2]), heading: heading_deg, autopilot: autopilot != 0, preload: false }).is_ok() as u32
    })
}

/// #2: loads Skate in the background around `spawn` (game world; triangles as `dkr_start`) and parks it, so the first
/// `dkr_start` resumes at once instead of loading (about 8 s). Nothing is stepped or reported active until that start.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dkr_preload(rt: *mut c_void, tris: *const f32, flags: *const u8, n: u32, spawn: *const f32, heading_deg: f32) -> u32 {
    guard(0, || {
        let Some(r) = (unsafe { (rt as *const Runtime).as_ref() }) else { return 0 };
        if spawn.is_null() { return 0; }
        let s = unsafe { std::slice::from_raw_parts(spawn, 3) };
        r.send.send(Job::Start { tris: tris_from(tris, flags, n), spawn: Vec3::new(s[0], s[1], s[2]), heading: heading_deg, autopilot: false, preload: true }).is_ok() as u32
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dkr_step(rt: *mut c_void, dt: f32, pad: *const DkrPad) -> u32 {
    guard(0, || {
        let Some(r) = (unsafe { (rt as *const Runtime).as_ref() }) else { return 0 };
        let pad = unsafe { pad.as_ref() }.copied().unwrap_or_default();
        r.send.send(Job::Step { dt: if dt.is_finite() { dt } else { 0.0 }, pad, posted: std::time::Instant::now() }).is_ok() as u32
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dkr_get_state(rt: *const c_void, out: *mut DkrState) -> u32 {
    guard(0, || {
        let (Some(r), Some(o)) = (unsafe { (rt as *const Runtime).as_ref() }, unsafe { out.as_mut() }) else { return 0 };
        match r.shared.lock() { Ok(s) => { *o = s.state; 1 } Err(_) => 0 }
    })
}

/// ABI 8 (P1-6): the state and the pose as one consistent pair, copied under one lock, so the host's clock (state) and
/// what it draws (pose) belong to the same tick.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dkr_get_snapshot(rt: *const c_void, state: *mut DkrState, pose: *mut DkrPose) -> u32 {
    guard(0, || {
        let (Some(r), Some(st), Some(po)) = (unsafe { (rt as *const Runtime).as_ref() }, unsafe { state.as_mut() }, unsafe { pose.as_mut() }) else { return 0 };
        match r.shared.lock() { Ok(s) => { *st = s.state; *po = s.pose; 1 } Err(_) => 0 }
    })
}

/// Returns 1 and writes the requested centre (game world) when the runtime wants collision around the skater.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dkr_wants_collision(rt: *const c_void, center: *mut f32) -> u32 {
    guard(0, || {
        let Some(r) = (unsafe { (rt as *const Runtime).as_ref() }) else { return 0 };
        if center.is_null() { return 0; }
        let Ok(mut s) = r.shared.lock() else { return 0 };
        match s.wants_collision.take() {
            Some(c) => { unsafe { std::slice::from_raw_parts_mut(center, 3) }.copy_from_slice(&c.to_array()); 1 }
            None => 0,
        }
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dkr_install_collision(rt: *mut c_void, tris: *const f32, flags: *const u8, n: u32, center: *const f32) -> u32 {
    guard(0, || {
        let Some(r) = (unsafe { (rt as *const Runtime).as_ref() }) else { return 0 };
        if center.is_null() { return 0; }
        let c = unsafe { std::slice::from_raw_parts(center, 3) };
        r.send.send(Job::Collision { tris: tris_from(tris, flags, n), center: Vec3::new(c[0], c[1], c[2]) }).is_ok() as u32
    })
}

/// C3: fits Skate's skater skeleton to V's rig (WolvenKit JSON of the player's `.rig`) on the worker; from then on each
/// tick's pose is retargeted. Returns 1 when the job was posted (the runtime log says whether the fit succeeded).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dkr_load_rig(rt: *mut c_void, path: *const c_char) -> u32 {
    guard(0, || {
        let Some(r) = (unsafe { (rt as *const Runtime).as_ref() }) else { return 0 };
        if path.is_null() { return 0; }
        let path = unsafe { CStr::from_ptr(path) }.to_string_lossy().into_owned();
        r.send.send(Job::Rig { path }).is_ok() as u32
    })
}

/// C3: the latest tick's retargeted pose (game world). Returns `valid`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dkr_get_pose(rt: *const c_void, out: *mut DkrPose) -> u32 {
    guard(0, || {
        let (Some(r), Some(o)) = (unsafe { (rt as *const Runtime).as_ref() }, unsafe { out.as_mut() }) else { return 0 };
        match r.shared.lock() { Ok(s) => { *o = s.pose; o.valid } Err(_) => 0 }
    })
}

/// C3: V's rig in its own reference pose (model space), for checking the graph's conventions in game. Returns `valid`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dkr_reference_pose(rt: *const c_void, out: *mut DkrPose) -> u32 {
    guard(0, || {
        let (Some(r), Some(o)) = (unsafe { (rt as *const Runtime).as_ref() }, unsafe { out.as_mut() }) else { return 0 };
        match r.shared.lock() { Ok(s) => { *o = s.reference; o.valid } Err(_) => 0 }
    })
}

/// C5: stops stepping and keeps the session loaded; the next `dkr_start` resumes it at a new spawn without reloading.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dkr_park(rt: *mut c_void) -> u32 {
    guard(0, || {
        let Some(r) = (unsafe { (rt as *const Runtime).as_ref() }) else { return 0 };
        r.send.send(Job::Park).is_ok() as u32
    })
}

/// P1-11 (ABI 9): asks for a capture: the skater's last 5 s at every tick and the host collision within 15 m of that
/// path, written about 0.5 s later beside the runtime log. Returns 1 when the request was queued; the runtime log says
/// what was written, or why not (not riding, one already being taken, the limit of 20 reached).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dkr_capture(rt: *mut c_void) -> u32 {
    guard(0, || {
        let Some(r) = (unsafe { (rt as *const Runtime).as_ref() }) else { return 0 };
        r.send.send(Job::Capture).is_ok() as u32
    })
}

/// P1-14 (ABI 10): the host's moving bodies near the skater this frame (cars), replacing the last set; their triangles
/// (`ntris` of 9 floats, game world) are indexed by each body's `first_tri` and `tri_count`. Pass `n` 0 when none are
/// near. Returns 1 when posted.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dkr_set_bodies(rt: *mut c_void, bodies: *const DkrBody, n: u32, tris: *const f32, ntris: u32) -> u32 {
    guard(0, || {
        let Some(r) = (unsafe { (rt as *const Runtime).as_ref() }) else { return 0 };
        if n > 4096 || ntris > 1_000_000 || (n > 0 && bodies.is_null()) || (ntris > 0 && tris.is_null()) { return 0; }
        let b: Vec<DkrBody> = if n == 0 { Vec::new() } else { unsafe { std::slice::from_raw_parts(bodies, n as usize) }.to_vec() };
        if b.iter().any(|x| x.size as usize != std::mem::size_of::<DkrBody>()) { return 0; }
        let f = if ntris == 0 { &[][..] } else { unsafe { std::slice::from_raw_parts(tris, ntris as usize * 9) } };
        let t: Vec<[Vec3; 3]> = f.chunks_exact(9).map(|c| [Vec3::new(c[0], c[1], c[2]), Vec3::new(c[3], c[4], c[5]), Vec3::new(c[6], c[7], c[8])]).collect();
        r.send.send(Job::Bodies { bodies: b, tris: t }).is_ok() as u32
    })
}

/// #6: Skate 3's difficulty (its physics_mode): 0 easy (the default), 1 normal, 2 hardcore, 3 motorized. Applies to the
/// running session at once and to every session after it. Returns 1 when sent, 0 for an unknown index.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dkr_set_difficulty(rt: *mut c_void, index: u32) -> u32 {
    guard(0, || {
        let Some(r) = (unsafe { (rt as *const Runtime).as_ref() }) else { return 0 };
        ((index as usize) < DIFFICULTIES.len() && r.send.send(Job::Difficulty(index)).is_ok()) as u32
    })
}

/// ABI 13: the HUD's values after the last Skate tick (`hud::DkrHud`, `out.size` set by the caller). Returns 1 once a
/// tick of the current ride has filled them in, 0 before (and while parked).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dkr_get_hud(rt: *const c_void, out: *mut hud::DkrHud) -> u32 {
    guard(0, || {
        let Some(r) = (unsafe { (rt as *const Runtime).as_ref() }) else { return 0 };
        let Some(o) = (unsafe { out.as_mut() }) else { return 0 };
        if o.size as usize != std::mem::size_of::<hud::DkrHud>() { return 0; }
        let Ok(sh) = r.shared.lock() else { return 0 };
        *o = sh.hud;
        sh.hud.valid
    })
}

/// Skate audio: starts SK8V's audio engine on the prepared cache (`cache_dir`: the folder holding `raw\audiofiles`,
/// from SK8V's tools/prepare-skate-audio.py). Returns 1 when configured (loading continues on the audio thread; see
/// `dkr_audio_status`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dkr_audio_configure(rt: *mut c_void, cache_dir: *const c_char, master_gain: f32) -> u32 {
    guard(0, || {
        let Some(r) = (unsafe { (rt as *const Runtime).as_ref() }) else { return 0 };
        if cache_dir.is_null() { return 0; }
        let dir = PathBuf::from(unsafe { CStr::from_ptr(cache_dir) }.to_string_lossy().into_owned());
        if !dir.join("raw").join("audiofiles").is_dir() {
            r.log.line(&format!("audio: {} is not a prepared Skate audio cache; Skate audio off", dir.display()));
            return 0;
        }
        let log = r.log.clone();
        let fwd: Arc<dyn Fn(&str) + Send + Sync> = Arc::new(move |m: &str| log.line(m));
        r.log.line(&format!("audio: configuring from {}", dir.display()));
        audio::configure(dir, master_gain, fwd) as u32
    })
}

/// Skate audio: up to `frames` mono samples (48 kHz) of emitter `id` (0 deck, 1 body, 2 head); `info` (optional) gets the
/// emitter's state with its position in game-world coordinates. Returns the frames copied (the rest of `out` is zero).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dkr_audio_pull(rt: *const c_void, id: u32, out: *mut f32, frames: u32, info: *mut SvAudioEmitter) -> u32 {
    guard(0, || {
        if unsafe { (rt as *const Runtime).as_ref() }.is_none() || out.is_null() { return 0; }
        let buf = unsafe { std::slice::from_raw_parts_mut(out, frames as usize) };
        match unsafe { info.as_mut() } {
            Some(i) => {
                let n = audio::pull(id as usize, buf, Some(&mut *i));
                if let Ok(o) = AUDIO_ORIGIN.lock() { i.position.x += o.x; i.position.y += o.y; i.position.z += o.z; }
                n
            }
            None => audio::pull(id as usize, buf, None),
        }
    })
}

/// Skate audio: pause (the game paused) or resume; while paused the rings are emptied and nothing is rendered.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dkr_audio_set_paused(rt: *const c_void, paused: u32) -> u32 {
    guard(0, || if unsafe { (rt as *const Runtime).as_ref() }.is_none() { 0 } else { audio::set_paused(paused != 0) as u32 })
}

/// Skate audio: the engine's state (off, loading, ready, error), voices, underruns and a message.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dkr_audio_status(rt: *const c_void, out: *mut SvAudioStatus) -> u32 {
    guard(0, || {
        let (Some(_), Some(o)) = (unsafe { (rt as *const Runtime).as_ref() }, unsafe { out.as_mut() }) else { return 0 };
        audio::status(o);
        1
    })
}

/// Stops the worker (it finishes with the session after the channel closes) and frees the handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dkr_destroy(rt: *mut c_void) {
    guard((), || {
        if !rt.is_null() {
            audio::shutdown();  // the audio engine is the process's; it stops with the runtime that configured it
            drop(unsafe { Box::from_raw(rt as *mut Runtime) });
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Probe-format OBJ (game world) -> host triangles, with the physical flag from each object's `sim=` word0.
    fn load_obj(path: &str) -> Vec<Tri> {
        let (mut v, mut out, mut physical) = (Vec::<Vec3>::new(), Vec::new(), true);
        for line in std::fs::read_to_string(path).expect("obj").lines() {
            if let Some(r) = line.strip_prefix("v ") {
                let p: Vec<f32> = r.split_whitespace().filter_map(|x| x.parse().ok()).collect();
                v.push(Vec3::new(p[0], p[1], p[2]));
            } else if let Some(r) = line.strip_prefix("f ") {
                let i: Vec<usize> = r.split_whitespace().filter_map(|x| x.split('/').next()?.parse().ok()).collect();
                if i.len() == 3 && i.iter().all(|&k| k >= 1 && k <= v.len()) {
                    out.push(Tri { p: [v[i[0] - 1], v[i[1] - 1], v[i[2] - 1]], physical });
                }
            } else if let Some(s) = line.strip_prefix("# shapeFlags=").and_then(|l| l.find("sim=").map(|k| &l[k + 4..k + 12])) {
                physical = u32::from_str_radix(s, 16).map(|w| w != 0).unwrap_or(true);
            }
        }
        out
    }

    /// Offline step cost at a fixed spot (investigation §7.2m). Not a regular test: it needs lab data.
    /// DKR_OBJ=probe.obj[;more.obj] DKR_ASSETS=skate-data/assets DKR_SPAWN=x,y,z,heading DKR_VARIANTS=base,norails,...
    /// cargo test --release -- --ignored step_cost --nocapture
    #[test]
    #[ignore]
    fn step_cost() {
        let env = |k: &str| std::env::var(k).unwrap_or_else(|_| panic!("set {k}"));
        let mut all: Vec<Tri> = env("DKR_OBJ").split(';').flat_map(load_obj).collect();
        let s: Vec<f32> = env("DKR_SPAWN").split(',').map(|x| x.trim().parse().unwrap()).collect();
        let (spawn, heading) = (Vec3::new(s[0], s[1], s[2]), s[3]);
        let ticks: u64 = std::env::var("DKR_TICKS").ok().and_then(|t| t.parse().ok()).unwrap_or(900);
        all.retain(|t| { let c = (t.p[0] + t.p[1] + t.p[2]) / 3.0; Vec3::new(c.x - spawn.x, c.y - spawn.y, 0.0).length() <= 40.0 });
        let root = PathBuf::from(env("DKR_ASSETS"));
        for variant in env("DKR_VARIANTS").split(',') {
            let within = |r: f32| move |t: &Tri| { let c = (t.p[0] + t.p[1] + t.p[2]) / 3.0; Vec3::new(c.x - spawn.x, c.y - spawn.y, 0.0).length() <= r };
            let tris: Vec<Tri> = match variant {
                "r20" => all.iter().copied().filter(within(20.0)).collect(),
                "r10" => all.iter().copied().filter(within(10.0)).collect(),
                "nodown" => all.iter().copied().filter(|t| (t.p[1] - t.p[0]).cross(t.p[2] - t.p[0]).normalize_or_zero().z > -0.95).collect(),
                _ => all.clone(),
            };
            let origin = Vec3::new(spawn.x, spawn.y, 0.0);
            // 0.18.0: the default is the cleaned collision (`prepare`); "rails-only" and "unsorted" are 0.17.0's.
            let (st, tags, mut sr) = match variant {
                "rails-only" | "unsorted" => { let (t, r) = prepare_rails_only(&tris, origin, variant != "unsorted"); let n = t.len(); (t, vec![0; n], r) }
                _ => { let p = prepare(&tris, origin, &mut Default::default()); (p.tris, p.tags, p.rails) }
            };
            if variant == "norails" { sr.clear(); }
            let (nt, nr) = (st.len(), sr.len());
            let spawn_s = coords::to_skate(spawn - origin).to_array();
            let mut sess = Session::new_tagged(&root, st, tags, sr, spawn_s, coords::skate_heading(heading)).expect("session");
            sess.activate(spawn_s, coords::skate_heading(heading)).expect("activate");
            let mut ap = Autopilot::new(heading, spawn);
            let mut costs = Vec::new();
            for _ in 0..ticks {
                let p = sess.pose();
                let at = coords::from_skate(p.root.w_axis.truncate()) + origin;
                let c = ap.controls(p.tick, at, coords::from_skate(p.velocity), &p.state);
                let t0 = std::time::Instant::now();
                sess.tick(c).expect("tick");
                costs.push(t0.elapsed().as_secs_f64() * 1000.0);
            }
            let p = sess.pose();
            let at = coords::from_skate(p.root.w_axis.truncate()) + origin;
            let v = coords::from_skate(p.velocity);
            let mut tail = costs[costs.len() / 3..].to_vec();
            tail.sort_by(f64::total_cmp);
            let q = |f: f64| tail[((tail.len() - 1) as f64 * f) as usize];
            println!("{variant:>8}: {nt:6} tris {nr:5} rails | last 2/3 of {ticks} ticks: mean {:.2} p50 {:.2} p90 {:.2} max {:.2} ms | end ({:.2}, {:.2}, {:.2}) {} {:.2} m/s",
                     tail.iter().sum::<f64>() / tail.len() as f64, q(0.5), q(0.9), q(1.0), at.x, at.y, at.z, p.state,
                     Vec3::new(v.x, v.y, 0.0).length());
        }
    }

    /// Autopilot on a synthetic straight road, 20 m wide and 400 m long, rising at `grade` (run 4's climb: 14.4%).
    /// DKR_ASSETS=skate-data/assets cargo test --release -- --ignored autopilot_road --nocapture
    #[test]
    #[ignore]
    fn autopilot_road() {
        let root = PathBuf::from(std::env::var("DKR_ASSETS").expect("set DKR_ASSETS"));
        // (grade, walls): walls 1.5 m high across the road at y = -15 and y = 40 (both faces), to make it turn.
        for (grade, walls) in [(0.0f32, false), (0.144, false), (0.0, true)] {
            let z = |y: f32| grade * y.max(0.0);
            let mut tris = Vec::new();
            for iy in -10..200 {
                for ix in -5..5 {
                    let (x0, y0, x1, y1) = (ix as f32 * 2.0, iy as f32 * 2.0, ix as f32 * 2.0 + 2.0, iy as f32 * 2.0 + 2.0);
                    let p = |x: f32, y: f32| Vec3::new(x, y, z(y));
                    tris.push(Tri { p: [p(x0, y0), p(x1, y0), p(x1, y1)], physical: true });
                    tris.push(Tri { p: [p(x0, y0), p(x1, y1), p(x0, y1)], physical: true });
                }
            }
            if walls {
                for wy in [-15.0f32, 40.0] {
                    let q = |x: f32, h: f32| Vec3::new(x, wy, h);
                    let (a, b, c, d) = (q(-10.0, 0.0), q(10.0, 0.0), q(10.0, 1.5), q(-10.0, 1.5));
                    for t in [[a, b, c], [a, c, d], [a, c, b], [a, d, c]] { tris.push(Tri { p: t, physical: true }); }
                }
            }
            let (spawn, heading) = (Vec3::new(0.0, 0.0, 0.1), 0.0f32);  // heading 0 = +Y = uphill
            let origin = Vec3::ZERO;
            let p = prepare(&tris, origin, &mut Default::default());
            let spawn_s = coords::to_skate(spawn).to_array();
            let mut sess = Session::new_tagged(&root, p.tris, p.tags, p.rails, spawn_s, coords::skate_heading(heading)).expect("session");
            sess.activate(spawn_s, coords::skate_heading(heading)).expect("activate");
            let mut ap = Autopilot::new(heading, spawn);
            let (mut turns, mut max_y, mut ollies, mut last_state) = (Vec::new(), 0f32, 0u32, String::new());
            for _ in 0..2400 {
                let p = sess.pose();
                let at = coords::from_skate(p.root.w_axis.truncate());
                let c = ap.controls(p.tick, at, coords::from_skate(p.velocity), &p.state);
                if let Some(h) = ap.turn_to.take() {
                    sess.activate(coords::to_skate(at + Vec3::new(0.0, 0.0, 0.05)).to_array(), coords::skate_heading(h)).expect("turn");
                    turns.push(format!("t={:.1}s ({:.1}, {:.1}, {:.2}) {} -> {h:.0}", p.tick as f32 / 60.0, at.x, at.y, at.z, p.state));
                }
                sess.tick(c).expect("tick");
                let p = sess.pose();
                if p.state == "KnownAir" && last_state != "KnownAir" { ollies += 1; }
                last_state = p.state.clone();
                max_y = max_y.max(coords::from_skate(p.root.w_axis.truncate()).y);
            }
            let p = sess.pose();
            let at = coords::from_skate(p.root.w_axis.truncate());
            let v = coords::from_skate(p.velocity);
            println!("grade {:4.1}%{}: 40 s, furthest y {max_y:.1} m, end ({:.1}, {:.1}, {:.2}) {} {:.2} m/s, {ollies} ollies, turns: {turns:?}",
                     grade * 100.0, if walls { " with walls" } else { "" }, at.x, at.y, at.z, p.state, Vec3::new(v.x, v.y, 0.0).length());
        }
    }

    /// The autopilot's flips on a flat synthetic road: each pop's trick name, and how far the board's up axis turns from
    /// vertical in the air (a kickflip rolls the board over: close to 180 degrees). DKR_FLIP_X=+1/-1 picks the flick side.
    /// DKR_ASSETS=skate-data/assets cargo test --release -- --ignored autopilot_flips --nocapture
    #[test]
    #[ignore]
    fn autopilot_flips() {
        let root = PathBuf::from(std::env::var("DKR_ASSETS").expect("set DKR_ASSETS"));
        let mut tris = Vec::new();
        for iy in -10..200 {
            for ix in -5..5 {
                let (x0, y0, x1, y1) = (ix as f32 * 2.0, iy as f32 * 2.0, ix as f32 * 2.0 + 2.0, iy as f32 * 2.0 + 2.0);
                let p = |x: f32, y: f32| Vec3::new(x, y, 0.0);
                tris.push(Tri { p: [p(x0, y0), p(x1, y0), p(x1, y1)], physical: true });
                tris.push(Tri { p: [p(x0, y0), p(x1, y1), p(x0, y1)], physical: true });
            }
        }
        let (spawn, heading) = (Vec3::new(0.0, 0.0, 0.1), 0.0f32);
        let p = prepare(&tris, Vec3::ZERO, &mut Default::default());
        let spawn_s = coords::to_skate(spawn).to_array();
        let mut sess = Session::new_tagged(&root, p.tris, p.tags, p.rails, spawn_s, coords::skate_heading(heading)).expect("session");
        sess.activate(spawn_s, coords::skate_heading(heading)).expect("activate");
        let mut ap = Autopilot::new(heading, spawn);
        let board = sess.pose().names.iter().position(|n| n.eq_ignore_ascii_case("SKATEBOARD_ROOT")).expect("board joint");
        let (mut air, mut tilt, mut trick, mut pops) = (false, 0f32, String::new(), Vec::new());
        for _ in 0..1800 {
            let p = sess.pose();
            let at = coords::from_skate(p.root.w_axis.truncate());
            let c = ap.controls(p.tick, at, coords::from_skate(p.velocity), &p.state);
            if let Some(h) = ap.turn_to.take() { sess.activate(coords::to_skate(at + Vec3::new(0.0, 0.0, 0.05)).to_array(), coords::skate_heading(h)).expect("turn"); }
            sess.tick(c).expect("tick");
            let p = sess.pose();
            let sc = sess.score();
            if !sc.trick_name.is_empty() { trick = sc.trick_name.clone(); }
            let (_, q) = retarget::board_transform(p.root, p.bones[board]);
            let up = bevy_math::Quat::from_xyzw(q[0], q[1], q[2], q[3]) * Vec3::Z;
            let in_air = p.state.contains("Air");
            if in_air { tilt = tilt.max(up.z.clamp(-1.0, 1.0).acos().to_degrees()); }
            if air && !in_air { pops.push(format!("{trick} (board up axis up to {tilt:.0} deg from vertical)")); tilt = 0.0; }
            air = in_air;
        }
        let p = sess.pose();
        match p.camera {
            Some((cp, cb, fov)) => {
                let (cam, root) = (coords::from_skate(cp), coords::from_skate(p.root.w_axis.truncate()));
                let fwd = (coords::basis() * cb.z_axis).normalize();
                println!("camera: {:.2} m from the skater root, {:.2} m above it, look direction ({:.2}, {:.2}, {:.2}) {:.0} deg off the skater, fov {fov:.1}",
                         (cam - root).length(), cam.z - root.z, fwd.x, fwd.y, fwd.z, fwd.angle_between((root - cam).normalize()).to_degrees());
            }
            None => println!("camera: none"),
        }
        println!("flick x {}: {} pops", ap.flip_x, pops.len());
        for (i, p) in pops.iter().enumerate() { println!("  pop {}: {p}", i + 1); }
    }

    /// P0-1 (first in-game ride, 2026-11-16): each d-pad direction, alone and with LB, while rolling on a flat road. With
    /// Deckrunner's settings every gesture plays; without them the first press must still stop the engine, which shows
    /// the test reaches the gesture path. Ported from SK8V's `the_d_pad_plays_a_gesture_without_failing`.
    /// DKR_ASSETS=skate-data/assets cargo test --release -- --ignored d_pad_gestures --nocapture
    #[test]
    #[ignore]
    fn d_pad_gestures() {
        let root = PathBuf::from(std::env::var("DKR_ASSETS").expect("set DKR_ASSETS"));
        let mut tris = Vec::new();
        for iy in -10..120 {
            for ix in -5..5 {
                let (x0, y0, x1, y1) = (ix as f32 * 2.0, iy as f32 * 2.0, ix as f32 * 2.0 + 2.0, iy as f32 * 2.0 + 2.0);
                let p = |x: f32, y: f32| Vec3::new(x, y, 0.0);
                tris.push(Tri { p: [p(x0, y0), p(x1, y0), p(x1, y1)], physical: true });
                tris.push(Tri { p: [p(x0, y0), p(x1, y1), p(x0, y1)], physical: true });
            }
        }
        let spawn_s = coords::to_skate(Vec3::new(0.0, 0.0, 0.1)).to_array();
        let run = |configured: bool| -> Result<(u64, Vec<String>), String> {
            let p = prepare(&tris, Vec3::ZERO, &mut Default::default());
            let mut s = Session::new_tagged(&root, p.tris, p.tags, p.rails, spawn_s, coords::skate_heading(0.0))?;
            if configured { configure(&mut s); }
            s.activate(spawn_s, coords::skate_heading(0.0))?;
            let mut seen = Vec::new();
            // XInput: d-pad up 0x1, down 0x2, left 0x4, right 0x8; LB 0x100. A push (A, 0x1000) first to get rolling.
            let presses: [u16; 9] = [0x1000, 0x1, 0x2, 0x4, 0x8, 0x101, 0x102, 0x104, 0x108];
            for (i, b) in presses.iter().enumerate() {
                for t in 0..60u32 {
                    let buttons = if t < 20 { *b } else { 0 };
                    s.tick(Controls { buttons, ..Controls::default() }).map_err(|e| format!("press {i} ({b:#06x}): {e}"))?;
                    let st = s.pose().state;
                    if seen.last() != Some(&st) { seen.push(st); }
                }
            }
            Ok((s.pose().tick, seen))
        };
        let bare = run(false);
        println!("without Deckrunner's settings: {:?}", bare.as_ref().map(|r| r.0));
        assert!(bare.as_ref().is_err_and(|e| e.contains("gesture")), "the d-pad should reach the gesture path: {bare:?}");
        let (ticks, states) = run(true).expect("with SK8V's gestures every d-pad press plays");
        println!("with gestures {GESTURES:?}: {ticks} ticks, states {states:?}");
    }

    /// The HUD (ABI 13): the autopilot's flips on a flat road, with the plugin's HUD values each tick. Skate's own words
    /// come from the converted data (private/hud/runtime/trickdisplay.json); prints what the HUD would show.
    /// DKR_ASSETS=skate-data/assets cargo test --release -- --ignored hud_during_flips --nocapture
    #[test]
    #[ignore]
    fn hud_during_flips() {
        let root = PathBuf::from(std::env::var("DKR_ASSETS").expect("set DKR_ASSETS"));
        let names = hud::Names::load(&root).expect("Skate's strings");
        println!("{} strings", names.len());
        let mut tris = Vec::new();
        for iy in -10..200 {
            for ix in -5..5 {
                let (x0, y0, x1, y1) = (ix as f32 * 2.0, iy as f32 * 2.0, ix as f32 * 2.0 + 2.0, iy as f32 * 2.0 + 2.0);
                let p = |x: f32, y: f32| Vec3::new(x, y, 0.0);
                tris.push(Tri { p: [p(x0, y0), p(x1, y0), p(x1, y1)], physical: true });
                tris.push(Tri { p: [p(x0, y0), p(x1, y1), p(x0, y1)], physical: true });
            }
        }
        let (spawn, heading) = (Vec3::new(0.0, 0.0, 0.1), 0.0f32);
        let p = prepare(&tris, Vec3::ZERO, &mut Default::default());
        let spawn_s = coords::to_skate(spawn).to_array();
        let mut sess = Session::new_tagged(&root, p.tris, p.tags, p.rails, spawn_s, coords::skate_heading(heading)).expect("session");
        sess.activate(spawn_s, coords::skate_heading(heading)).expect("activate");
        let mut ap = Autopilot::new(heading, spawn);
        let mut tracker = hud::Tracker::default();
        let text = |b: &[u8]| String::from_utf8_lossy(&b[..b.iter().position(|&c| c == 0).unwrap_or(b.len())]).into_owned();
        let (mut shown, mut best_line, mut best_seq, mut last) = (Vec::<String>::new(), 0f32, 0f32, hud::DkrHud::default());
        for _ in 0..1800 {
            let p = sess.pose();
            let at = coords::from_skate(p.root.w_axis.truncate());
            let c = ap.controls(p.tick, at, coords::from_skate(p.velocity), &p.state);
            if let Some(h) = ap.turn_to.take() { sess.activate(coords::to_skate(at + Vec3::new(0.0, 0.0, 0.05)).to_array(), coords::skate_heading(h)).expect("turn"); }
            sess.tick(c).expect("tick");
            let h = tracker.update(&sess, sess.pose().tick, &names);
            let t = text(&h.trick);
            if h.new_tricks != last.new_tricks || h.upgraded_tricks != last.upgraded_tricks {
                shown.push(format!("{t} ({}; trick {:.0}, line {:.0} x{:.1}, {:.1} s left; {})", text(&h.landing_text), h.sequence_score,
                                   h.line_score, h.multiplier, h.line_seconds, text(&h.stance_text)));
            }
            (best_line, best_seq, last) = (best_line.max(h.line_score), best_seq.max(h.sequence_score), h);
        }
        for s in &shown { println!("  {s}"); }
        println!("{} tricks announced, {} upgraded, {} lines closed; best trick score {best_seq:.0}, best line {best_line:.0}",
                 last.new_tricks, last.upgraded_tricks, last.closed_lines);
        assert!(names.len() > 500, "Skate's trick names are loaded");
        assert!(last.new_tricks > 0 && !shown.is_empty(), "the autopilot's flips are announced");
        assert!(shown.iter().any(|s| s.contains("flip") || s.contains("Ollie")), "a trick shows by its name: {shown:?}");
        assert!(best_seq > 0.0 || best_line > 0.0, "the flips score");
    }

    /// #2: through the C interface as the plugin uses it: `dkr_preload` loads Skate and stays parked (never active, no
    /// steps), then the first `dkr_start` 30 m away resumes without a reload. Prints how long that start takes.
    /// DKR_ASSETS=skate-data/assets cargo test --release -- --ignored preload_then_start --nocapture
    #[test]
    #[ignore]
    fn preload_then_start() {
        let assets = std::ffi::CString::new(std::env::var("DKR_ASSETS").expect("set DKR_ASSETS")).unwrap();
        let out_dir = std::env::temp_dir().join(format!("dkr-preload-{}", std::process::id()));
        let out = std::ffi::CString::new(out_dir.to_string_lossy().into_owned()).unwrap();
        let mut f = Vec::new();
        for iy in -40..40 {
            for ix in -40..40 {
                let (x0, y0, x1, y1) = (ix as f32 * 2.0, iy as f32 * 2.0, ix as f32 * 2.0 + 2.0, iy as f32 * 2.0 + 2.0);
                f.extend_from_slice(&[x0, y0, 0.0, x1, y0, 0.0, x1, y1, 0.0, x0, y0, 0.0, x1, y1, 0.0, x0, y1, 0.0]);
            }
        }
        let n = (f.len() / 9) as u32;
        let state = |rt: *mut c_void| { let mut s = DkrState { size: std::mem::size_of::<DkrState>() as u32, ..Default::default() };
                                        unsafe { dkr_get_state(rt, &mut s) }; s };
        unsafe {
            let rt = dkr_create(assets.as_ptr(), out.as_ptr());
            assert!(!rt.is_null());
            let t0 = std::time::Instant::now();
            assert_eq!(dkr_preload(rt, f.as_ptr(), std::ptr::null(), n, [0.0f32, 0.0, 0.0].as_ptr(), 0.0), 1);
            let log = out_dir.join("deckrunner-runtime.log");
            while !std::fs::read_to_string(&log).unwrap_or_default().contains("preloaded in") {
                assert!(t0.elapsed().as_secs() < 120, "preload never finished");
                assert_ne!(state(rt).status, STATUS_ACTIVE, "a preload is never active");
                std::thread::sleep(std::time::Duration::from_millis(50));
            }
            println!("preloaded in {} ms; status {}", t0.elapsed().as_millis(), state(rt).status);
            assert_eq!(state(rt).status, STATUS_IDLE);
            let t1 = std::time::Instant::now();
            assert_eq!(dkr_start(rt, f.as_ptr(), std::ptr::null(), n, [30.0f32, 20.0, 0.0].as_ptr(), 90.0, 0), 1);
            let pad = DkrPad::default();
            loop {
                let s = state(rt);
                if s.status == STATUS_ACTIVE && s.tick > 0 {
                    println!("first start: riding state {:?} at tick {} after {} ms, at ({:.1}, {:.1}, {:.1})",
                             CStr::from_ptr(s.state.as_ptr() as *const c_char), s.tick, t1.elapsed().as_millis(), s.pos[0], s.pos[1], s.pos[2]);
                    assert!((s.pos[0] - 30.0).abs() < 1.0 && (s.pos[1] - 20.0).abs() < 1.0, "the skater is at the start, not the preload");
                    break;
                }
                assert!(t1.elapsed().as_secs() < 10, "the first start took too long (status {})", s.status);
                dkr_step(rt, 1.0 / 60.0, &pad);
                std::thread::sleep(std::time::Duration::from_millis(16));
            }
            let text = std::fs::read_to_string(&log).unwrap_or_default();
            assert!(text.contains("session resumed in"), "the first start resumed the preloaded session");
            for l in text.lines().filter(|l| l.contains("preload") || l.contains("resumed")) { println!("log: {l}"); }
            dkr_destroy(rt);
        }
        let _ = std::fs::remove_dir_all(&out_dir);
    }

    /// #2: a player's start (no autopilot) stands the skater at the spawn, off the board with it in hand; Y gets on.
    /// DKR_ASSETS=skate-data/assets cargo test --release -- --ignored starts_on_foot --nocapture
    #[test]
    #[ignore]
    fn starts_on_foot() {
        let root = PathBuf::from(std::env::var("DKR_ASSETS").expect("set DKR_ASSETS"));
        let mut tris = Vec::new();
        for iy in -10..10 {
            for ix in -10..10 {
                let (x0, y0, x1, y1) = (ix as f32 * 2.0, iy as f32 * 2.0, ix as f32 * 2.0 + 2.0, iy as f32 * 2.0 + 2.0);
                let p = |x: f32, y: f32| Vec3::new(x, y, 0.0);
                tris.push(Tri { p: [p(x0, y0), p(x1, y0), p(x1, y1)], physical: true });
                tris.push(Tri { p: [p(x0, y0), p(x1, y1), p(x0, y1)], physical: true });
            }
        }
        let spawn = Vec3::new(1.0, 2.0, 0.0);
        let spawn_s = coords::to_skate(spawn).to_array();
        let p = prepare(&tris, Vec3::ZERO, &mut Default::default());
        let mut s = Session::new_tagged(&root, p.tris, p.tags, p.rails, spawn_s, coords::skate_heading(0.0)).expect("session");
        configure(&mut s);
        enter(&mut s, spawn_s, 30.0, false).expect("enter on foot");
        for _ in 0..30 { s.tick(Controls::default()).expect("stand"); }
        let v = s.lifecycle_view();
        let at = coords::from_skate(s.pose().root.w_axis.truncate());
        println!("after 30 ticks standing: state {} ({}), board possession {}, at ({:.2}, {:.2}, {:.2})",
                 v.state, s.pose().state, v.board_possession, at.x, at.y, at.z);
        assert_eq!(v.state, 500, "BipedGround: standing, off the board");
        assert_eq!(v.board_possession, 1, "the board is held");
        assert!((at - spawn).truncate().length() < 0.5, "stands where it started");
        let mut on = None;
        for t in 0..240u32 {
            let buttons = if t < 10 { 0x8000 } else { 0 };  // Y: Skate's board action
            s.tick(Controls { buttons, ..Controls::default() }).expect("mount");
            if s.lifecycle_view().state < 500 { on = Some(t); break; }
        }
        println!("Y: on the board after {on:?} ticks, state {}", s.pose().state);
        assert!(on.is_some(), "Y gets on the board");
    }

    /// #6: a session runs in each of Skate 3's physics modes, set before `activate` as the worker does, and switches mode
    /// mid-ride (a resume with a changed deckrunner.ini). Pushes off on a flat road and rides for three seconds per mode.
    /// DKR_ASSETS=skate-data/assets cargo test --release -- --ignored difficulty_modes --nocapture
    #[test]
    #[ignore]
    fn difficulty_modes() {
        let root = PathBuf::from(std::env::var("DKR_ASSETS").expect("set DKR_ASSETS"));
        let mut tris = Vec::new();
        for iy in -10..120 {
            for ix in -5..5 {
                let (x0, y0, x1, y1) = (ix as f32 * 2.0, iy as f32 * 2.0, ix as f32 * 2.0 + 2.0, iy as f32 * 2.0 + 2.0);
                let p = |x: f32, y: f32| Vec3::new(x, y, 0.0);
                tris.push(Tri { p: [p(x0, y0), p(x1, y0), p(x1, y1)], physical: true });
                tris.push(Tri { p: [p(x0, y0), p(x1, y1), p(x0, y1)], physical: true });
            }
        }
        let spawn_s = coords::to_skate(Vec3::new(0.0, 0.0, 0.1)).to_array();
        let ride = |s: &mut Session, ticks: u32| -> Result<Vec<String>, String> {
            let mut seen = Vec::new();
            for t in 0..ticks {
                let buttons = if t % 60 < 15 { A } else { 0 };  // push now and then
                s.tick(Controls { buttons, ..Controls::default() })?;
                let st = s.pose().state;
                if seen.last() != Some(&st) { seen.push(st); }
            }
            Ok(seen)
        };
        let mut last = None;
        for (index, name) in DIFFICULTIES.iter().enumerate() {
            let p = prepare(&tris, Vec3::ZERO, &mut Default::default());
            let mut s = Session::new_tagged(&root, p.tris, p.tags, p.rails, spawn_s, coords::skate_heading(0.0)).expect("session");
            configure(&mut s);
            assert!(s.set_difficulty(index as u32));
            s.activate(spawn_s, coords::skate_heading(0.0)).expect("activate");
            let seen = ride(&mut s, 180).unwrap_or_else(|e| panic!("{name}: {e}"));
            assert_eq!(s.difficulty(), index as u32, "{name}");
            println!("{name}: {} ticks, states {seen:?}", s.pose().tick);
            last = Some(s);
        }
        let mut s = last.unwrap();
        assert!(!s.set_difficulty(DIFFICULTIES.len() as u32), "an unknown mode is refused");
        for index in [0u32, 2, 1] {
            assert!(s.set_difficulty(index));
            let seen = ride(&mut s, 120).unwrap_or_else(|e| panic!("live switch to {}: {e}", DIFFICULTIES[index as usize]));
            assert_eq!(s.difficulty(), index);
            println!("live {}: states {seen:?}", DIFFICULTIES[index as usize]);
        }
    }

    /// Skate 3's session marker (fifth test: LB + d-pad down played a taunt and nothing came back): roll on a flat road,
    /// place a marker with LB + d-pad down, ride on, then hold LB + d-pad up. Run twice: input through `Session::tick`
    /// (Deckrunner's worker), and through `collect` + `advance` as SK8V's worker feeds it (which also runs the marker's
    /// clock). Prints where the skater was at the press, before the hold, and after it, and the states seen.
    /// DKR_ASSETS=skate-data/assets cargo test --release -- --ignored session_marker --nocapture
    #[test]
    #[ignore]
    fn session_marker() {
        use skate_host::bridge::InputFrame;
        let root = PathBuf::from(std::env::var("DKR_ASSETS").expect("set DKR_ASSETS"));
        let mut tris = Vec::new();
        for iy in -10..120 {
            for ix in -5..5 {
                let (x0, y0, x1, y1) = (ix as f32 * 2.0, iy as f32 * 2.0, ix as f32 * 2.0 + 2.0, iy as f32 * 2.0 + 2.0);
                let p = |x: f32, y: f32| Vec3::new(x, y, 0.0);
                tris.push(Tri { p: [p(x0, y0), p(x1, y0), p(x1, y1)], physical: true });
                tris.push(Tri { p: [p(x0, y0), p(x1, y1), p(x0, y1)], physical: true });
            }
        }
        let spawn_s = coords::to_skate(Vec3::new(0.0, 0.0, 0.1)).to_array();
        for collect in [false, true] {
            let p = prepare(&tris, Vec3::ZERO, &mut Default::default());
            let mut s = Session::new_tagged(&root, p.tris, p.tags, p.rails, spawn_s, coords::skate_heading(0.0)).expect("session");
            configure(&mut s);
            s.activate(spawn_s, coords::skate_heading(0.0)).expect("activate");
            let (mut seen, mut marks) = (Vec::<String>::new(), Vec::new());
            for t in 0..700u32 {
                // Push for 4 s; LB + d-pad down at 2.5 s; LB + d-pad up held from 8 s for 1.5 s.
                let mut buttons = if t < 240 && t % 30 < 4 { A } else { 0 };
                if (150..160).contains(&t) { buttons |= 0x102; }
                if (480..570).contains(&t) { buttons |= 0x101; }
                if collect {
                    s.collect(InputFrame::from_pad(buttons, [0, 0], [0, 0], [0, 0], t + 1), s.period());
                    s.advance().expect("advance");
                } else {
                    s.tick(Controls { buttons, ..Controls::default() }).expect("tick");
                }
                let pose = s.pose();
                let at = coords::from_skate(pose.root.w_axis.truncate());
                if seen.last() != Some(&pose.state) { seen.push(pose.state.clone()); }
                if [150, 479, 600, 699].contains(&t) { marks.push(format!("t {t}: ({:.2}, {:.2})", at.x, at.y)); }
            }
            println!("{}: {}; states {seen:?}", if collect { "collect + advance" } else { "tick" }, marks.join(", "));
        }
    }

    /// The session marker's HUD (ABI 14, the engine's overlay patch 0050): roll, place a marker with LB + d-pad down,
    /// tap LB + d-pad up (too short to return), then hold it until the return. Prints the HUD's marker values as they
    /// change. DKR_ASSETS=skate-data/assets cargo test --release -- --ignored marker_hud --nocapture
    #[test]
    #[ignore]
    fn marker_hud() {
        use skate_host::bridge::InputFrame;
        let root = PathBuf::from(std::env::var("DKR_ASSETS").expect("set DKR_ASSETS"));
        let names = hud::Names::load(&root).expect("Skate's strings");
        let mut tris = Vec::new();
        for iy in -10..120 {
            for ix in -5..5 {
                let (x0, y0, x1, y1) = (ix as f32 * 2.0, iy as f32 * 2.0, ix as f32 * 2.0 + 2.0, iy as f32 * 2.0 + 2.0);
                let p = |x: f32, y: f32| Vec3::new(x, y, 0.0);
                tris.push(Tri { p: [p(x0, y0), p(x1, y0), p(x1, y1)], physical: true });
                tris.push(Tri { p: [p(x0, y0), p(x1, y1), p(x0, y1)], physical: true });
            }
        }
        let spawn_s = coords::to_skate(Vec3::new(0.0, 0.0, 0.1)).to_array();
        let p = prepare(&tris, Vec3::ZERO, &mut Default::default());
        let mut s = Session::new_tagged(&root, p.tris, p.tags, p.rails, spawn_s, coords::skate_heading(0.0)).expect("session");
        configure(&mut s);
        s.activate(spawn_s, coords::skate_heading(0.0)).expect("activate");
        let mut tracker = hud::Tracker::default();
        let text = |b: &[u8]| String::from_utf8_lossy(&b[..b.iter().position(|&c| c == 0).unwrap_or(b.len())]).into_owned();
        let (mut last, mut most, mut changes) = (hud::DkrHud::default(), 0f32, Vec::new());
        for t in 0..700u32 {
            // Push for 4 s; LB + d-pad down at 2.5 s; a 3-tick tap of LB + d-pad up at 6.7 s; held from 8 s for 1.5 s.
            let mut buttons = if t < 240 && t % 30 < 4 { A } else { 0 };
            if (150..160).contains(&t) { buttons |= LB | DPAD_DOWN; }
            if (400..403).contains(&t) || (480..570).contains(&t) { buttons |= LB | DPAD_UP; }
            s.collect(InputFrame::from_pad(buttons, [0, 0], [0, 0], [0, 0], t + 1), s.period());
            s.advance().expect("advance");
            let h = tracker.update(&s, s.pose().tick, &names);
            most = most.max(h.marker_progress);
            let counts = |h: &hud::DkrHud| (h.markers_placed, h.markers_refused, h.marker_returns, h.marker_short_holds);
            if counts(&h) != counts(&last) || (h.marker_progress > 0.) != (last.marker_progress > 0.) {
                changes.push(format!("t {t}: placed {}, refused {}, returned {}, short holds {}; hold {:.2}, can return {}",
                                     h.markers_placed, h.markers_refused, h.marker_returns, h.marker_short_holds, h.marker_progress,
                                     h.marker_can_return));
            }
            last = h;
        }
        for c in &changes { println!("  {c}"); }
        println!("words: {:?}, {:?}, {:?}", text(&last.marker_placed_text), text(&last.marker_return_text), text(&last.hold_text));
        assert_eq!(last.markers_placed, 1, "LB + d-pad down placed the marker");
        assert_eq!(last.marker_short_holds, 1, "the tap was too short to return");
        assert_eq!(last.marker_returns, 1, "the hold returned");
        assert!(most > 0.9, "the hold ran to its end: {most}");
        assert!(!text(&last.marker_placed_text).is_empty() && !text(&last.hold_text).is_empty());
    }

    /// What SK8V's collision cleanup (world::build_area_reusing) changes in Cyberpunk collision, which Deckrunner
    /// handed to Skate with rails only up to 0.17.0 (`prepare_rails_only`); also times a rebuild with the reuse cache. Not a regular test: it needs lab OBJ dumps.
    /// DKR_OBJ=fall.obj[;more.obj] DKR_AT=x,y cargo test --release -- --ignored sk8v_cleanup_census --nocapture
    #[test]
    #[ignore]
    fn sk8v_cleanup_census() {
        use SkateVRuntime::{curbs, grid::XyGrid, plies};
        let all: Vec<Tri> = std::env::var("DKR_OBJ").expect("set DKR_OBJ").split(';').flat_map(load_obj).collect();
        let at: Vec<f32> = std::env::var("DKR_AT").expect("set DKR_AT").split(',').map(|x| x.trim().parse().unwrap()).collect();
        let origin = Vec3::new(at[0], at[1], 0.0);
        let near: Vec<Tri> = all.into_iter().filter(|t| { let c = (t.p[0] + t.p[1] + t.p[2]) / 3.0; Vec3::new(c.x - origin.x, c.y - origin.y, 0.0).length() <= 40.0 }).collect();
        let local: Vec<[Vec3; 3]> = blend(&near).into_iter().map(|t| t.map(|v| v - origin)).collect();
        let t0 = std::time::Instant::now();
        let (rails_now, census_now) = SkateVRuntime::rails::find_metres(&local);
        let rails_ms = t0.elapsed().as_millis();
        let grid = XyGrid::triangles(curbs::CELL, &local);
        let t1 = std::time::Instant::now();
        let caps = plies::abutting_caps(&local);
        let interior = plies::interior(&local, &grid);
        let solids = plies::buried_faces(&local);
        let bev = curbs::bevels_with_sources(&local, &grid);
        let filters_ms = t1.elapsed().as_millis();
        let t2 = std::time::Instant::now();
        let area = SkateVRuntime::world::build_area_tagged(origin, local.iter().map(|t| (t.map(|v| v.to_array()), 0u32)));
        let full_ms = t2.elapsed().as_millis();
        let c = &bev.census;
        println!("{} host triangles within 40 m -> {} after Deckrunner's blend", near.len(), local.len());
        println!("rails today: {} (lips {}, runs {}, duplicates {}) in {rails_ms} ms", rails_now.len(), census_now.lips, census_now.runs, census_now.duplicates);
        println!("SK8V filters ({filters_ms} ms): abutting end caps {}, interior ply faces {}, buried faces {}; curb edges {}: {} ramp-toe bevels \
                  ({} bevel triangles, {} risers buried under them), {} sloped detail kept, {} real risers kept",
                 caps.len(), interior.len(), solids.len(), c.edges, c.bevels, bev.triangles.len(), bev.buried.len(), c.slopes, c.curbs);
        let no_grind = area.tags.iter().filter(|t| **t & skate_host::bridge::NO_GRIND_TAG != 0).count();
        println!("world::build_area_tagged: {} triangles ({no_grind} no-grind bevels), {} rails, {full_ms} ms", area.triangles.len(), area.rails.len());
        // The removed faces, by size and facing: end caps are small and vertical.
        let mut by_kind = std::collections::BTreeMap::<&str, (usize, f32)>::new();
        for (name, idx) in [("caps", &caps), ("interior", &interior), ("solids", &solids), ("buried by bevels", &bev.buried)] {
            for &i in idx.iter() {
                let t = local[i];
                let n = (t[1] - t[0]).cross(t[2] - t[0]);
                let e = by_kind.entry(name).or_default();
                e.0 += (n.normalize_or_zero().z.abs() < 0.3) as usize;
                e.1 += n.length() * 0.5;
            }
        }
        for (k, (vertical, area_m2)) in by_kind { println!("  {k}: {vertical} near-vertical, {area_m2:.1} m2 in all"); }
        // The worker's own path: a build, then rebuilds 10 and 25 m on with the same cache, as the skater travels (the
        // plugin gathers 40 m around each centre; the dump only reaches 40 m around its own, so later builds see less).
        let mut cache = SkateVRuntime::reuse::AreaCache::default();
        for shift in [0.0f32, 0.0, 10.0, 25.0] {
            let c = origin + Vec3::new(shift, 0.0, 0.0);
            let around: Vec<Tri> = near.iter().copied().filter(|t| { let m = (t.p[0] + t.p[1] + t.p[2]) / 3.0; Vec3::new(m.x - c.x, m.y - c.y, 0.0).length() <= 40.0 }).collect();
            let t = std::time::Instant::now();
            let p = prepare(&around, origin, &mut cache);
            println!("prepare at +{shift} m: {} host -> {} triangles, {} rails in {} ms; {}", around.len(), p.tris.len(), p.rails.len(), t.elapsed().as_millis(), p.note);
        }
    }

    /// The faces straight above and below given points, before and after `prepare` (SK8V's cleanup), with their
    /// facing and tags: what Skate had where a skater fell through (P2-12). Not a regular test: it needs a lab dump.
    /// DKR_OBJ=fall.obj DKR_AT=x,y (the collision centre) DKR_COLUMNS="x,y,zlo,zhi;..." cargo test --release -- --ignored column_census --nocapture
    #[test]
    #[ignore]
    fn column_census() {
        let all: Vec<Tri> = std::env::var("DKR_OBJ").expect("set DKR_OBJ").split(';').flat_map(load_obj).collect();
        let at: Vec<f32> = std::env::var("DKR_AT").expect("set DKR_AT").split(',').map(|x| x.trim().parse().unwrap()).collect();
        let origin = Vec3::new(at[0], at[1], 0.0);
        let near: Vec<Tri> = all.into_iter().filter(|t| { let c = (t.p[0] + t.p[1] + t.p[2]) / 3.0; Vec3::new(c.x - origin.x, c.y - origin.y, 0.0).length() <= 40.0 }).collect();
        let host: Vec<([Vec3; 3], u32)> = near.iter().map(|t| (t.p, if t.physical { 0 } else { 1 })).collect();
        let blended: Vec<([Vec3; 3], u32)> = blend(&near).into_iter().map(|t| (t, 0)).collect();
        let p = prepare(&near, origin, &mut SkateVRuntime::reuse::AreaCache::default());
        let skate: Vec<([Vec3; 3], u32)> = p.tris.iter().zip(&p.tags)
            .map(|(t, &tag)| (t.map(|v| coords::from_skate(Vec3::from_array(v)) + origin), tag)).collect();
        println!("{} host, {} after the blend, {} after the cleanup; {}", host.len(), blended.len(), skate.len(), p.note);
        let column = |set: &[([Vec3; 3], u32)], x: f32, y: f32, zlo: f32, zhi: f32| -> Vec<(f32, f32, f32, u32)> {
            let mut out = Vec::new();
            for (t, tag) in set {
                let [a, b, c] = *t;
                let d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
                if d.abs() < 1e-9 { continue; }
                let l1 = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / d;
                let l2 = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / d;
                let l3 = 1.0 - l1 - l2;
                if l1.min(l2).min(l3) < -1e-5 { continue; }
                let z = l1 * a.z + l2 * b.z + l3 * c.z;
                if z < zlo || z > zhi { continue; }
                let n = (b - a).cross(c - a);
                out.push((z, n.normalize_or_zero().z, n.length() * 0.5, *tag));
            }
            out.sort_by(|p, q| q.0.total_cmp(&p.0));
            out
        };
        for spec in std::env::var("DKR_COLUMNS").expect("set DKR_COLUMNS").split(';') {
            let v: Vec<f32> = spec.split(',').map(|x| x.trim().parse().unwrap()).collect();
            println!("\ncolumn at ({}, {}), z {}..{}", v[0], v[1], v[2], v[3]);
            for (name, set) in [("host (tag 1 = detail)", &host), ("after the blend", &blended), ("given to Skate (tags)", &skate)] {
                let faces = column(set, v[0], v[1], v[2], v[3]);
                println!("  {name}: {}", if faces.is_empty() { "nothing".into() } else {
                    faces.iter().map(|(z, nz, a, tag)| format!("z {z:.2} nz {nz:+.2} {a:.1} m2{}", if *tag != 0 { format!(" tag {tag:#x}") } else { String::new() }))
                        .collect::<Vec<_>>().join("; ") });
            }
        }
    }

    /// The rails SK8V's finder makes inside a box, in world coordinates: each polyline's points, slope per segment and
    /// turn per joint, and every rail end in the box with the rails passing near it (where a grind could carry on).
    /// Not a regular test: it needs a lab dump or capture.
    /// DKR_OBJ=capture.obj DKR_AT=x,y (the collision centre) DKR_BOX="x0,x1,y0,y1,z0,z1" cargo test --release -- --ignored rail_census --nocapture
    #[test]
    #[ignore]
    fn rail_census() {
        let all: Vec<Tri> = std::env::var("DKR_OBJ").expect("set DKR_OBJ").split(';').flat_map(load_obj).collect();
        let at: Vec<f32> = std::env::var("DKR_AT").expect("set DKR_AT").split(',').map(|x| x.trim().parse().unwrap()).collect();
        let b: Vec<f32> = std::env::var("DKR_BOX").expect("set DKR_BOX").split(',').map(|x| x.trim().parse().unwrap()).collect();
        let origin = Vec3::new(at[0], at[1], 0.0);
        let near: Vec<Tri> = all.into_iter().filter(|t| { let c = (t.p[0] + t.p[1] + t.p[2]) / 3.0; Vec3::new(c.x - origin.x, c.y - origin.y, 0.0).length() <= 40.0 }).collect();
        // The rails Skate gets (`prepare`); DKR_RAILS_OLD=1 for 0.19.0's.
        let p = prepare_with(&near, origin, &mut Default::default(), if std::env::var("DKR_RAILS_OLD").is_ok() { 19 } else { 21 });
        let census = &p.note;
        let local = &p.tris;
        let rails: Vec<Vec<Vec3>> = p.rails.iter().map(|r| r.iter().map(|q| coords::from_skate(Vec3::from_array(*q)) + origin).collect()).collect();
        let inside = |p: Vec3| p.x >= b[0] && p.x <= b[1] && p.y >= b[2] && p.y <= b[3] && p.z >= b[4] && p.z <= b[5];
        let seg_in = |p: Vec3, q: Vec3| (0..=10).any(|k| inside(p.lerp(q, k as f32 / 10.0)));
        println!("{} host -> {} given to Skate; {} rails in all ({census})", near.len(), local.len(), rails.len());
        let slope = |p: Vec3, q: Vec3| { let d = q - p; d.z.atan2(Vec3::new(d.x, d.y, 0.0).length()).to_degrees() };
        let picked: Vec<usize> = (0..rails.len()).filter(|&i| rails[i].windows(2).any(|w| seg_in(w[0], w[1]))).collect();
        for &i in &picked {
            let r = &rails[i];
            let len: f32 = r.windows(2).map(|w| w[0].distance(w[1])).sum();
            println!("\nrail {i}: {} points, {len:.2} m", r.len());
            for (k, p) in r.iter().enumerate() {
                let seg = if k + 1 < r.len() { format!(" -> {:.2} m at {:+.1} deg", p.distance(r[k + 1]), slope(*p, r[k + 1])) } else { String::new() };
                let turn = if k > 0 && k + 1 < r.len() {
                    let (u, v) = ((*p - r[k - 1]).normalize_or_zero(), (r[k + 1] - *p).normalize_or_zero());
                    format!(" turn {:.1} deg", u.dot(v).clamp(-1.0, 1.0).acos().to_degrees())
                } else { String::new() };
                println!("  ({:.3}, {:.3}, {:.3}){turn}{seg}", p.x, p.y, p.z);
            }
        }
        println!("\nrail ends in the box, and the rails passing within 1 m of them:");
        for &i in &picked {
            let r = &rails[i];
            for (end, toward) in [(r[0], r[1]), (r[r.len() - 1], r[r.len() - 2])] {
                if !inside(end) { continue; }
                let out = (end - toward).normalize_or_zero();
                let mut found = Vec::new();
                for (j, s) in rails.iter().enumerate() {
                    if j == i { continue; }
                    for w in s.windows(2) {
                        let ab = w[1] - w[0];
                        let t = ((end - w[0]).dot(ab) / ab.length_squared().max(1e-12)).clamp(0.0, 1.0);
                        let q = w[0] + ab * t;
                        let d = end.distance(q);
                        if d <= 1.0 {
                            let dir = ab.normalize_or_zero();
                            found.push(format!("rail {j} {d:.2} m away at ({:.2}, {:.2}, {:.2}), {:.0} deg from this rail's heading off its end",
                                               q.x, q.y, q.z, out.dot(dir).abs().clamp(0.0, 1.0).acos().to_degrees()));
                            break;
                        }
                    }
                }
                println!("  rail {i} ends at ({:.2}, {:.2}, {:.2}) heading ({:+.2}, {:+.2}, {:+.2}): {}", end.x, end.y, end.z, out.x, out.y, out.z,
                         if found.is_empty() { "nothing".into() } else { found.join("; ") });
            }
        }
    }

    /// What the collision passes cost per build: `prepare` as 0.19.0, 0.20.0 and 0.21.0 built it, over a full 40 m
    /// gather. DKR_OBJ=fall.obj[;more.obj] cargo test --release -- --ignored prepare_cost --nocapture
    #[test]
    #[ignore]
    fn prepare_cost() {
        for path in std::env::var("DKR_OBJ").expect("set DKR_OBJ").split(';') {
            let all = load_obj(path);
            let c = all.iter().fold(Vec3::ZERO, |a, t| a + t.p[0]) / all.len() as f32;
            let origin = Vec3::new(c.x, c.y, 0.0);
            let near: Vec<Tri> = all.into_iter().filter(|t| { let m = (t.p[0] + t.p[1] + t.p[2]) / 3.0; Vec3::new(m.x - origin.x, m.y - origin.y, 0.0).length() <= 40.0 }).collect();
            for level in [19, 20, 21, 19, 20, 21] {
                let t0 = std::time::Instant::now();
                let p = prepare_with(&near, origin, &mut Default::default(), level);
                let ms = t0.elapsed().as_millis();
                println!("{path}: {} host, as 0.{level}.0: {} triangles, {} rails in {ms} ms; {}", near.len(), p.tris.len(), p.rails.len(), p.note);
            }
        }
    }

    /// The faces inside a box before and after `prepare` (Deckrunner's blend, then SK8V's cleanup), in world
    /// coordinates: which host faces Skate never sees and which it gets that the host did not have (fillets).
    /// DKR_OBJ=capture.obj DKR_AT=x,y DKR_BOX="x0,x1,y0,y1,z0,z1" cargo test --release -- --ignored faces_census --nocapture
    #[test]
    #[ignore]
    fn faces_census() {
        let all: Vec<Tri> = std::env::var("DKR_OBJ").expect("set DKR_OBJ").split(';').flat_map(load_obj).collect();
        let at: Vec<f32> = std::env::var("DKR_AT").expect("set DKR_AT").split(',').map(|x| x.trim().parse().unwrap()).collect();
        let b: Vec<f32> = std::env::var("DKR_BOX").expect("set DKR_BOX").split(',').map(|x| x.trim().parse().unwrap()).collect();
        let origin = Vec3::new(at[0], at[1], 0.0);
        let near: Vec<Tri> = all.into_iter().filter(|t| { let c = (t.p[0] + t.p[1] + t.p[2]) / 3.0; Vec3::new(c.x - origin.x, c.y - origin.y, 0.0).length() <= 40.0 }).collect();
        let p = prepare(&near, origin, &mut SkateVRuntime::reuse::AreaCache::default());
        let meets = |t: &[Vec3; 3]| {
            let (lo, hi) = t.iter().fold((Vec3::splat(f32::MAX), Vec3::splat(f32::MIN)), |(a, c), v| (a.min(*v), c.max(*v)));
            !(hi.x < b[0] || lo.x > b[1] || hi.y < b[2] || lo.y > b[3] || hi.z < b[4] || lo.z > b[5])
        };
        let key = |t: &[Vec3; 3]| { let mut k: Vec<[i32; 3]> = t.iter().map(|v| (*v * 1000.0).round().as_ivec3().to_array()).collect(); k.sort(); k };
        let host: Vec<[Vec3; 3]> = near.iter().map(|t| t.p).filter(|t| meets(t)).collect();
        let skate: Vec<([Vec3; 3], u32)> = p.tris.iter().zip(&p.tags).map(|(t, &g)| (t.map(|v| coords::from_skate(Vec3::from_array(v)) + origin), g)).filter(|(t, _)| meets(t)).collect();
        let skate_keys: std::collections::HashSet<Vec<[i32; 3]>> = skate.iter().map(|(t, _)| key(t)).collect();
        let host_keys: std::collections::HashSet<Vec<[i32; 3]>> = host.iter().map(key).collect();
        let show = |t: &[Vec3; 3]| { let n = (t[1] - t[0]).cross(t[2] - t[0]).normalize_or_zero();
            format!("n ({:+.2} {:+.2} {:+.2}) {}", n.x, n.y, n.z, t.iter().map(|v| format!("({:.3} {:.3} {:.3})", v.x, v.y, v.z)).collect::<Vec<_>>().join(" ")) };
        println!("{} host faces in the box, {} given to Skate", host.len(), skate.len());
        for t in &host { if !skate_keys.contains(&key(t)) { println!("  not given to Skate: {}", show(t)); } }
        for (t, g) in &skate { if !host_keys.contains(&key(t)) { println!("  added (tag {g:#x}): {}", show(t)); } }
    }

    /// P1-11 (0.21.0): a scripted run over a speed bump in a capture: push along a line from the spawn up to a speed,
    /// then roll over the line `DKR_AT_M` metres along it. Per run: the speed 1 m before, the lowest speed and largest
    /// upward speed from 0.5 m before to 1.5 m after, and whether the skater wiped out within 1 s. Each `DKR_LEVELS`
    /// entry is the collision as that plugin version built it (20 or 21).
    /// DKR_OBJ=capture.obj DKR_ASSETS=... DKR_SPAWN=x,y,z,heading DKR_AT_M=d DKR_SPEEDS=4,6,8 [DKR_HEADINGS=...]
    /// [DKR_LEVELS=20,21] cargo test --release -- --ignored bump_ride --nocapture
    #[test]
    #[ignore]
    fn bump_ride() {
        let env = |k: &str| std::env::var(k).unwrap_or_else(|_| panic!("set {k}"));
        let nums = |v: String| v.split(',').map(|x| x.trim().parse::<f32>().unwrap()).collect::<Vec<f32>>();
        let s = nums(env("DKR_SPAWN"));
        let (spawn, heading0) = (Vec3::new(s[0], s[1], s[2]), s[3]);
        let mut all: Vec<Tri> = env("DKR_OBJ").split(';').flat_map(load_obj).collect();
        all.retain(|t| { let c = (t.p[0] + t.p[1] + t.p[2]) / 3.0; Vec3::new(c.x - spawn.x, c.y - spawn.y, 0.0).length() <= 40.0 });
        let root = PathBuf::from(env("DKR_ASSETS"));
        let at_m: f32 = env("DKR_AT_M").parse().unwrap();
        let headings = std::env::var("DKR_HEADINGS").map(nums).unwrap_or(vec![heading0]);
        let levels: Vec<u32> = std::env::var("DKR_LEVELS").unwrap_or_else(|_| "20,21".into()).split(',').map(|x| x.trim().parse().unwrap()).collect();
        let origin = Vec3::new(spawn.x, spawn.y, 0.0);
        let spawn_s = coords::to_skate(spawn - origin).to_array();
        let first = prepare_with(&all, origin, &mut Default::default(), levels[0]);
        let mut sess = Session::new_tagged(&root, first.tris, first.tags, first.rails, spawn_s, coords::skate_heading(heading0)).expect("session");
        configure(&mut sess);
        let builder = sess.collision_builder();
        for &level in &levels {
            let p = prepare_with(&all, origin, &mut Default::default(), level);
            println!("as 0.{level}.0: {}", p.note);
            sess.install_collision(builder.build_tagged(p.tris, p.tags, p.rails).expect("build")).expect("install");
            for &target in &nums(env("DKR_SPEEDS")) {
                for &heading in &headings {
                    let dir = Vec3::new(-heading.to_radians().sin(), heading.to_radians().cos(), 0.0);
                    let right = Vec3::new(dir.y, -dir.x, 0.0);
                    sess.activate(spawn_s, coords::skate_heading(heading)).expect("activate");
                    let start = sess.pose().tick;
                    let (mut before, mut low, mut kick, mut crossed_at, mut wiped) = (f32::NAN, f32::MAX, 0f32, None::<u64>, false);
                    for _ in 0..1200 {
                        let pose = sess.pose();
                        let at = coords::from_skate(pose.root.w_axis.truncate()) + origin;
                        let v = coords::from_skate(pose.velocity);
                        let speed = Vec3::new(v.x, v.y, 0.0).length();
                        let along = (at - spawn).dot(dir);
                        let mut c = Controls::default();
                        if pose.state == "PhysicsGround" {
                            if (pose.tick - start) % 30 < 4 && speed < target && along < at_m - 3.0 { c.buttons |= A; }
                            if speed > 0.5 {
                                let e = (at - spawn).dot(right);
                                c.left = [((-(0.6 * e + 0.9 * v.dot(right))).clamp(-1.0, 1.0) * 32767.0) as i16, 0];
                            }
                        }
                        sess.tick(c).expect("tick");
                        let pose = sess.pose();
                        let at = coords::from_skate(pose.root.w_axis.truncate()) + origin;
                        let v = coords::from_skate(pose.velocity);
                        let speed = Vec3::new(v.x, v.y, 0.0).length();
                        let along = (at - spawn).dot(dir);
                        if before.is_nan() && along >= at_m - 1.0 { before = speed; }
                        if (at_m - 0.5..=at_m + 1.5).contains(&along) { low = low.min(speed); kick = kick.max(v.z); }
                        if std::env::var("DKR_VERBOSE").is_ok() && (at_m - 1.5..=at_m + 2.0).contains(&along) {
                            let b = pose.names.iter().position(|n| n.eq_ignore_ascii_case("SKATEBOARD_ROOT")).map(|i| retarget::board_transform(pose.root, pose.bones[i]));
                            let (bz, pitch) = b.map(|(bp, bq)| { let q = bevy_math::Quat::from_xyzw(bq[0], bq[1], bq[2], bq[3]); let f = q * Vec3::Y; (bp.z, f.z.asin().to_degrees()) }).unwrap_or((f32::NAN, f32::NAN));
                            println!("    t {} {:<14} along {:+.2} z {:.3} board z {:.3} pitch {:+.1} v ({:.2} {:.2} {:+.2}) {:.2} m/s", pose.tick, pose.state, along - at_m, at.z, bz, pitch, v.x, v.y, v.z, speed);
                        }
                        if crossed_at.is_none() && along >= at_m { crossed_at = Some(pose.tick); }
                        if crossed_at.is_some_and(|t| pose.tick <= t + 60) && pose.state.starts_with("Wipeout") { wiped = true; }
                        if crossed_at.is_some_and(|t| pose.tick > t + 60) { break; }
                    }
                    match crossed_at {
                        Some(_) => println!("  0.{level}.0 target {target} heading {heading}: {before:.2} m/s before, lowest {low:.2} ({:+.0}%), up to {kick:+.2} m/s upward{}",
                                            100.0 * (low / before - 1.0), if wiped { "; WIPED OUT" } else { "" }),
                        None => println!("  0.{level}.0 target {target} heading {heading}: never reached the bump"),
                    }
                }
            }
        }
    }

    /// A scripted run at a rail in a lab dump or capture: push along a line from the spawn, ollie once the skater is
    /// `ollie` m along it, then let go; prints the air, the grind and what follows, for each ollie distance and rail
    /// variant. Not a regular test: it needs lab data.
    /// DKR_OBJ=capture.obj DKR_ASSETS=... DKR_SPAWN=x,y,z,heading DKR_OLLIE=d1,d2,... [DKR_SPEED=4.5] [DKR_RAILS=base,...]
    /// [DKR_STEER=x,y] (left stick held from the ollie on) cargo test --release -- --ignored rail_ride --nocapture
    #[test]
    #[ignore]
    fn rail_ride() {
        let env = |k: &str| std::env::var(k).unwrap_or_else(|_| panic!("set {k}"));
        let nums = |k: &str| env(k).split(',').map(|x| x.trim().parse::<f32>().unwrap()).collect::<Vec<f32>>();
        let s = nums("DKR_SPAWN");
        let (spawn, heading) = (Vec3::new(s[0], s[1], s[2]), s[3]);
        let mut all: Vec<Tri> = env("DKR_OBJ").split(';').flat_map(load_obj).collect();
        all.retain(|t| { let c = (t.p[0] + t.p[1] + t.p[2]) / 3.0; Vec3::new(c.x - spawn.x, c.y - spawn.y, 0.0).length() <= 40.0 });
        let root = PathBuf::from(env("DKR_ASSETS"));
        let target: f32 = std::env::var("DKR_SPEED").ok().and_then(|v| v.parse().ok()).unwrap_or(4.5);
        let steer: Option<[i16; 2]> = std::env::var("DKR_STEER").ok().map(|v| { let p: Vec<i16> = v.split(',').map(|x| x.trim().parse().unwrap()).collect(); [p[0], p[1]] });
        let origin = Vec3::new(spawn.x, spawn.y, 0.0);
        let variants = std::env::var("DKR_RAILS").unwrap_or_else(|_| "base".into());
        // Variants: "base" (today's collision), "old" (0.19.0's: no tall end caps, no rail join), "cut" (see below).
        let base = prepare(&all, origin, &mut Default::default());
        let old = prepare_with(&all, origin, &mut Default::default(), 19);
        println!("0.20.0: {}\n0.19.0: {}", base.note, old.note);
        let spawn_s = coords::to_skate(spawn - origin).to_array();
        let mut sess = Session::new_tagged(&root, base.tris.clone(), base.tags.clone(), base.rails.clone(), spawn_s, coords::skate_heading(heading)).expect("session");
        let builder = sess.collision_builder();
        // Headings to try (DKR_HEADINGS), each from the same spawn; the spawn's own heading if unset.
        let headings: Vec<f32> = std::env::var("DKR_HEADINGS").ok().map(|v| v.split(',').map(|x| x.trim().parse().unwrap()).collect()).unwrap_or(vec![heading]);
        for variant in variants.split(',') {
            let rails = if variant.starts_with("old") { old.rails.clone() } else { base.rails.clone() };
            // "cut": the base rails over collision without the up-facing faces in DKR_CUT ("x0,x1,y0,y1,z0,z1"), a
            // diagnostic for whether a bail comes from the rail or from the board meeting those faces.
            let (tris, tags) = if variant.starts_with("cut") {
                let b = nums("DKR_CUT");
                let cut: Vec<Tri> = all.iter().copied().filter(|t| {
                    let c = (t.p[0] + t.p[1] + t.p[2]) / 3.0;
                    let up = (t.p[1] - t.p[0]).cross(t.p[2] - t.p[0]).normalize_or_zero().z > 0.7;
                    !(up && c.x >= b[0] && c.x <= b[1] && c.y >= b[2] && c.y <= b[3] && c.z >= b[4] && c.z <= b[5])
                }).collect();
                println!("{variant}: {} faces cut", all.len() - cut.len());
                let p = prepare(&cut, origin, &mut Default::default());
                (p.tris, p.tags)
            } else if variant.starts_with("old") { (old.tris.clone(), old.tags.clone()) } else { (base.tris.clone(), base.tags.clone()) };
            sess.install_collision(builder.build_tagged(tris, tags, rails).expect("build")).expect("install");
            for &heading in &headings {
            let dir = Vec3::new(-heading.to_radians().sin(), heading.to_radians().cos(), 0.0);
            let right = Vec3::new(dir.y, -dir.x, 0.0);
            for &ollie in &nums("DKR_OLLIE") {
                sess.activate(spawn_s, coords::skate_heading(heading)).expect("activate");
                let run_start = sess.pose().tick;
                let (mut popped, mut pop_tick, mut grind_from, mut grind_to, mut last) = (false, 0u64, None::<u64>, None::<u64>, String::new());
                let (mut first_at, mut last_at, mut bailed, mut last_grind) = (None::<Vec3>, Vec3::ZERO, false, 0u64);
                let mut lines = Vec::new();
                let mut trick = String::new();
                for _ in 0..900 {
                    let pose = sess.pose();
                    let at = coords::from_skate(pose.root.w_axis.truncate()) + origin;
                    let v = coords::from_skate(pose.velocity);
                    let speed = Vec3::new(v.x, v.y, 0.0).length();
                    let ground = pose.state == "PhysicsGround";
                    let mut c = Controls::default();
                    let along = (at - spawn).dot(dir);
                    if !popped {
                        if ground && (pose.tick - run_start) % 30 < 4 && speed < target && along < ollie - 1.5 { c.buttons |= A; }
                        if ground && speed > 0.5 {
                            let e = (at - spawn).dot(right);
                            c.left = [((-(0.6 * e + 0.9 * v.dot(right))).clamp(-1.0, 1.0) * 32767.0) as i16, 0];
                        }
                        if ground && along >= ollie { popped = true; pop_tick = pose.tick; }
                    }
                    if popped {
                        let d = pose.tick - pop_tick;
                        if d < 8 { c.right = [0, -32767]; } else if d < 14 { c.right = [0, 32767]; }
                        if let Some(st) = steer { c.left = st; }
                    }
                    sess.tick(c).expect("tick");
                    let pose = sess.pose();
                    let sc = sess.score();
                    if !sc.trick_name.is_empty() { trick = sc.trick_name.clone(); }
                    let at = coords::from_skate(pose.root.w_axis.truncate()) + origin;
                    let v = coords::from_skate(pose.velocity);
                    let grinding = pose.state.starts_with("Grind");
                    if grinding && grind_from.is_none() { grind_from = Some(pose.tick); }
                    if grinding && grind_to.is_none() { first_at.get_or_insert(at); last_at = at; last_grind = pose.tick; }
                    if grind_from.is_some() && (pose.state.starts_with("Wipeout") || pose.state.starts_with("Biped")) { bailed = true; }
                    let down = pose.state.starts_with("Wipeout") || pose.state.starts_with("Biped");
                    if !grinding && grind_from.is_some() && grind_to.is_none() && (down || pose.tick > last_grind + 10) { grind_to = Some(last_grind + 1); }
                    let changed = pose.state != last;
                    let window = (popped || std::env::var("DKR_VERBOSE").is_ok()) && grind_to.is_none_or(|e| pose.tick < e + 40);
                    if window && (changed || pose.tick % 3 == 0) {
                        lines.push(format!("    {:5} {:<18} {:<26} ({:9.2} {:8.2} {:7.2}) v ({:5.2} {:5.2} {:5.2}) {:4.2} m/s",
                                           pose.tick, pose.state, trick, at.x, at.y, at.z, v.x, v.y, v.z, v.length()));
                    }
                    last = pose.state.clone();
                    if grind_to.is_some_and(|e| pose.tick > e + 40) { break; }
                }
                let summary = match (grind_from, grind_to) {
                    (Some(a), Some(b)) => {
                        let f = first_at.unwrap_or_default();
                        format!("ground {} ticks from ({:.2}, {:.2}, {:.2}) to ({:.2}, {:.2}, {:.2}), then {}{}", b - a, f.x, f.y, f.z, last_at.x, last_at.y, last_at.z, last,
                                if bailed { "; BAILED" } else { "" })
                    }
                    (Some(a), None) => format!("still grinding after {} ticks", 900 - a),
                    _ => "no grind".into(),
                };
                println!("{variant} heading {heading} ollie at {ollie} m: {summary}");
                if std::env::var("DKR_VERBOSE").is_ok() { for l in lines.iter().take(160) { println!("{l}"); } }
            }
            }
        }
    }

    /// The blend's stair-ramp rule on a dump, 0.18.0's version (`blend_cells`) against today's (`blend`): how many sloped
    /// physical faces each drops, and how many of those have detail geometry within 0.5 m under their centroid (real
    /// steps) rather than only somewhere in the same 1 m cells (P0-12: H10's walkway ramps were dropped beside a 23 m
    /// tall detail mesh). Not a regular test: it needs lab dumps.
    /// DKR_OBJ=fall.obj[;more.obj] cargo test --release -- --ignored blend_census --nocapture
    #[test]
    #[ignore]
    fn blend_census() {
        let key = |p: &[Vec3; 3]| { let mut k = [0u32; 9]; for (i, v) in p.iter().enumerate() { k[i * 3] = v.x.to_bits(); k[i * 3 + 1] = v.y.to_bits(); k[i * 3 + 2] = v.z.to_bits(); } k };
        for path in std::env::var("DKR_OBJ").expect("set DKR_OBJ").split(';') {
            let all = load_obj(path);
            let detail: Vec<&Tri> = all.iter().filter(|t| !t.physical).collect();
            let mut grid: HashMap<(i32, i32), Vec<usize>> = HashMap::new();
            for (i, t) in detail.iter().enumerate() {
                let (lo, hi) = t.p.iter().fold((Vec3::splat(f32::MAX), Vec3::splat(f32::MIN)), |(a, b), v| (a.min(*v), b.max(*v)));
                for x in lo.x.floor() as i32..=hi.x.floor() as i32 { for y in lo.y.floor() as i32..=hi.y.floor() as i32 { grid.entry((x, y)).or_default().push(i); } }
            }
            println!("{path}: {} host triangles", all.len());
            for (name, out) in [("0.18.0 (cells)", blend_cells(&all)), ("0.19.0 (under the ramp)", blend(&all))] {
                let kept: std::collections::HashSet<[u32; 9]> = out.iter().map(key).collect();
                let (mut dropped, mut stepped, mut area_dropped, mut area_stepped) = (0, 0, 0.0f32, 0.0f32);
                for t in all.iter().filter(|t| t.physical && !kept.contains(&key(&t.p))) {
                    let n = (t.p[1] - t.p[0]).cross(t.p[2] - t.p[0]);
                    if n.length() <= 1e-6 { continue; }
                    let c = (t.p[0] + t.p[1] + t.p[2]) / 3.0;
                    dropped += 1;
                    area_dropped += n.length() * 0.5;
                    let under = grid.get(&(c.x.floor() as i32, c.y.floor() as i32)).is_some_and(|ids| ids.iter().any(|&i| {
                        z_at(&detail[i].p, c.x, c.y).is_some_and(|z| z <= c.z + 0.2 && z >= c.z - 0.5)
                    }));
                    if under { stepped += 1; area_stepped += n.length() * 0.5; }
                }
                println!("  {name}: drops {dropped} sloped physical faces ({area_dropped:.0} m2); {stepped} of them ({area_stepped:.0} m2) have detail geometry 0.5 m or less under their centroid, {} ({:.0} m2) have none", dropped - stepped, area_dropped - area_stepped);
            }
        }
    }

    #[test]
    fn state_layout_is_stable() {
        assert_eq!(std::mem::size_of::<super::DkrPad>(), 16);
        assert_eq!(std::mem::size_of::<super::DkrState>(), 368);
        assert_eq!(std::mem::offset_of!(super::DkrState, state), 44);
        assert_eq!(std::mem::offset_of!(super::DkrState, step_ms_avg), 140);
        assert_eq!(std::mem::offset_of!(super::DkrState, collision_center), 164);
        assert_eq!(std::mem::offset_of!(super::DkrState, error), 188);
        assert_eq!(std::mem::offset_of!(super::DkrState, sub_tick), 348);
        assert_eq!(std::mem::offset_of!(super::DkrState, steps), 360);
    }
}
