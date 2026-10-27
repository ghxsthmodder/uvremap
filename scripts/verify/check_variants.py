#!/usr/bin/env python3
"""Check KHR_materials_variants survives UV regeneration.

Usage: check_variants.py <output.gltf|output.glb>

Asserts the extension and the root variant list survive, every mapping names a
distinct material, mapped materials point at rebuilt textures (uri/name or
buffer view mentioning the atlas group), and each variant's texture is a
different image (per-material bake copies).
"""
import base64
import json
import struct
import sys


def load_json(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[4:8] == b'\x02\x00\x00\x00':  # GLB
        magic, ver, length = struct.unpack('<III', data[:12])
        clen, ctype = struct.unpack('<II', data[12:20])
        assert ctype == 0x4E4F534A  # 'JSON'
        return json.loads(data[20:20 + clen])
    with open(path) as f:
        return json.load(f)


def tex_image(j, mat_idx, slot='baseColorTexture'):
    textures = j['textures']
    images = j['images']
    pbr = j['materials'][mat_idx].get('pbrMetallicRoughness', {})
    t = pbr.get(slot)
    if not t:
        return None
    tex = textures[t['index']]
    return images[tex['source']]


def image_key(j, img):
    """Identity of an image: uri/data-uri, or bufferView contents."""
    if 'uri' in img:
        u = img['uri']
        if u.startswith('data:'):
            raw = base64.b64decode(u.split(',', 1)[1])
            return hash(raw[:64 << 10])
        return u
    if 'bufferView' in img:
        bv = j['bufferViews'][img['bufferView']]
        with open(sys.argv[1], 'rb') as f:
            whole = f.read()
        if whole[4:8] == 2:
            off = 12
            blen = None
            while off + 8 <= len(whole):
                l, ty = struct.unpack('<II', whole[off:off + 8])
                if ty == 0x004E4942:
                    blen = l
                    bin_off = off + 8
                    break
                off += 8 + l
            start = bin_off + bv.get('byteOffset', 0)
        else:
            start = bv.get('byteOffset', 0)
        return hash(whole[start:start + bv['byteLength']])
    return img.get('name')


def main():
    j = load_json(sys.argv[1])
    exts = j.get('extensionsUsed', [])
    assert 'KHR_materials_variants' in exts, 'KHR_materials_variants missing from extensionsUsed'
    root = j.get('extensions', {}).get('KHR_materials_variants')
    assert root and 'variants' in root, 'root variants missing'
    n_variants = len(root['variants'])
    assert n_variants > 1, 'expected more than one variant'

    checked = 0
    for mi, mesh in enumerate(j.get('meshes', [])):
        for pi, p in enumerate(mesh.get('primitives', [])):
            pv = p.get('extensions', {}).get('KHR_materials_variants')
            if not pv:
                continue
            mappings = pv['mappings']
            assert mappings, f'mesh {mi} prim {pi}: empty mappings'
            mats = [m['material'] for m in mappings]
            assert len(set(mats)) == len(mats), f'mesh {mi} prim {pi}: duplicate mapped materials'
            keys = {}
            for m in mats:
                img = tex_image(j, m)
                assert img is not None, f'material {m}: no baseColorTexture'
                ident = (img.get('name') or '') + ' ' + (img.get('uri') or '')
                assert ('basecolor' in ident) or ('bufferView' in img), (
                    f'material {m}: texture is not a rebuilt atlas image: {ident!r}')
                k = image_key(j, img)
                assert k not in keys, f'materials {keys.get(k)} and {m} share one image'
                keys[k] = m
            checked += 1
    assert checked > 0, 'no primitive carried KHR_materials_variants'
    print(f'ok: {checked} variant-prim(s), {n_variants} variants, '
          f'{len(keys)} distinct baked texture(s) on last prim')


if __name__ == '__main__':
    main()
