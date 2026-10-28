#!/usr/bin/env python3
"""Write a synthetic three-prim glTF whose materials use factors only (no images).

Usage: make_factor_model.py <output.gltf>
Prim A: linear baseColorFactor [0.8,0.05,0.05,1]; Prim B: [0.05,0.8,0.05,1]
plus metallic/roughness, emissive and occlusion-strength factors; Prim C: every
factor at the glTF default, so the atlas must carry its white too. Each prim is
a UV-covered quad sharing geometry layout (separate copies, distinct materials).
"""
import json
import struct
import sys

POS = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
UV = [(0, 0), (1, 0), (1, 1), (0, 1)]
IDX = [0, 1, 2, 0, 2, 3]


def pad4(n):
    return (4 - n % 4) % 4


def build():
    binchunks = []
    offset = 0

    def add(data):
        nonlocal offset
        bview = {'buffer': 0, 'byteOffset': offset, 'byteLength': len(data)}
        binchunks.append(data)
        offset += len(data) + pad4(len(data))
        binchunks.append(b'\x00' * pad4(len(data)))
        return bview

    pos_bytes = struct.pack('<12f', *[c for v in POS for c in v])
    uv_bytes = struct.pack('<8f', *[c for v in UV for c in v])
    idx_bytes = struct.pack('<6H', *IDX)

    bv_pos = add(pos_bytes)
    bv_uv = add(uv_bytes)
    bv_idx = add(idx_bytes)

    accessors = [
        {'bufferView': 0, 'componentType': 5126, 'count': 4, 'type': 'VEC3',
         'min': [0.0, 0.0, 0.0], 'max': [1.0, 1.0, 0.0]},
        {'bufferView': 1, 'componentType': 5126, 'count': 4, 'type': 'VEC2'},
        {'bufferView': 2, 'componentType': 5123, 'count': 6, 'type': 'SCALAR'},
    ]
    attrs = {'POSITION': 0, 'TEXCOORD_0': 1}

    materials = [
        {'name': 'FactorRed', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.8, 0.05, 0.05, 1.0]}},
        {'name': 'FactorGreen', 'doubleSided': True,
         'pbrMetallicRoughness': {'baseColorFactor': [0.05, 0.8, 0.05, 1.0],
                                  'metallicFactor': 0.25, 'roughnessFactor': 0.4},
         'emissiveFactor': [0.0, 0.2, 0.0]},
        {'name': 'FactorWhite', 'doubleSided': True,
         'pbrMetallicRoughness': {}},
    ]
    # occlusion strength needs an occlusionTexture entry to exist in glTF, so
    # material 1 also gets the extension-free default; strength is expressed
    # via KHR_materials_* is not applicable — keep the three core factors.

    prims = []
    for m in (0, 1, 2):
        prims.append({'attributes': dict(attrs), 'indices': 2, 'material': m, 'mode': 4})

    gltf = {
        'asset': {'version': '2.0'},
        'scene': 0,
        'scenes': [{'nodes': [0, 1, 2]}],
        'nodes': [{'mesh': 0}, {'mesh': 1}, {'mesh': 2}],
        'meshes': [{'name': 'quadA', 'primitives': [prims[0]]},
                   {'name': 'quadB', 'primitives': [prims[1]]},
                   {'name': 'quadC', 'primitives': [prims[2]]}],
        'materials': materials,
        'accessors': accessors,
        'bufferViews': [bv_pos, bv_uv, bv_idx],
        'buffers': [{'byteLength': offset, 'uri': 'factor_model.bin'}],
    }
    return gltf, b''.join(binchunks)


def main():
    out = sys.argv[1]
    import os
    d = os.path.dirname(out)
    if d:
        os.makedirs(d, exist_ok=True)
    gltf, blob = build()
    with open(out, 'w') as f:
        json.dump(gltf, f, indent=2)
    bin_name = out.rsplit('.', 1)[0] + '.bin'
    with open(bin_name, 'wb') as f:
        f.write(blob)
    print(f'wrote {out} + {bin_name}')


if __name__ == '__main__':
    main()
