#!/usr/bin/env python3
"""Assert that a uvremap glTF output carries no unreachable entries.

usage: check_orphans.py <output-dir> [<model.gltf>]

Checks that every material is reachable from a primitive (directly or through
KHR_materials_variants), every texture from a material, every image from a
texture, and that no file sits in textures/ without being referenced.
"""
import json
import os
import sys

SLOTS = ("baseColorTexture", "metallicRoughnessTexture", "occlusionTexture",
         "emissiveTexture", "normalTexture")


def numbers(node, key, out):
    """Collects integers stored under `key` anywhere in an extension value."""
    if isinstance(node, dict):
        for k, v in node.items():
            if k == key and isinstance(v, int):
                out.append(v)
            else:
                numbers(v, key, out)
    elif isinstance(node, list):
        for v in node:
            numbers(v, key, out)


def main() -> int:
    out_dir = sys.argv[1]
    models = sys.argv[2:]
    if not models:
        models = [os.path.join(out_dir, f) for f in os.listdir(out_dir)
                  if f.endswith(".gltf")]
    if not models:
        print("no gltf model found", file=sys.stderr)
        return 1
    problems = []
    referenced_files = set()
    for path in models:
        with open(path) as fh:
            doc = json.load(fh)
        mats, texs, imgs = (doc.get("materials", []), doc.get("textures", []),
                           doc.get("images", []))
        used_m = set()
        for mesh in doc.get("meshes", []):
            for prim in mesh.get("primitives", []):
                if prim.get("material") is not None:
                    used_m.add(prim["material"])
                found = []
                for value in (prim.get("extensions") or {}).values():
                    numbers(value, "material", found)
                used_m.update(found)
        used_t = set()
        for i in sorted(used_m):
            m = mats[i]
            for group in (m.get("pbrMetallicRoughness", {}), m):
                for slot in SLOTS:
                    t = group.get(slot)
                    if isinstance(t, dict) and isinstance(t.get("index"), int):
                        used_t.add(t["index"])
            found = []
            for value in (m.get("extensions") or {}).values():
                numbers(value, "index", found)
            used_t.update(found)
        used_i = set()
        for i in sorted(used_t):
            used_i.add(texs[i]["source"])
        for i in sorted(used_i):
            uri = imgs[i].get("uri", "")
            if not uri.startswith("data:"):
                referenced_files.add(os.path.normpath(os.path.join(path, "..", uri)))
        if len(used_m) != len(mats):
            problems.append(f"{os.path.basename(path)}: {len(mats) - len(used_m)} "
                            f"unreferenced material(s) of {len(mats)}")
        if len(used_t) != len(texs):
            problems.append(f"{os.path.basename(path)}: {len(texs) - len(used_t)} "
                            f"unreferenced texture(s) of {len(texs)}")
        if len(used_i) != len(imgs):
            problems.append(f"{os.path.basename(path)}: {len(imgs) - len(used_i)} "
                            f"unreferenced image(s) of {len(imgs)}")
        for i in sorted(used_m):
            if i < 0 or i >= len(mats):
                problems.append(f"{os.path.basename(path)}: material index {i} out of range")
        for i in sorted(used_t):
            if i < 0 or i >= len(texs):
                problems.append(f"{os.path.basename(path)}: texture index {i} out of range")
        for i in sorted(used_i):
            if i < 0 or i >= len(imgs):
                problems.append(f"{os.path.basename(path)}: image index {i} out of range")
    tex_dir = os.path.join(out_dir, "textures")
    if os.path.isdir(tex_dir):
        for root, _, files in os.walk(tex_dir):
            for f in files:
                full = os.path.normpath(os.path.join(root, f))
                if full not in referenced_files:
                    problems.append(f"orphan file on disk: {os.path.relpath(full, out_dir)}")
    for p in problems:
        print(p, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
