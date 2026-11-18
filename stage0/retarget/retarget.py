"""Stage 0 retarget prototype: Skate 3 skater pose stream -> Cyberpunk 2077 player rig.

This is the 'RigProfile' idea from the report, prototyped offline. It follows SK8V's ped.rs principles:
- Bind fit: each mapped bone wears its Skate joint's bind->posed rotation, carried through a fit that swings the
  Cyberpunk bind segment onto Skate's bind segment. Feet are fitted about the vertical only; terminal bones take
  their parent's fit.
- Cyberpunk keeps its own bone lengths (rigid bones). The pelvis is placed so V's hip joints straddle Skate's.
- Two-bone IK puts V's ankles and wrists on Skate's, bending in the plane Skate's limb bends in.

Inputs: the stub's pose.json/pose.bin and trace.jsonl, Skate's converted skater.glb (inverse binds), and a
Cyberpunk rig JSON from investigation/probes/cr2w_rig.py.
Outputs: metrics.json, frames.png (Skate grey vs V coloured), and cp_pose.npz (V's world matrices per frame).

Usage: python retarget.py <stage0 run dir> <skater.glb> <cp rig json> <out dir>
"""
import json
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

MAP = ["Hips", "Spine", "Spine1", "Spine2", "Spine3", "Neck", "Neck1", "Head",
       "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand", "RightShoulder", "RightArm", "RightForeArm", "RightHand",
       "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase", "RightUpLeg", "RightLeg", "RightFoot", "RightToeBase"]
SEGMENT = {  # bone -> child that defines its bind segment
    "Hips": "Spine", "Spine": "Spine1", "Spine1": "Spine2", "Spine2": "Spine3", "Spine3": "Neck", "Neck": "Neck1",
    "Neck1": "Head", "LeftShoulder": "LeftArm", "LeftArm": "LeftForeArm", "LeftForeArm": "LeftHand",
    "RightShoulder": "RightArm", "RightArm": "RightForeArm", "RightForeArm": "RightHand",
    "LeftUpLeg": "LeftLeg", "LeftLeg": "LeftFoot", "RightUpLeg": "RightLeg", "RightLeg": "RightFoot",
    "LeftFoot": "LeftToeBase", "RightFoot": "RightToeBase",
}
TERMINAL = {"Head": "Neck1", "LeftHand": "LeftForeArm", "RightHand": "RightForeArm",
            "LeftToeBase": "LeftFoot", "RightToeBase": "RightFoot"}
GROUND = {"LeftFoot", "RightFoot"}
LIMBS = [("LeftUpLeg", "LeftLeg", "LeftFoot"), ("RightUpLeg", "RightLeg", "RightFoot"),
         ("LeftArm", "LeftForeArm", "LeftHand"), ("RightArm", "RightForeArm", "RightHand")]
B3 = np.array([[1, 0, 0], [0, 0, -1], [0, 1, 0]], float)  # Skate (Y up) -> Z up, SK8V coords::basis
BZ = np.eye(4); BZ[:3, :3] = B3
BLENDER = np.array([[1, 0, 0, 0], [0, 0, 1, 0], [0, -1, 0, 0], [0, 0, 0, 1]], float)  # cols X, -Z, Y, W


def unit(v):
    n = np.linalg.norm(v)
    return v / n if n > 1e-9 else v


def rot_between(a, b):
    a, b = unit(a), unit(b)
    v, c = np.cross(a, b), float(np.dot(a, b))
    if c < -0.999999:
        axis = unit(np.cross(a, [1, 0, 0] if abs(a[0]) < 0.9 else [0, 1, 0]))
        return 2 * np.outer(axis, axis) - np.eye(3)
    k = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + k + k @ k / (1 + c)


def quat_mat(q):  # (i, j, k, r)
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def orthonormal(m):
    u, _, vt = np.linalg.svd(m)
    r = u @ vt
    if np.linalg.det(r) < 0:
        u[:, -1] *= -1
        r = u @ vt
    return r


def load_glb_ibm(path):
    b = Path(path).read_bytes()
    jl = struct.unpack_from("<I", b, 12)[0]
    j = json.loads(b[20:20 + jl])
    bin_off = 20 + jl + 8
    skin = j["skins"][0]
    acc = j["accessors"][skin["inverseBindMatrices"]]
    bv = j["bufferViews"][acc["bufferView"]]
    off = bin_off + bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    m = np.frombuffer(b, "<f4", acc["count"] * 16, off).reshape(-1, 4, 4).transpose(0, 2, 1).astype(float)
    names = [j["nodes"][i]["name"] for i in skin["joints"]]
    return dict(zip(names, m))


def cp_bind(rig):
    bones, parents, ls = rig["bones"], rig["parents"], rig["referencePoseLS"]
    world = []
    for i, (t, q, s) in enumerate(ls):
        local = np.eye(4)
        local[:3, :3] = quat_mat(q) * np.array(s[:3])
        local[:3, 3] = t[:3]
        world.append(local if parents[i] < 0 else world[parents[i]] @ local)
    return bones, parents, [np.array(m) for m in world], [np.array(t[:3], float) for t, _, _ in ls]


def two_bone(a, b, c, target, pole_hint):
    """Positions of the middle and end joint for chain a-b-c reaching target, bending toward pole_hint."""
    l1, l2 = np.linalg.norm(b - a), np.linalg.norm(c - b)
    d = target - a
    dist = np.clip(np.linalg.norm(d), 1e-6, l1 + l2 - 1e-6)
    dirn = unit(d)
    cos_a = np.clip((l1 * l1 + dist * dist - l2 * l2) / (2 * l1 * dist), -1, 1)
    side = unit(pole_hint - a - np.dot(pole_hint - a, dirn) * dirn)
    mid = a + dirn * (l1 * cos_a) + side * (l1 * np.sqrt(max(0.0, 1 - cos_a * cos_a)))
    end = a + dirn * dist
    return mid, end, np.linalg.norm(target - a) > l1 + l2


def main():
    run, glb, rig_path, out = Path(sys.argv[1]), sys.argv[2], sys.argv[3], Path(sys.argv[4])
    out.mkdir(parents=True, exist_ok=True)
    meta = json.loads((run / "pose.json").read_text())
    names = meta["names"]
    n = meta["matrices_per_frame"]
    raw = np.fromfile(run / "pose.bin", "<f4").reshape(-1, n, 4, 4).transpose(0, 1, 3, 2).astype(float)
    frames = raw.shape[0]
    trace = [json.loads(line) for line in (run / "trace.jsonl").open()]
    ibm = load_glb_ibm(glb)
    rig = json.loads(Path(rig_path).read_text())
    cp_names, cp_parent, cp_world_bind, cp_local_t = cp_bind(rig)
    ci = {nm: i for i, nm in enumerate(cp_names)}
    si = {nm: i for i, nm in enumerate(names)}

    # Skate bind (Z up): joint frames = BZ * inverse(IBM)
    sk_bind = {nm.upper(): BZ @ np.linalg.inv(ibm[nm.upper()]) for nm in MAP}
    sb = lambda nm: sk_bind[nm.upper()][:3, 3]

    # Align V's bind heading to Skate's bind heading (both stand upright, Z up): lateral axis right->left hip.
    lat_s = unit((sb("LeftUpLeg") - sb("RightUpLeg")) * [1, 1, 0])
    cpb = lambda nm: cp_world_bind[ci[nm]][:3, 3]
    lat_c = unit((cpb("LeftUpLeg") - cpb("RightUpLeg")) * [1, 1, 0])
    align = np.eye(4)
    align[:3, :3] = rot_between(lat_c, lat_s)
    cp_bind_al = [align @ m for m in cp_world_bind]
    cpa = lambda nm: cp_bind_al[ci[nm]][:3, 3]
    height_c = cpa("Head")[2] - min(cpa("LeftFoot")[2], cpa("RightFoot")[2])
    height_s = sb("Head")[2] - min(sb("LeftFoot")[2], sb("RightFoot")[2])

    # Bind fits.
    fit = {}
    for nm in MAP:
        if nm in SEGMENT:
            f = rot_between(cpa(SEGMENT[nm]) - cpa(nm), sb(SEGMENT[nm]) - sb(nm))
            if nm in GROUND:  # about the vertical only
                a = unit((cpa(SEGMENT[nm]) - cpa(nm)) * [1, 1, 0])
                b = unit((sb(SEGMENT[nm]) - sb(nm)) * [1, 1, 0])
                f = rot_between(a, b)
            fit[nm] = f
    for nm, src in TERMINAL.items():
        fit[nm] = fit[src]
    hipmid_local = np.linalg.inv(cp_bind_al[ci["Hips"]][:3, :3]) @ (
        (cpa("LeftUpLeg") + cpa("RightUpLeg")) / 2 - cpa("Hips"))

    order = sorted(range(len(cp_names)), key=lambda i: (cp_parent[i] >= 0, 0))
    # parents always precede children in the rig arrays (verified while decoding)
    assert all(cp_parent[i] < i for i in range(len(cp_names)))
    out_world = np.zeros((frames, len(cp_names), 4, 4))
    stats = {"unreachable": {l[2]: 0 for l in LIMBS}, "knee_deg": [], "elbow_deg": [], "toe_err": [], "nan": 0,
             "max_rot_step_deg_cp": 0.0, "max_rot_step_deg_skate": 0.0, "sole_above_deck": [],
             "toe_above_deck_cp": [], "toe_above_deck_skate": []}
    prev_cp, prev_sk = None, None
    for f in range(frames):
        root = raw[f, 0]
        bones = raw[f, 1:]
        world_s = {nm: BZ @ (root @ bones[si[nm.upper()]] @ BLENDER) for nm in MAP}
        skin = {nm: BZ @ (root @ bones[si[nm.upper()]] @ BLENDER @ ibm[nm.upper()]) @ BZ.T for nm in MAP}
        sp = lambda nm: world_s[nm][:3, 3]
        rot = {}
        for nm in MAP:
            rot[nm] = orthonormal(skin[nm][:3, :3]) @ fit[nm] @ cp_bind_al[ci[nm]][:3, :3]
        # FK with V's own lengths.
        W = [None] * len(cp_names)
        for i, nm in enumerate(cp_names):
            p = cp_parent[i]
            if nm == "Hips":
                r = rot["Hips"]
                pos = (sp("LeftUpLeg") + sp("RightUpLeg")) / 2 - r @ hipmid_local
            elif p < 0:
                r, pos = cp_bind_al[i][:3, :3], cp_bind_al[i][:3, 3]
            else:
                pr, pp = W[p][:3, :3], W[p][:3, 3]
                pbind = cp_bind_al[p][:3, :3]
                local_r = pbind.T @ cp_bind_al[i][:3, :3]
                r = rot[nm] if nm in rot else pr @ local_r
                pos = pp + pr @ (pbind.T @ (cp_bind_al[i][:3, 3] - cp_bind_al[p][:3, 3]))
            M = np.eye(4); M[:3, :3] = r; M[:3, 3] = pos
            W[i] = M
            if nm == "Hips" and cp_parent[i] >= 0:
                pass
        # Two-bone IK: ankles and wrists onto Skate's, bending in Skate's limb plane.
        for up, mid, end in LIMBS:
            a, b, c = W[ci[up]][:3, 3], W[ci[mid]][:3, 3], W[ci[end]][:3, 3]
            nb, nc, far = two_bone(a, b, c, sp(end), sp(mid))
            stats["unreachable"][end] += int(far)
            W[ci[up]][:3, :3] = rot_between(b - a, nb - a) @ W[ci[up]][:3, :3]
            W[ci[mid]][:3, :3] = rot_between(c - b, nc - nb) @ W[ci[mid]][:3, :3]
            W[ci[mid]][:3, 3] = nb
            shift = nc - c
            # move the end joint and everything below it
            stack = [ci[end]]
            while stack:
                k = stack.pop()
                W[k][:3, 3] += shift
                stack += [j for j in range(len(cp_names)) if cp_parent[j] == k]
            bend = np.degrees(np.arccos(np.clip(np.dot(unit(nb - a), unit(nc - nb)), -1, 1)))
            (stats["knee_deg"] if "Leg" in up else stats["elbow_deg"]).append(bend)
        for side in ("Left", "Right"):
            stats["toe_err"].append(float(np.linalg.norm(W[ci[side + "ToeBase"]][:3, 3] - sp(side + "ToeBase"))))
        # Sole vs deck plane (deck centre and normal from the board root joint).
        deck = BZ @ (root @ bones[si["SKATEBOARD_ROOT"]] @ BLENDER)
        state = trace[f]["state"]
        if state.startswith(("PhysicsGround", "Grind")):
            normal = unit(deck[:3, :3] @ np.array([0, 0, 1.0]))
            for side in ("Left", "Right"):
                for b in ("Heel", "ToeBase"):
                    stats["sole_above_deck"].append(float(np.dot(W[ci[side + b]][:3, 3] - deck[:3, 3], normal)))
                stats["toe_above_deck_cp"].append(float(np.dot(W[ci[side + "ToeBase"]][:3, 3] - deck[:3, 3], normal)))
                stats["toe_above_deck_skate"].append(float(np.dot(sp(side + "ToeBase") - deck[:3, 3], normal)))
        cur = np.stack(W)
        if not np.isfinite(cur).all():
            stats["nan"] += 1
        sk_cur = np.stack([world_s[nm] for nm in MAP])
        if prev_cp is not None:
            def step(a, b):
                r = np.einsum("nij,nkj->nik", a[:, :3, :3], b[:, :3, :3])
                tr = np.clip((np.trace(r, axis1=1, axis2=2) - 1) / 2, -1, 1)
                return float(np.degrees(np.arccos(tr)).max())
            mapped = [ci[nm] for nm in MAP]
            stats["max_rot_step_deg_cp"] = max(stats["max_rot_step_deg_cp"], step(cur[mapped], prev_cp[mapped]))
            stats["max_rot_step_deg_skate"] = max(stats["max_rot_step_deg_skate"], step(sk_cur, prev_sk))
        prev_cp, prev_sk = cur, sk_cur
        out_world[f] = cur

    # Bone-length check over the whole run (mapped chain segments).
    length_dev = 0.0
    for nm, child in SEGMENT.items():
        bind_len = np.linalg.norm(cpa(child) - cpa(nm))
        lens = np.linalg.norm(out_world[:, ci[child], :3, 3] - out_world[:, ci[nm], :3, 3], axis=1)
        length_dev = max(length_dev, float(np.abs(lens - bind_len).max()))
    q = lambda xs, p: float(np.percentile(xs, p)) if xs else None
    metrics = {
        "frames": frames, "rig": rig["path"], "mapped_bones": len(MAP),
        "bind_height_cp_m": round(float(height_c), 3), "bind_height_skate_m": round(float(height_s), 3),
        "max_bone_length_deviation_m": length_dev,
        "nan_frames": stats["nan"], "unreachable_frames": stats["unreachable"],
        "knee_bend_deg_p1_p99": [q(stats["knee_deg"], 1), q(stats["knee_deg"], 99)],
        "elbow_bend_deg_p1_p99": [q(stats["elbow_deg"], 1), q(stats["elbow_deg"], 99)],
        "toe_offset_from_skate_toe_m_p50_p99": [q(stats["toe_err"], 50), q(stats["toe_err"], 99)],
        "sole_height_above_deck_centre_m_p1_p50_p99": [q(stats["sole_above_deck"], 1), q(stats["sole_above_deck"], 50), q(stats["sole_above_deck"], 99)],
        "toe_joint_above_deck_centre_m_p1_p50": {"cp": [q(stats["toe_above_deck_cp"], 1), q(stats["toe_above_deck_cp"], 50)],
                                                  "skate": [q(stats["toe_above_deck_skate"], 1), q(stats["toe_above_deck_skate"], 50)]},
        "max_joint_rotation_per_tick_deg": {"cp": stats["max_rot_step_deg_cp"], "skate": stats["max_rot_step_deg_skate"]},
    }
    (out / "metrics.json").write_text(json.dumps(metrics, indent=2))
    np.savez_compressed(out / "cp_pose.npz", world=out_world.astype(np.float32), names=np.array(cp_names), parents=np.array(cp_parent))
    print(json.dumps(metrics, indent=2))

    # Render comparison frames: side view (Y forward, Z up) and front view (X, Z).
    picks = [int(x) for x in sys.argv[5].split(",")] if len(sys.argv) > 5 else [300, 438, 650, 745, 1105]
    picks = [p for p in picks if p < frames]
    cell, scale = 340, 150.0
    img = Image.new("RGB", (cell * len(picks), cell * 2 + 30), (250, 250, 250))
    d = ImageDraw.Draw(img)
    skel_s = [(nm, SEGMENT.get(nm) or None) for nm in MAP]
    colors = {"Left": (40, 90, 220), "Right": (220, 50, 40)}
    for k, f in enumerate(picks):
        root, bones = raw[f, 0], raw[f, 1:]
        ws = {nm: (BZ @ (root @ bones[si[nm.upper()]] @ BLENDER))[:3, 3] for nm in MAP}
        for extra in ("SKATEBOARD_ROOT", "TRUCK_FRONT", "TRUCK_BACK"):
            ws[extra] = (BZ @ (root @ bones[si[extra]] @ BLENDER))[:3, 3]
        wc = {nm: out_world[f, ci[nm], :3, 3] for nm in cp_names}
        centre = ws["Hips"]
        for view in range(2):
            ox, oy = k * cell + cell // 2, view * cell + cell // 2 + 40
            def P(p):
                rel = p - centre
                h = rel[1] if view == 0 else rel[0]
                return (ox + h * scale, oy - rel[2] * scale)
            # deck
            d.line([P(ws["TRUCK_BACK"]), P(ws["TRUCK_FRONT"])], fill=(120, 80, 40), width=5)
            for nm, ch in skel_s:
                if ch:
                    d.line([P(ws[nm]), P(ws[ch])], fill=(170, 170, 170), width=7)
            for i, nm in enumerate(cp_names):
                p = cp_parent[i]
                if p < 0 or cp_names[p] in ("Root", "Trajectory"):
                    continue
                col = colors.get("Left" if nm.startswith("Left") else "Right" if nm.startswith("Right") else "", (30, 150, 60))
                d.line([P(wc[cp_names[p]]), P(wc[nm])], fill=col, width=2)
        d.text((k * cell + 6, 4), f"tick {f}: {trace[f]['state']}", fill=(0, 0, 0))
        d.text((k * cell + 6, 18), "side (Y fwd, Z up) / front (X, Z)", fill=(90, 90, 90))
    img.save(out / "frames.png")
    print("wrote", out / "frames.png")
    gif = []
    for f in range(0, frames, 4):
        im = Image.new("RGB", (cell * 2, cell + 20), (250, 250, 250))
        dd = ImageDraw.Draw(im)
        root, bones = raw[f, 0], raw[f, 1:]
        ws = {nm: (BZ @ (root @ bones[si[nm.upper()]] @ BLENDER))[:3, 3] for nm in MAP}
        for extra in ("TRUCK_FRONT", "TRUCK_BACK"):
            ws[extra] = (BZ @ (root @ bones[si[extra]] @ BLENDER))[:3, 3]
        wc = {nm: out_world[f, ci[nm], :3, 3] for nm in cp_names}
        centre = ws["Hips"]
        for view in range(2):
            ox, oy = view * cell + cell // 2, cell // 2 + 30
            def P(p, view=view, ox=ox, oy=oy):
                rel = p - centre
                return (ox + (rel[1] if view == 0 else rel[0]) * scale, oy - rel[2] * scale)
            dd.line([P(ws["TRUCK_BACK"]), P(ws["TRUCK_FRONT"])], fill=(120, 80, 40), width=5)
            for nm, ch in skel_s:
                if ch:
                    dd.line([P(ws[nm]), P(ws[ch])], fill=(170, 170, 170), width=7)
            for i, nm in enumerate(cp_names):
                p = cp_parent[i]
                if p < 0 or cp_names[p] in ("Root", "Trajectory"):
                    continue
                col = colors.get("Left" if nm.startswith("Left") else "Right" if nm.startswith("Right") else "", (30, 150, 60))
                dd.line([P(wc[cp_names[p]]), P(wc[nm])], fill=col, width=2)
        dd.text((6, 4), f"tick {f}  {trace[f]['state']}   grey: Skate 3 skater   colour: V ({Path(rig_path).stem})", fill=(0, 0, 0))
        gif.append(im)
    gif[0].save(out / "line.gif", save_all=True, append_images=gif[1:], duration=67, loop=0)
    print("wrote", out / "line.gif")


if __name__ == "__main__":
    main()
