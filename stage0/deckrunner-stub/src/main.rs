//! Stage 0 headless host for SK8V's Skate session.
//!
//! - Builds synthetic collision in a Z-up, metre world (the frame GTA and Cyberpunk share): a floor,
//!   a ledge and a kicker ramp. It finds grind lips with SK8V's own `rails::find_metres`, converts
//!   everything with SK8V's `coords`, and hands it to the unchanged `skate_host::bridge::Session`.
//! - Drives the session with a reactive scripted XInput controller at Skate's own period.
//! - Records every tick's state and pose. `trace.jsonl` holds the per-tick summary; `pose.bin` holds
//!   the raw joint matrices, little-endian f32 of root and bones per tick; `pose.json` holds the
//!   joint names and layout.
//!
//! Usage: deckrunner-stub <converted skate-data/assets> <out dir> [seconds]
use bevy_math::{Mat4, Vec3};
use skate_host::bridge::{Controls, Session};
use std::io::Write;
use std::path::PathBuf;
use SkateVRuntime::{coords, rails};

const A: u16 = 0x1000;
const B: u16 = 0x2000;
const X: u16 = 0x4000;

fn quad(a: Vec3, b: Vec3, c: Vec3, d: Vec3) -> [[Vec3; 3]; 2] {
    // a,b,c,d counterclockwise seen from the side the face points to
    [[a, b, c], [a, c, d]]
}

/// Axis-aligned box (Z-up), all faces outward.
fn cuboid(min: Vec3, max: Vec3) -> Vec<[Vec3; 3]> {
    let (x0, y0, z0, x1, y1, z1) = (min.x, min.y, min.z, max.x, max.y, max.z);
    let v = |x, y, z| Vec3::new(x, y, z);
    let mut t = Vec::new();
    t.extend(quad(v(x0, y0, z1), v(x1, y0, z1), v(x1, y1, z1), v(x0, y1, z1))); // top +Z
    t.extend(quad(v(x0, y0, z0), v(x1, y0, z0), v(x1, y0, z1), v(x0, y0, z1))); // -Y
    t.extend(quad(v(x1, y1, z0), v(x0, y1, z0), v(x0, y1, z1), v(x1, y1, z1))); // +Y
    t.extend(quad(v(x0, y1, z0), v(x0, y0, z0), v(x0, y0, z1), v(x0, y1, z1))); // -X
    t.extend(quad(v(x1, y0, z0), v(x1, y1, z0), v(x1, y1, z1), v(x1, y0, z1))); // +X
    t
}

/// A kicker rising along +Y from z=0 at y0 to height h at y0+len, width w centred on x=0.
fn kicker(y0: f32, len: f32, h: f32, w: f32) -> Vec<[Vec3; 3]> {
    let (xa, xb, y1) = (-w / 2.0, w / 2.0, y0 + len);
    let v = |x, y, z| Vec3::new(x, y, z);
    let mut t = Vec::new();
    t.extend(quad(v(xa, y0, 0.0), v(xb, y0, 0.0), v(xb, y1, h), v(xa, y1, h))); // slope
    t.extend(quad(v(xb, y1, 0.0), v(xa, y1, 0.0), v(xa, y1, h), v(xb, y1, h))); // back +Y
    t.push([v(xa, y0, 0.0), v(xa, y1, h), v(xa, y1, 0.0)]); // side -X
    t.push([v(xb, y0, 0.0), v(xb, y1, 0.0), v(xb, y1, h)]); // side +X
    t
}

fn world() -> Vec<[Vec3; 3]> {
    let h = 150.0;
    let mut t = Vec::new();
    t.extend(quad(Vec3::new(-h, -h, 0.0), Vec3::new(h, -h, 0.0), Vec3::new(h, h, 0.0), Vec3::new(-h, h, 0.0)));
    // A long low ledge centred on the riding line, 30..42 m ahead.
    t.extend(cuboid(Vec3::new(-0.25, 30.0, 0.0), Vec3::new(0.25, 42.0, 0.3)));
    // A kicker beyond it.
    t.extend(kicker(60.0, 3.0, 0.7, 3.0));
    t
}

#[derive(Clone, Copy, PartialEq, Debug)]
enum Phase {
    Settle,
    ProbePush { button: u16, until: u64, start_speed: f32 },
    Push,
    OllieFlat,
    ApproachLedge,
    Ride,
}

fn main() -> Result<(), String> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 {
        return Err("usage: deckrunner-stub <skate-data/assets> <out dir> [seconds]".into());
    }
    let root = PathBuf::from(&args[1]);
    let out = PathBuf::from(&args[2]);
    let seconds: f32 = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(30.0);
    // Left-stick steering sign and target line (x, m); the sign is checked empirically from the trace.
    let steer_sign: f32 = args.get(4).and_then(|s| s.parse().ok()).unwrap_or(1.0);
    let line_x: f32 = args.get(5).and_then(|s| s.parse().ok()).unwrap_or(0.0);
    std::fs::create_dir_all(&out).map_err(|e| e.to_string())?;

    // Collision, Z-up metres -> rails (SK8V's finder) -> Skate space (SK8V's axis map).
    let tris = world();
    let (rails_z, census) = rails::find_metres(&tris);
    eprintln!("synthetic world: {} triangles; rails: {} ({:?})", tris.len(), rails_z.len(), census);
    for (i, r) in rails_z.iter().enumerate() {
        let (a, b) = (r.first().unwrap(), r.last().unwrap());
        eprintln!("  rail {i}: {} pts ({:.2},{:.2},{:.2}) -> ({:.2},{:.2},{:.2})", r.len(), a.x, a.y, a.z, b.x, b.y, b.z);
    }
    let to_s = |p: Vec3| coords::to_skate(p).to_array();
    let skate_tris: Vec<[[f32; 3]; 3]> = tris.iter().map(|t| t.map(to_s)).collect();
    let tags = vec![0u32; skate_tris.len()];
    let skate_rails: Vec<Vec<[f32; 3]>> = rails_z.iter().map(|r| r.iter().map(|p| to_s(*p)).collect()).collect();
    let spawn = coords::to_skate(Vec3::new(0.0, 0.0, 0.0)).to_array();
    let heading = coords::skate_heading(0.0); // GTA/CP heading 0 = facing +Y

    let t0 = std::time::Instant::now();
    let mut s = Session::new_tagged(&root, skate_tris, tags, skate_rails, spawn, heading)?;
    let load_ms = t0.elapsed().as_millis();
    let first = s.activate(spawn, heading)?;
    eprintln!("session loaded in {load_ms} ms, period {:.5} s, {} joints, state {}", s.period(), first.names.len(), first.state);

    let mut trace = std::io::BufWriter::new(std::fs::File::create(out.join("trace.jsonl")).map_err(|e| e.to_string())?);
    let mut posebin = std::io::BufWriter::new(std::fs::File::create(out.join("pose.bin")).map_err(|e| e.to_string())?);
    let ticks = (seconds / s.period()).round() as u64;
    let mut phase = Phase::Settle;
    let mut push_button = 0u16;
    let mut ollie_at: Option<u64> = None;
    let mut ledge_ollie_done = false;
    let mut states: Vec<(u64, String)> = Vec::new();
    let mut max_height = 0f32;
    let mut max_speed = 0f32;
    let mut step_cost = 0f64;
    let mut names = first.names.clone();
    let mut tricks: Vec<(u64, String)> = Vec::new();
    let mut last_score = (0f32, 0f32, 0f32, 0f32);

    for tick in 0..ticks {
        let p = s.pose();
        let at = coords::from_skate(p.root.w_axis.truncate());
        let v = coords::from_skate(p.velocity);
        let speed = Vec3::new(v.x, v.y, 0.0).length();
        max_speed = max_speed.max(speed);
        max_height = max_height.max(at.z);
        if states.last().map(|(_, s)| s != &p.state).unwrap_or(true) {
            states.push((tick, p.state.clone()));
        }
        // Reactive scripted controller.
        let mut c = Controls::default();
        let tap = |period: u64, len: u64| tick % period < len;
        phase = match phase {
            Phase::Settle if tick >= 60 => Phase::ProbePush { button: A, until: tick + 150, start_speed: speed },
            Phase::ProbePush { button, until, start_speed } if tick >= until => {
                if speed > start_speed + 1.0 {
                    push_button = button;
                    eprintln!("push button: {button:#06x} (speed {start_speed:.2} -> {speed:.2} m/s)");
                    Phase::Push
                } else {
                    let next = match button { A => X, X => B, _ => 0 };
                    if next == 0 { return Err(format!("no face button pushed the board (speed {speed:.2})")); }
                    Phase::ProbePush { button: next, until: tick + 150, start_speed: speed }
                }
            }
            Phase::Push if speed > 5.5 || at.y > 14.0 => Phase::OllieFlat,
            Phase::OllieFlat if ollie_at.is_some_and(|t| tick > t + 90) => Phase::ApproachLedge,
            Phase::ApproachLedge if at.y > 45.0 => Phase::Ride,
            other => other,
        };
        match phase {
            Phase::ProbePush { button, .. } => if tap(30, 4) { c.buttons |= button },
            Phase::Push => if tap(30, 4) { c.buttons |= push_button },
            _ => {}
        }
        // Ollie: right stick pulled down, then flicked up (Flick-It).
        let mut ollie = |c: &mut Controls, start: u64| {
            let d = tick.saturating_sub(start);
            if d < 8 { c.right = [0, -32767] } else if d < 14 { c.right = [0, 32767] }
        };
        if phase == Phase::OllieFlat {
            let start = *ollie_at.get_or_insert(tick);
            ollie(&mut c, start);
        }
        if phase == Phase::ApproachLedge && !ledge_ollie_done && at.y > 30.0 - speed * 0.55 {
            ledge_ollie_done = true;
            ollie_at = Some(tick);
        }
        if phase == Phase::ApproachLedge && ledge_ollie_done {
            ollie(&mut c, ollie_at.unwrap());
        }
        if phase == Phase::Ride && tap(45, 4) && at.y < 55.0 { c.buttons |= push_button }
        // Hold the riding line: steer on lateral offset and lateral velocity while rolling on the ground.
        if speed > 0.5 && p.state == "PhysicsGround" {
            let err = line_x - at.x;
            let cmd = (steer_sign * (0.6 * err - 0.9 * v.x)).clamp(-1.0, 1.0);
            c.left = [(cmd * 32767.0) as i16, 0];
        }
        if at.y > 110.0 { break; }

        let step = std::time::Instant::now();
        s.tick(c)?;
        step_cost += step.elapsed().as_secs_f64();

        let p = s.pose();
        if p.names.len() != names.len() { names = p.names.clone(); }
        let at = coords::from_skate(p.root.w_axis.truncate());
        let (deck_p, deck_q) = s.deck();
        let deck = coords::from_skate(Vec3::from_array(deck_p));
        let sc = s.score();
        if !sc.trick_name.is_empty() && tricks.last().map(|(_, n): &(u64, String)| n != &sc.trick_name).unwrap_or(true) {
            tricks.push((p.tick, sc.trick_name.clone()));
        }
        last_score = (sc.line_score, sc.completed_lines, sc.multiplier, sc.sequence_score);
        let cam = p.camera.map(|(pos, _, fov)| { let g = coords::from_skate(pos); [g.x, g.y, g.z, fov] });
        writeln!(trace, "{}", serde_json::json!({
            "tick": p.tick, "state": p.state, "buttons": c.buttons, "right": c.right,
            "root": [at.x, at.y, at.z], "deck": [deck.x, deck.y, deck.z], "deck_q_skate": deck_q,
            "vel": coords::from_skate(p.velocity).to_array(), "camera": cam,
        })).map_err(|e| e.to_string())?;
        for m in std::iter::once(&p.root).chain(p.bones.iter()) {
            for f in Mat4::to_cols_array(m) { posebin.write_all(&f.to_le_bytes()).map_err(|e| e.to_string())?; }
        }
    }
    trace.flush().map_err(|e| e.to_string())?;
    posebin.flush().map_err(|e| e.to_string())?;
    std::fs::write(out.join("pose.json"), serde_json::to_string_pretty(&serde_json::json!({
        "frames": ticks, "matrices_per_frame": names.len() + 1, "layout": "per frame: root then bones, column-major 4x4 f32, Skate space (Y up); world joint = root * bone * basis(X,-Z,Y)",
        "names": names, "period": s.period(), "skate_space_to_z_up": "(x, y, z) -> (x, -z, y)",
        "rails_z_up": rails_z.iter().map(|r| r.iter().map(|p| p.to_array()).collect::<Vec<_>>()).collect::<Vec<_>>(),
    })).unwrap()).map_err(|e| e.to_string())?;

    println!("ticks {ticks}, mean step {:.3} ms (period {:.3} ms)", 1000.0 * step_cost / ticks as f64, 1000.0 * s.period());
    println!("max speed {max_speed:.2} m/s, max root height {max_height:.2} m, push button {push_button:#06x}");
    println!("tricks named by Skate's scorer:");
    for (t, n) in &tricks { println!("  {t:5} {n}"); }
    println!("final score view: line {:.0}, completed lines {:.0}, multiplier {:.1}, sequence {:.0}", last_score.0, last_score.1, last_score.2, last_score.3);
    println!("state changes:");
    for (t, st) in &states { println!("  {t:5} {st}"); }
    Ok(())
}
