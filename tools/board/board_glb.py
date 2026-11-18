"""Stage C3b: Skate's skateboard (deck, trucks, wheels) from the user's converted skater.glb, as a static glTF for
WolvenKit's mesh import, plus its textures as PNG files. Lab only: the output is the user's own Skate 3 data and must
not be committed or shared.

The board's vertices are expressed in the SKATEBOARD_ROOT joint's own frame: u = IBM_root * v_bind. At runtime the
board's world transform is then BZ * (root * bone_SKATEBOARD_ROOT * BLENDER), the same product SK8V's presenter skins
with (stageB/deckrunner-runtime/src/retarget.rs uses it for the body). WolvenKit's glTF import maps glTF (Y up) to
the game's Z up as (x, y, z) -> (x, -z, y); the glTF written here is the inverse of that, so the mesh's local axes
are the joint's.

Usage: python board_glb.py <skater.glb> <out dir> [--report]
Writes <out dir>/deckrunner_board.glb and <out dir>/textures/*.png (base colour and normal per part, plus flat
roughness and metalness from Skate's material factors).
"""
import json
import struct
import sys
from pathlib import Path

import numpy as np

PARTS = [("deck", "Retail_SkateBoard"), ("truck", "Retail_SkateTruck"), ("wheel", "Retail_SkateWheel")]
CT = {5120: "b", 5121: "B", 5122: "h", 5123: "H", 5125: "I", 5126: "f"}
NC = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def read_glb(path):
    b = Path(path).read_bytes()
    jl = struct.unpack_from("<I", b, 12)[0]
    j = json.loads(b[20:20 + jl])
    bin_off = 20 + jl + 8
    return j, b, bin_off


def accessor(j, b, bin_off, i):
    a = j["accessors"][i]
    bv = j["bufferViews"][a["bufferView"]]
    n, k = a["count"], NC[a["type"]]
    fmt = CT[a["componentType"]]
    size = struct.calcsize(fmt)
    stride = bv.get("byteStride", size * k)
    off = bin_off + bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    out = np.empty((n, k), dtype=np.dtype(fmt).newbyteorder("<"))
    raw = np.frombuffer(b, dtype=np.uint8, count=stride * (n - 1) + size * k, offset=off)
    for c in range(k):
        out[:, c] = np.ndarray((n,), dtype=np.dtype(fmt).newbyteorder("<"), buffer=raw, offset=c * size, strides=(stride,))
    return out


def image_bytes(j, b, bin_off, tex_index):
    img = j["images"][j["textures"][tex_index]["source"]]
    bv = j["bufferViews"][img["bufferView"]]
    off = bin_off + bv.get("byteOffset", 0)
    return b[off:off + bv["byteLength"]]


def tangents(p, n, uv, idx):
    """Per-vertex tangents (xyz, handedness w) from the UV layout, Gram-Schmidt against the normal. WolvenKit's mesh
    import requires them."""
    t = np.zeros_like(p)
    b = np.zeros_like(p)
    tri = idx.reshape(-1, 3)
    p0, p1, p2 = p[tri[:, 0]], p[tri[:, 1]], p[tri[:, 2]]
    w0, w1, w2 = uv[tri[:, 0]], uv[tri[:, 1]], uv[tri[:, 2]]
    e1, e2 = p1 - p0, p2 - p0
    d1, d2 = w1 - w0, w2 - w0
    r = d1[:, 0] * d2[:, 1] - d2[:, 0] * d1[:, 1]
    r = np.where(np.abs(r) < 1e-12, 1e-12, r)
    sd = (e1 * d2[:, 1:2] - e2 * d1[:, 1:2]) / r[:, None]
    td = (e2 * d1[:, 0:1] - e1 * d2[:, 0:1]) / r[:, None]
    for k in range(3):
        np.add.at(t, tri[:, k], sd)
        np.add.at(b, tri[:, k], td)
    t = t - n * np.sum(n * t, axis=1, keepdims=True)
    length = np.linalg.norm(t, axis=1, keepdims=True)
    # Degenerate UVs: any direction perpendicular to the normal.
    fallback = np.cross(n, np.where(np.abs(n[:, :1]) < 0.9, [[1.0, 0, 0]], [[0, 1.0, 0]]))
    t = np.where(length > 1e-8, t / np.maximum(length, 1e-12), fallback / np.linalg.norm(fallback, axis=1, keepdims=True))
    w = np.where(np.sum(np.cross(n, t) * b, axis=1) < 0, -1.0, 1.0)
    return np.concatenate([t, w[:, None]], axis=1)


def solid_png(value):
    """A 4x4 greyscale PNG of one value (0..255), written without external libraries."""
    import zlib
    raw = b"".join(b"\x00" + bytes([value]) * 4 for _ in range(4))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 0, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


def main():
    src, out = sys.argv[1], Path(sys.argv[2])
    report = "--report" in sys.argv
    j, b, bin_off = read_glb(src)
    skin = j["skins"][0]
    names = [j["nodes"][k]["name"] for k in skin["joints"]]
    ibm = accessor(j, b, bin_off, skin["inverseBindMatrices"]).astype(np.float64).reshape(-1, 4, 4).transpose(0, 2, 1)
    root_ibm = ibm[names.index("SKATEBOARD_ROOT")]
    mats = {m["name"]: i for i, m in enumerate(j["materials"])}
    prims = {p.get("material"): p for p in j["meshes"][0]["primitives"]}
    parts = []
    for part, mat_name in PARTS:
        p = prims[mats[mat_name]]
        pos = accessor(j, b, bin_off, p["attributes"]["POSITION"]).astype(np.float64)
        nrm = accessor(j, b, bin_off, p["attributes"]["NORMAL"]).astype(np.float64)
        uv = accessor(j, b, bin_off, p["attributes"]["TEXCOORD_0"]).astype(np.float64)
        idx = accessor(j, b, bin_off, p["indices"]).astype(np.int64).reshape(-1)
        # Bind space -> the board joint's frame.
        u = (root_ibm[:3, :3] @ pos.T).T + root_ibm[:3, 3]
        n = (root_ibm[:3, :3] @ nrm.T).T
        n /= np.linalg.norm(n, axis=1, keepdims=True)
        parts.append((part, mat_name, u, n, uv, idx))
        if report:
            print(f"{part}: {len(u)} vertices, {len(idx) // 3} triangles; joint frame box min {u.min(0).round(3)} max {u.max(0).round(3)}")
    allu = np.concatenate([p[2] for p in parts])
    if report:
        print("whole board: min", allu.min(0).round(3), "max", allu.max(0).round(3), "size", (allu.max(0) - allu.min(0)).round(3))
        return
    out.mkdir(parents=True, exist_ok=True)
    (out / "textures").mkdir(exist_ok=True)
    # glTF writer: per part one mesh node, positions/normals in glTF axes so WolvenKit's import lands them in the
    # joint frame: game (x, y, z) <- glTF (x, -z, y), i.e. glTF = (x, z, -y).
    to_gltf = lambda a: np.stack([a[:, 0], a[:, 2], -a[:, 1]], axis=1)
    blob = bytearray()
    views, accs, meshes, nodes, materials, images, textures = [], [], [], [], [], [], []

    def add(arr, comp, typ, target=None, minmax=False):
        data = np.ascontiguousarray(arr, dtype={5126: "<f4", 5125: "<u4"}[comp]).tobytes()
        while len(blob) % 4: blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data), **({"target": target} if target else {})})
        blob.extend(data)
        acc = {"bufferView": len(views) - 1, "componentType": comp, "count": int(arr.shape[0]), "type": typ}
        if minmax:
            acc["min"] = [float(x) for x in arr.min(0)]
            acc["max"] = [float(x) for x in arr.max(0)]
        accs.append(acc)
        return len(accs) - 1

    for part, mat_name, u, n, uv, idx in parts:
        m = j["materials"][mats[mat_name]]
        pbr = m["pbrMetallicRoughness"]
        col = image_bytes(j, b, bin_off, pbr["baseColorTexture"]["index"])
        nrm = image_bytes(j, b, bin_off, m["normalTexture"]["index"])
        (out / "textures" / f"board_{part}_d.png").write_bytes(col)
        (out / "textures" / f"board_{part}_n.png").write_bytes(nrm)
        (out / "textures" / f"board_{part}_r.png").write_bytes(solid_png(round(255 * pbr.get("roughnessFactor", 1.0))))
        (out / "textures" / f"board_{part}_m.png").write_bytes(solid_png(round(255 * pbr.get("metallicFactor", 0.0))))
        a_pos = add(to_gltf(u), 5126, "VEC3", 34962, minmax=True)
        a_nrm = add(to_gltf(n), 5126, "VEC3", 34962)
        a_uv = add(uv, 5126, "VEC2", 34962)
        a_tan = add(np.concatenate([to_gltf(tangents(u, n, uv, idx)[:, :3]), tangents(u, n, uv, idx)[:, 3:]], axis=1), 5126, "VEC4", 34962)
        a_idx = add(idx.reshape(-1, 1), 5125, "SCALAR", 34963)
        materials.append({"name": f"board_{part}", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1]}})
        meshes.append({"name": f"board_{part}", "primitives": [{"attributes": {"POSITION": a_pos, "NORMAL": a_nrm, "TANGENT": a_tan, "TEXCOORD_0": a_uv},
                                                                 "indices": a_idx, "material": len(materials) - 1}]})
        nodes.append({"name": f"submesh_{len(nodes):02d}_LOD_1", "mesh": len(meshes) - 1})
    while len(blob) % 4: blob.append(0)
    doc = {"asset": {"version": "2.0", "generator": "deckrunner board_glb.py"}, "scene": 0, "scenes": [{"nodes": list(range(len(nodes)))}],
           "nodes": nodes, "meshes": meshes, "materials": materials, "accessors": accs, "bufferViews": views,
           "buffers": [{"byteLength": len(blob)}]}
    js = json.dumps(doc, separators=(",", ":")).encode()
    while len(js) % 4: js += b" "
    glb = b"glTF" + struct.pack("<II", 2, 12 + 8 + len(js) + 8 + len(blob)) + struct.pack("<I", len(js)) + b"JSON" + js + struct.pack("<I", len(blob)) + b"BIN\x00" + bytes(blob)
    (out / "deckrunner_board.glb").write_bytes(glb)
    print(f"wrote {out / 'deckrunner_board.glb'} ({len(glb)} bytes, {sum(len(p[5]) // 3 for p in parts)} triangles) and {len(list((out / 'textures').glob('*.png')))} textures")
    print("board in its joint frame: min", allu.min(0).round(3), "max", allu.max(0).round(3))


if __name__ == "__main__":
    main()
