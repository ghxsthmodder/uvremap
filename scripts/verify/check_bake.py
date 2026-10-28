#!/usr/bin/env python3
"""Verify uvremap-baked atlases against the original source textures.

Run uvremap with --source-uv 0 --target-uv 1 so the output keeps both the
source UVs (TEXCOORD_0) and the new atlas UVs (TEXCOORD_1). For random
points on the remeshed triangles this samples the baked atlas at the atlas
UV and the original texture at the source UV and checks agreement.

Usage: check_bake.py <output.gltf|output.glb> <original_basecolor.png> [input.gltf|input.glb]
Exit 0 when >=90% of covered sample points agree. When the input model is
given, its materials' KHR_texture_transform (matched by material name) is
applied to source UVs and the wrap mode of the source texture's sampler is
followed when sampling the original texture.
"""
import base64, io, json, math, os, struct, sys

import numpy as np
from PIL import Image

if len(sys.argv) < 3:
    print(__doc__);
    sys.exit(2)
path, orig_path = sys.argv[1], sys.argv[2]
src_model_path = sys.argv[3] if len(sys.argv) > 3 else None


def load_gltf(p):
    if p.endswith('.glb'):
        with open(p, 'rb') as f:
            data = f.read()
        ln, typ = struct.unpack('<II', data[12:20])
        assert typ == 0x4E4F534A
        j = json.loads(data[20:20 + ln])
        j['_bin'] = data[20 + ln + 8:]
        return j
    with open(p) as f:
        return json.load(f)


j = load_gltf(path)
BASE = os.path.dirname(os.path.abspath(path))


def buffer_bytes(j, i, base=None):
    base = BASE if base is None else base
    b = j['buffers'][i]
    if 'uri' not in b:
        return j['_bin']
    uri = b['uri']
    if uri.startswith('data:'):
        return base64.b64decode(uri.split(',', 1)[1])
    with open(os.path.join(base, uri), 'rb') as f:
        return f.read()


CT = {'5120': ('b', 1), '5121': ('B', 1), '5122': ('h', 2), '5123': ('H', 2),
      '5124': ('i', 4), '5125': ('I', 4), '5126': ('f', 4)}
NT = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}


def read_accessor(j, idx):
    a = j['accessors'][idx]
    fmt, comp = CT[str(a['componentType'])]
    n = NT[a['type']]
    bv = j['bufferViews'][a['bufferView']]
    buf = buffer_bytes(j, bv['buffer'])
    stride = bv.get('byteStride', comp * n)
    off = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
    out = np.empty((a['count'], n), dtype=np.float64)
    for i in range(a['count']):
        out[i] = struct.unpack_from('<' + fmt * n, buf, off + i * stride)
    return out


def image_bytes(j, i, base=None):
    base = BASE if base is None else base
    img = j['images'][i]
    if 'uri' in img:
        uri = img['uri']
        if uri.startswith('data:'):
            return base64.b64decode(uri.split(',', 1)[1])
        with open(os.path.join(base, uri), 'rb') as f:
            return f.read()
    bv = j['bufferViews'][img['bufferView']]
    buf = buffer_bytes(j, bv['buffer'], base)
    return buf[bv.get('byteOffset', 0):bv.get('byteOffset', 0) + bv['byteLength']]


def to_img(arr):
    return np.array(Image.open(io.BytesIO(arr)).convert('RGB'), dtype=np.float64)


# glTF wrap codes: 33071/10497 clamp, 33648/10498 mirrored, 10499 repeat.
WRAPCODES = {33071: 'clamp', 10497: 'clamp', 33648: 'mirror', 10498: 'mirror',
             10499: 'repeat'}
# A texture with no sampler is implementation-defined; every renderer we know
# repeats, and so does the bake.
REPEAT_WRAP = ('repeat', 'repeat')


def fold(x, mode):
    if mode == 'repeat':
        return x % 1.0
    if mode == 'mirror':
        p = x % 2.0
        return p if p < 1.0 else 2.0 - p
    return x


def sample_nearest(img, u, v, wrap):
    h, w = img.shape[:2]
    x = int(np.clip(fold(u, wrap[0]) * w, 0, w - 1))
    y = int(np.clip(fold(v, wrap[1]) * h, 0, h - 1))
    return img[y, x]


def apply_transform(uv, off, scale, rot):
    u, v = uv[0] * scale[0], uv[1] * scale[1]
    if rot:
        c, s = math.cos(rot), math.sin(rot)
        u, v = u * c - v * s, u * s + v * c
    return (u + off[0], v + off[1])


def sampler_wrap(j2, tex):
    """(wrapS, wrapT) of a texture entry, per the spec's defaults."""
    s = tex.get('sampler')
    if s is None or 'samplers' not in j2:
        return REPEAT_WRAP
    sm = j2['samplers'][s]
    return tuple(WRAPCODES.get(sm.get(k, 10497), 'clamp')
                 for k in ('wrapS', 'wrapT'))


def tex_transforms(p):
    """material name -> list of input baseColorTexture info dicts."""
    if not p:
        return {}
    j2 = load_gltf(p)
    base2 = os.path.dirname(os.path.abspath(p))
    img_cache = {}
    out = {}
    for m in j2.get('materials', []):
        pbr = m.get('pbrMetallicRoughness') or {}
        bt = pbr.get('baseColorTexture')
        factor = pbr.get('baseColorFactor')
        if not bt:
            # A material with no texture and no baseColorFactor still has the
            # spec default white, which --bake-factors paints into the atlas.
            out.setdefault(m.get('name', ''), []).append(
                dict(factor=factor if factor is not None else [1.0, 1.0, 1.0, 1.0]))
            continue
        if bt:
            kt = (bt.get('extensions') or {}).get('KHR_texture_transform', {})
            tex = j2['textures'][bt['index']]
            src = tex['source']
            if src not in img_cache:
                img_cache[src] = image_bytes(j2, src, base2)
            out.setdefault(m.get('name', ''), []).append(dict(
                off=kt.get('offset', [0.0, 0.0]),
                scale=kt.get('scale', [1.0, 1.0]),
                rot=kt.get('rotation', 0.0),
                tc=kt.get('texCoord', bt.get('texCoord', 0)),
                wrap=sampler_wrap(j2, tex),
                img=img_cache[src]))
        else:
            out.setdefault(m.get('name', ''), []).append(dict(factor=factor))
    return out


def linear_to_srgb_byte(c):
    c = max(0.0, min(1.0, c))
    v = 12.92 * c if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055
    return int(round(v * 255))


def factor_image(f):
    rgb = [linear_to_srgb_byte(c) for c in f[:3]]
    return np.zeros((2, 2, 3), dtype=np.float64) + rgb


def base_orig():
    """Original basecolor image, loaded only when a texture-based material needs it."""
    global _orig
    if _orig is None:
        _orig = to_img(open(orig_path, 'rb').read())
    return _orig


_orig = None
transforms = tex_transforms(src_model_path)
rng = np.random.default_rng(7)
total_ok = total = 0
for p_idx, mesh in enumerate(j.get('meshes', [])):
    for pi, prim in enumerate(mesh['primitives']):
        attrs = prim['attributes']
        mat = j['materials'][prim['material']] if 'material' in prim else {}
        pbr = mat.get('pbrMetallicRoughness', {})
        if 'baseColorTexture' not in pbr:
            continue
        atlas_tc = pbr['baseColorTexture'].get('texCoord', 1)
        atlas_key = f'TEXCOORD_{atlas_tc}'
        if atlas_key not in attrs:
            print(f"mesh[{p_idx}] prim[{pi}]: no {atlas_key} — left unchanged, skipping")
            continue
        tf_list = transforms.get(mat.get('name', ''), [])
        tex_tfs = [t for t in tf_list if 'off' in t]
        if len({(json.dumps(t['off']), json.dumps(t['scale']), t['rot'], t['tc'])
                for t in tex_tfs}) > 1:
            print(f"mesh[{p_idx}] prim[{pi}]: ambiguous input transforms — using identity")
            tf_list = []
        tf = tf_list[0] if tf_list else None
        is_factor = bool(tf and 'factor' in tf)
        src_tc = 0 if is_factor else (tf['tc'] if tf else 0)
        src_key = f'TEXCOORD_{src_tc}'
        if src_key not in attrs:
            print(f"mesh[{p_idx}] prim[{pi}]: no {src_key} for source sampling — skipping")
            continue
        atlas = to_img(image_bytes(j, pbr['baseColorTexture']['index']))
        if is_factor:
            orig_arr = factor_image(tf['factor'])
            bcf = np.array(pbr.get('baseColorFactor', [1, 1, 1, 1]), dtype=float)
            if max(abs(bcf - 1)) > 1e-6:
                print(f"mesh[{p_idx}] prim[{pi}]: factor not neutralized on cloned material")
                sys.exit(1)
        else:
            orig_arr = to_img(tf['img']) if tf and tf.get('img') else base_orig()
        pos = read_accessor(j, attrs['POSITION'])
        uv0 = read_accessor(j, attrs[src_key])
        uv1 = read_accessor(j, attrs[atlas_key])
        if uv1[:, :2].min() < -1e-3 or uv1[:, :2].max() > 1 + 1e-3:
            print(f"mesh[{p_idx}] prim[{pi}]: FAIL atlas UVs outside [0,1]")
            sys.exit(1)
        idx = (read_accessor(j, prim['indices']).astype(int).ravel()
               if 'indices' in prim else np.arange(len(pos)))
        tris = idx.reshape(-1, 3)
        sel = rng.integers(0, len(tris), size=min(3000, max(len(tris), 300)))
        n_ok = n_tot = 0
        for t in sel:
            a, b, c = tris[t]
            r = np.sort(rng.random(2))
            w = np.array([1 - r[1], r[1] - r[0], r[0]])
            d1 = uv1[b, :2] - uv1[a, :2]
            d2 = uv1[c, :2] - uv1[a, :2]
            area_atlas = abs(d1[0] * d2[1] - d2[0] * d1[1]) / 2
            if area_atlas < 1e-8:
                continue
            ua = w[0] * uv1[a, :2] + w[1] * uv1[b, :2] + w[2] * uv1[c, :2]
            us = w[0] * uv0[a, :2] + w[1] * uv0[b, :2] + w[2] * uv0[c, :2]
            pa = sample_nearest(atlas, ua[0], ua[1], REPEAT_WRAP)
            su = us if is_factor else (apply_transform(us, tf['off'], tf['scale'], tf['rot'])
                                       if tf else us)
            # A baked factor has no source texture to wrap; an input model we
            # could not match leaves the mode implementation-defined.
            ps = sample_nearest(orig_arr, su[0], su[1],
                                tf['wrap'] if (tf and not is_factor) else REPEAT_WRAP)
            n_tot += 1
            if np.abs(pa - ps).max() < 40:
                n_ok += 1
        if n_tot:
            total_ok += n_ok
            total += n_tot
            print(f"mesh[{p_idx}] prim[{pi}]: agreement {n_ok}/{n_tot} ({100 * n_ok / n_tot:.1f}%)")

print(f"TOTAL: {total_ok}/{total} ({100 * total_ok / max(1, total):.1f}%)")
sys.exit(0 if total > 0 and total_ok >= 0.9 * total else 1)
