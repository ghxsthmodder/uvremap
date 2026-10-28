#!/usr/bin/env bash
# Golden verification of uvremap against Khronos glTF-Sample-Assets models.
# Usage: scripts/verify/run_samples.sh [build-dir]
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-}/bin/uvremap"
[ -x "$BIN" ] || BIN="$ROOT/build/bin/uvremap"
SAMPLES="$ROOT/samples/Models"
WORK="$(mktemp -d /tmp/uvremap-verify.XXXXXX)"
PASS=0; FAIL=0

ok()   { PASS=$((PASS+1)); echo "  PASS: $1"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL: $1"; }
expect() { # <desc> <expected-code> <cmd...>
    local desc="$1" want="$2"; shift 2
    "$@" >"$WORK/stdout" 2>"$WORK/stderr"; local got=$?
    if [ "$got" = "$want" ]; then ok "$desc (exit $got)"; else
        bad "$desc (exit $got, want $want)"; sed -n '1,6p' "$WORK/stderr" | sed 's/^/      /'; fi
}

run_model() { # <name> <input> <expect-regen:0|4>
    local name="$1" input="$2" regen_exit="$3"
    echo "== $name"
    [ -f "$input" ] || { bad "$name: input missing"; return; }
    expect "scan"        0 "$BIN" scan "$input"
    expect "inspect"     0 "$BIN" inspect "$input"
    expect "dry-run"     "$regen_exit" "$BIN" regenerate "$input" -o "$WORK/$name-dry" --dry-run --atlas-mode material
    for mode in material mesh scene; do
        for fmt in gltf glb; do
            local out="$WORK/$name-$mode-$fmt"
            expect "regenerate $mode/$fmt" "$regen_exit" \
                "$BIN" regenerate "$input" -o "$out" --atlas-mode "$mode" --format "$fmt" --quiet
            if [ "$regen_exit" = 0 ]; then
                local model
                model=$(find "$out" -maxdepth 1 -name "*.gltf" -o -maxdepth 1 -name "*.glb" | head -1)
                if [ -n "$model" ]; then ok "$mode/$fmt: model written"; expect "$mode/$fmt: reparse" 0 "$BIN" scan "$model";
                else bad "$mode/$fmt: no model in output"; fi
                [ -f "$out/report.txt" ] && ok "$mode/$fmt: report.txt" || bad "$mode/$fmt: report.txt missing"
            fi
        done
    done
    # refusal to overwrite without --force (use first successful regen output)
    local first="$WORK/$name-material-gltf"
    if [ -d "$first" ] && [ "$regen_exit" = 0 ]; then
        local ext=gltf; [[ "$input" == *.glb ]] && ext=glb
        expect "overwrite refused" 6 "$BIN" regenerate "$input" -o "$first" --atlas-mode material --format gltf
        expect "overwrite forced"  0 "$BIN" regenerate "$input" -o "$first" --atlas-mode material --format gltf --force --quiet
    fi
}

# exit-code behavior
expect "missing input -> 3" 3 "$BIN" scan "$WORK/does-not-exist.glb"
expect "bad args -> 2" 2 "$BIN" regenerate "$SAMPLES/Box/glTF-Binary/Box.glb" -o "$WORK/badargs"
expect "help/version -> 0" 0 "$BIN" version

run_model BoomBox        "$SAMPLES/BoomBox/glTF-Binary/BoomBox.glb"                            0
run_model SimpleTexture  "$SAMPLES/SimpleTexture/glTF-Embedded/SimpleTexture.gltf"             0
run_model AlphaBlendMode "$SAMPLES/AlphaBlendModeTest/glTF-Binary/AlphaBlendModeTest.glb"      0
run_model MetalRough     "$SAMPLES/MetalRoughSpheres/glTF-Binary/MetalRoughSpheres.glb"        0
# OrientationTest has no TEXCOORDs (factor-only materials) -> exit 4 per README
run_model Orientation    "$SAMPLES/OrientationTest/glTF-Binary/OrientationTest.glb"            4
run_model NormalTangent  "$SAMPLES/NormalTangentTest/glTF-Binary/NormalTangentTest.glb"        0
run_model BoxTextured    "$SAMPLES/BoxTextured/glTF-Binary/BoxTextured.glb"                    0
# Shoe: KHR_materials_variants remapped since v0.1.5 (M1.2) -> regenerates
run_model Shoe           "$SAMPLES/MaterialsVariantsShoe/glTF-Binary/MaterialsVariantsShoe.glb" 0
# variant mappings must survive export pointing at per-variant baked textures
if python3 "$ROOT/scripts/verify/check_variants.py" \
        "$WORK/Shoe-material-glb/MaterialsVariantsShoe.glb" >/dev/null 2>&1; then
    ok "shoe: variant mappings + per-variant textures"
else
    bad "shoe: variant mappings + per-variant textures"
    python3 "$ROOT/scripts/verify/check_variants.py" \
        "$WORK/Shoe-material-glb/MaterialsVariantsShoe.glb" 2>&1 | tail -3 | sed 's/^/      /'
fi
if python3 "$ROOT/scripts/verify/check_variants.py" \
        "$WORK/Shoe-scene-glb/MaterialsVariantsShoe.glb" >/dev/null 2>&1; then
    ok "shoe scene: variant mappings + per-variant textures"
else
    bad "shoe scene: variant mappings + per-variant textures"
fi
# KHR_texture_transform rotation models: rebuilt since v0.1.3 (M1.1)
run_model TexTransformMulti "$SAMPLES/TextureTransformMultiTest/glTF-Binary/TextureTransformMultiTest.glb" 0

# draco/meshopt compressed meshes: refused with a clear message (M1.3), never garbled
for comp in DragonAttenuation/glTF-Meshopt/DragonAttenuation.gltf \
            DragonAttenuation/glTF-Meshopt-EXT/DragonAttenuation.gltf \
            VirtualCity/glTF-Draco/VirtualCity.gltf \
            CarConcept/glTF-KTX-BasisU-Draco/CarConcept.gltf; do
    "$BIN" scan "$SAMPLES/$comp" >/dev/null 2>"$WORK/comp.err"
    got=$?
    if [ "$got" = 3 ] && grep -q "compressed geometry" "$WORK/comp.err"; then
        ok "compressed refused cleanly: $comp"
    else
        bad "compressed refused cleanly: $comp (exit $got)"; sed -n '1,3p' "$WORK/comp.err" | sed 's/^/      /'
    fi
done

# input immutability: hash all sample inputs before/after is impractical here; check one:
SRC="$SAMPLES/BoomBox/glTF-Binary/BoomBox.glb"
H1=$(sha256sum "$SRC" | cut -d' ' -f1)
"$BIN" regenerate "$SRC" -o "$WORK/boom-imm" --atlas-mode scene --quiet >/dev/null
H2=$(sha256sum "$SRC" | cut -d' ' -f1)
[ "$H1" = "$H2" ] && ok "input file unchanged" || bad "input file modified!"

# texture transfer accuracy on the BoomBox glTF sample (atlas UV kept in TEXCOORD_1)
"$BIN" regenerate "$SAMPLES/BoomBox/glTF/BoomBox.gltf" -o "$WORK/boom-t1" --atlas-mode material \
    --format gltf --target-uv 1 --quiet >/dev/null \
    && python3 "$ROOT/scripts/verify/check_bake.py" "$WORK/boom-t1/BoomBox.gltf" \
        "$SAMPLES/BoomBox/glTF/BoomBox_baseColor.png" >/dev/null 2>&1 \
    && ok "bake agreement (check_bake.py)" || bad "bake agreement (check_bake.py)"

# KHR_texture_transform (rotation/scale/offset) accuracy on the conformance model:
# atlas into TEXCOORD_2 so both source sets survive; nearest filter for exact compare.
TT="$SAMPLES/TextureTransformMultiTest/glTF/TextureTransformMultiTest.gltf"
"$BIN" regenerate "$TT" -o "$WORK/ttm-t2" --atlas-mode material \
    --format gltf --target-uv 2 --texture-filter nearest --quiet >/dev/null \
    && python3 "$ROOT/scripts/verify/check_bake.py" "$WORK/ttm-t2/TextureTransformMultiTest.gltf" \
        "$SAMPLES/TextureTransformMultiTest/glTF/TestMap.png" "$TT" >/dev/null 2>&1 \
    && ok "rotation bake agreement (check_bake.py)" || bad "rotation bake agreement (check_bake.py)"

# M2.1: real multi-page atlas — high texel density against a small fixed
# resolution overflows into sub-atlases; the baked texture stacks the pages
# vertically into one tall image and the mapping must still agree.
MP="$SAMPLES/DamagedHelmet/glTF/DamagedHelmet.gltf"
if "$BIN" regenerate "$MP" -o "$WORK/multipage" --atlas-mode scene \
        --resolution 1024x1024 --texels-per-unit 400 --target-uv 1 --quiet >/dev/null \
   && python3 - "$WORK/multipage" <<'EOF'
import re, sys
from PIL import Image
w = sys.argv[1]
m = re.search(r'charts needed (\d+) pages', open(w + '/report.txt').read())
assert m, 'no multi-page note in report'
n = int(m.group(1))
assert n >= 2, f'expected overflow, got {n} page(s)'
sw, sh = Image.open(w + '/textures/scene_0_basecolor.png').size
assert sh == sw * n, f'stacked image {sw}x{sh} != {n} pages of {sw}'
EOF
then
    ok "multi-page: stacked image height == pages x width"
else
    bad "multi-page: stacked image height == pages x width"
fi
python3 "$ROOT/scripts/verify/check_bake.py" "$WORK/multipage/DamagedHelmet.gltf" \
    "$SAMPLES/DamagedHelmet/glTF/Default_albedo.jpg" "$MP" >/dev/null 2>&1 \
    && ok "multi-page bake agreement (check_bake.py)" || bad "multi-page bake agreement (check_bake.py)"

# M2.2: non-square --resolution — charts packed square are renormalized to the
# requested W x H; the baked page width/height must match and transfer hold.
"$BIN" regenerate "$MP" -o "$WORK/nonsquare" --atlas-mode scene \
    --resolution 2048x1536 --target-uv 1 --quiet >/dev/null \
    && python3 - "$WORK/nonsquare" <<'EOF'
import sys
from PIL import Image
w = sys.argv[1]
assert 'renormalized to 2048x1536' in open(w + '/report.txt').read(), 'no renormalize note'
sw, sh = Image.open(w + '/textures/scene_0_basecolor.png').size
assert sw == 2048 and sh >= 1536 and sh % 1536 == 0, f'page size {sw}x{sh} not W x k*H'
EOF
if [ $? -eq 0 ]; then ok "non-square: baked pages are 2048 x k*1536"
else bad "non-square: baked pages are 2048 x k*1536"; fi
python3 "$ROOT/scripts/verify/check_bake.py" "$WORK/nonsquare/DamagedHelmet.gltf" \
    "$SAMPLES/DamagedHelmet/glTF/Default_albedo.jpg" "$MP" >/dev/null 2>&1 \
    && ok "non-square bake agreement (check_bake.py)" || bad "non-square bake agreement (check_bake.py)"

# M3.2: --self-check N — the CLI itself samples baked texels against the
# source and reports agreement per rebuilt image; normal maps are skipped.
expect "self-check exit 0" 0 \
    "$BIN" regenerate "$MP" -o "$WORK/selfcheck" --atlas-mode scene --self-check 2000 --quiet
if grep -q "self-check 'scene_0' basecolor: [0-9]*/2000" "$WORK/selfcheck/report.txt" 2>/dev/null; then
    ok "self-check: agreement line in report"
else
    bad "self-check: agreement line in report"
fi
grep -q "self-check.*normal: skipped" "$WORK/selfcheck/report.txt" 2>/dev/null \
    && ok "self-check: normal map marked skipped" || bad "self-check: normal map marked skipped"
expect "self-check bad value" 2 "$BIN" regenerate "$MP" -o "$WORK/selfcheck2" --self-check 0

# M5.4: --fail-under N — the floor is a gate, not a warning: below it the
# output is never published and the run exits with the transfer-failure code.
expect "fail-under: breach exits 5" 5 \
    "$BIN" regenerate "$MP" -o "$WORK/floor-bad" --atlas-mode scene --self-check 500 \
        --fail-under 99.9 --quiet
[ ! -d "$WORK/floor-bad" ] && ok "fail-under: breach publishes nothing" \
    || bad "fail-under: breach publishes nothing"
expect "fail-under: floor met exits 0" 0 \
    "$BIN" regenerate "$MP" -o "$WORK/floor-ok" --atlas-mode scene --self-check 500 \
        --fail-under 50 --quiet
grep -q "floor met" "$WORK/floor-ok/report.txt" 2>/dev/null \
    && ok "fail-under: floor recorded in report" || bad "fail-under: floor recorded in report"
expect "fail-under without self-check" 2 \
    "$BIN" regenerate "$MP" -o "$WORK/floor-arg" --atlas-mode scene --fail-under 90
expect "fail-under out of range" 2 \
    "$BIN" regenerate "$MP" -o "$WORK/floor-arg2" --atlas-mode scene --self-check 10 --fail-under 0

# M3.3: --preview emits diagnostic PNGs (atlas layout, UV compare, heatmaps)
png_dims() { # <file> <w> <h>
    python3 - "$1" "$2" "$3" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read(24)
assert d[:8] == b'\x89PNG\r\n\x1a\n', 'not a png'
w, h = struct.unpack('>II', d[16:24])
assert (w, h) == (int(sys.argv[2]), int(sys.argv[3])), '%dx%d' % (w, h)
PY
}
expect "preview exit 0" 0 \
    "$BIN" regenerate "$MP" -o "$WORK/preview" --atlas-mode scene --preview --quiet
if [ -d "$WORK/preview" ]; then
    png_dims "$WORK/preview/scene_0_atlas_preview.png" 1024 1024 \
        && ok "preview: atlas layout png" || bad "preview: atlas layout png"
    png_dims "$WORK/preview/scene_0_uv_compare.png" 2064 1024 \
        && ok "preview: uv compare png" || bad "preview: uv compare png"
    png_dims "$WORK/preview/scene_0_heatmaps.png" 2064 1024 \
        && ok "preview: heatmaps png" || bad "preview: heatmaps png"
else
    bad "preview: output dir missing"
fi

# M3.4: --json writes report.json and prints the report as JSON on stdout
expect "json exit 0" 0 \
    "$BIN" regenerate "$MP" -o "$WORK/jsonout" --atlas-mode scene --json
if python3 - "$WORK/jsonout/report.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert d['tool'] == 'uvremap' and 'stats' in d
assert isinstance(d['warnings'], list) and 'warningCount' in d
PY
then ok "json: report.json parses"; else bad "json: report.json parses"; fi
# M5.3: every atlas publishes how much of it the charts use and the texel
# density that footprint implies.
if python3 - "$WORK/jsonout/report.json" <<'PY'
import json, sys
u = json.load(open(sys.argv[1]))['atlasUsage']
assert u, 'no atlasUsage entries'
for x in u:
    assert x['width'] > 0 and x['height'] > 0 and x['pages'] >= 1
    assert 0.0 < x['coverage'] <= 1.0
    assert x['texelsPerUnit'] > 0.0 and x['surfaceArea'] > 0.0
PY
then ok "json: atlasUsage occupancy + density"; else bad "json: atlasUsage occupancy + density"; fi
if grep -q 'Atlas usage:' "$WORK/jsonout/report.txt" &&
   grep -Eq 'scene_0: [0-9]+x[0-9]+ x[0-9]+ page\(s\), [0-9.]+% covered, [0-9.]+ texels per unit' \
       "$WORK/jsonout/report.txt"; then
    ok "report.txt: atlas usage line"
else
    bad "report.txt: atlas usage line"
fi
if python3 - "$WORK/stdout" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert d['stats']['atlasCount'] >= 1
PY
then ok "json: stdout is pure JSON"; else bad "json: stdout is pure JSON"; fi

# texture placement option: gltf embedded (data URIs, no textures dir) vs external
EMB="$WORK/boom-texembed"
BBI="$SAMPLES/BoomBox/glTF/BoomBox.gltf"
expect "textures embedded (gltf) exit 0" 0 \
    "$BIN" regenerate "$BBI" -o "$EMB" --atlas-mode material --format gltf --textures embedded --quiet
if [ -d "$EMB" ]; then
    grep -q 'data:image' "$EMB/BoomBox.gltf" && ok "embedded: data URIs present" || bad "embedded: no data URIs"
    [ ! -d "$EMB/textures" ] && ok "embedded: no textures dir" || bad "embedded: textures dir created"
    expect "embedded: reparse" 0 "$BIN" scan "$EMB/BoomBox.gltf"
fi
expect "external + glb rejected" 2 \
    "$BIN" regenerate "$BBI" -o "$WORK/texext-glb" --atlas-mode material --format glb --textures external

# GLB output must carry a real BIN chunk (buffer in binary), not a data-URI buffer in JSON
"$BIN" regenerate "$BBI" -o "$WORK/glbchunk" --atlas-mode material --format glb --quiet >/dev/null \
    && python3 - "$WORK/glbchunk/BoomBox.glb" <<'PY' >/dev/null 2>&1
import struct, sys
d = open(sys.argv[1], 'rb').read()
magic, ver, total = struct.unpack('<4sII', d[:12])
assert magic == b'glTF' and ver == 2 and total == len(d), 'bad header'
off, chunks = 12, []
while off < len(d):
    clen, ctype = struct.unpack('<I4s', d[off:off + 8])
    chunks.append(ctype)
    off += 8 + clen
assert b'BIN\x00' in chunks, 'missing BIN chunk'
assert b'JSON' in chunks, 'missing JSON chunk'
import json
joff = 12 + 8
jlen, = struct.unpack('<I', d[12:16])
g = json.loads(d[joff:joff + jlen])
assert all('uri' not in b for b in g.get('buffers', [])), 'buffer has uri in GLB'
PY
if [ $? -eq 0 ]; then ok "glb: real BIN chunk, buffer without uri"
else bad "glb: real BIN chunk, buffer without uri"; fi
expect "invalid --textures value" 2 \
    "$BIN" regenerate "$BBI" -o "$WORK/texbogus" --atlas-mode material --textures bogus

# --bake-factors on a synthetic texture-less model (M1.4): factors fold into
# rebuilt textures and the cloned materials end up factor-neutral.
FACT="$WORK/factor/factor_model.gltf"
mkdir -p "$(dirname "$FACT")"
if python3 "$ROOT/scripts/verify/make_factor_model.py" "$FACT" > /dev/null 2>&1; then
    ok "factor model generated"
else
    bad "factor model generation failed"
fi
expect "factors kept by default (material)" 0 \
    "$BIN" regenerate "$FACT" -o "$WORK/factor-off" --atlas-mode material --quiet
if grep -q '"baseColorTexture"' "$WORK/factor-off/factor_model.gltf"; then
    bad "factors kept by default: texture appeared"
else
    ok "factors kept by default: no textures added"
fi
expect "bake-factors (material)" 0 \
    "$BIN" regenerate "$FACT" -o "$WORK/factor-mat" --atlas-mode material --bake-factors \
        --texture-filter nearest --target-uv 1 --quiet
expect "bake-factors (scene)" 0 \
    "$BIN" regenerate "$FACT" -o "$WORK/factor-scene" --atlas-mode scene --bake-factors \
        --texture-filter nearest --target-uv 1 --quiet
for variant in mat scene; do
    FO="$WORK/factor-$variant/factor_model.gltf"
    if [ -f "$FO" ] && python3 - "$FO" <<'EOF'
import json, sys
j = json.load(open(sys.argv[1]))
mats = j['materials']
n = len([m for m in mats if m['pbrMetallicRoughness'].get('baseColorTexture')])
# every material of the model is texture-less, so with --bake-factors all of
# them must move their color into the atlas, the default-white one included
assert n == len(mats), f'{n} of {len(mats)} cloned materials have baseColorTexture'
for m in mats:
    pbr = m['pbrMetallicRoughness']
    if pbr.get('baseColorTexture'):
        f = pbr.get('baseColorFactor', [1, 1, 1, 1])
        assert all(abs(x - 1) < 1e-6 for x in f), f'factor not neutralized: {f}'
        assert pbr.get('metallicFactor', 1.0) == 1.0, 'metallic not neutralized'
        assert pbr.get('roughnessFactor', 1.0) == 1.0, 'roughness not neutralized'
        ef = m.get('emissiveFactor', [0, 0, 0])
        assert all(v == 0.0 for v in ef), 'emissive not neutralized'
print('ok')
EOF
    then
        ok "bake-factors ($variant): textures + neutral factors"
    else
        bad "bake-factors ($variant): textures + neutral factors"
    fi
    if [ -f "$FO" ] && python3 "$ROOT/scripts/verify/check_bake.py" "$FO" \
            "$WORK/factor/factor_model_baseColor_unused.png" "$FACT" >/dev/null 2>&1; then
        ok "bake-factors ($variant): painted colors match factors"
    else
        bad "bake-factors ($variant): painted colors match factors"
    fi
    # M4.5: the material whose factors are all spec defaults must still have
    # its white painted; before, only non-neutral factors were folded and its
    # islands stayed on the atlas' neutral transparent fill.
    if python3 - "$WORK/factor-$variant/textures" <<'EOF'
import glob, sys
import numpy as np
from PIL import Image
hits = 0
for f in glob.glob(sys.argv[1] + '/*_basecolor*.png'):
    a = np.array(Image.open(f).convert('RGBA'))
    hits += int(((a[..., 0] > 250) & (a[..., 1] > 250) &
                 (a[..., 2] > 250) & (a[..., 3] > 250)).sum())
assert hits > 1000, f'default factor painted on {hits} texels'
EOF
    then
        ok "bake-factors ($variant): spec-default factor painted too"
    else
        bad "bake-factors ($variant): spec-default factor missing from the atlas"
    fi
done

# M4.1: multi-material scene bakes into shared images by default (one per
# texture type); --split-materials restores the per-material layout.
expect "bake-factors (split)" 0 \
    "$BIN" regenerate "$FACT" -o "$WORK/factor-split" --atlas-mode scene --bake-factors \
        --split-materials --texture-filter nearest --target-uv 1 --quiet
if [ -d "$WORK/factor-scene/textures" ] &&
        ! ls "$WORK/factor-scene/textures" | grep -q '_mat[0-9]'; then
    ok "shared-by-default: no per-material files"
else
    bad "shared-by-default: per-material files appeared"
fi
if ls "$WORK/factor-split/textures" 2>/dev/null | grep -q '_mat0' &&
        ls "$WORK/factor-split/textures" | grep -q '_mat1'; then
    ok "split-materials: per-material files restored"
else
    bad "split-materials: per-material files missing"
fi

# M4.2: textures inside material extensions are copied as-is and keep sampling
# their original TEXCOORD set; the atlas moves to a free set unless the user
# pinned --target-uv.
EXTUV="$SAMPLES/TextureTransformMultiTest/glTF/TextureTransformMultiTest.gltf"
expect "ext-uv auto-move" 0 "$BIN" regenerate "$EXTUV" -o "$WORK/ext-auto" \
    --atlas-mode scene --quiet
if grep -q "atlas mapping was moved to the free TEXCOORD" "$WORK/ext-auto/report.txt"; then
    ok "ext-uv: atlas moved off the set sampled by extensions"
else
    bad "ext-uv: no auto-move reported"
fi
if [ -f "$WORK/ext-auto/TextureTransformMultiTest.gltf" ] && python3 - "$EXTUV" \
        "$WORK/ext-auto/TextureTransformMultiTest.gltf" <<'EOF'
import json, sys
from collections import Counter
src = json.load(open(sys.argv[1]))
out = json.load(open(sys.argv[2]))
atlas = max(int(k.split('_')[1]) for p in out['meshes'] for pr in p['primitives']
            for k in pr['attributes'] if k.startswith('TEXCOORD_'))
assert atlas >= 1, 'atlas set missing'
def ext_texcoords(doc):
    c = Counter()
    for m in doc.get('materials', []):
        for en, ev in m.get('extensions', {}).items():
            if not en.startswith('KHR_materials_'):
                continue
            for k, v in ev.items():
                if isinstance(v, dict) and 'index' in v:
                    c[v.get('texCoord', 0)] += 1
    return c
a, b = ext_texcoords(src), ext_texcoords(out)
assert a, 'sample has no extension textures'
# materials are cloned per atlased primitive, so compare the multiset of sets
assert set(b) == set(a), f'extension texCoord sets changed: {dict(a)} vs {dict(b)}'
assert atlas not in b, 'extension samples the atlas set'
std = [t.get('texCoord', 0) for m in out['materials']
       for grp in (m.get('pbrMetallicRoughness', {}), m)
       for slot in ('baseColorTexture', 'metallicRoughnessTexture', 'normalTexture',
                    'occlusionTexture', 'emissiveTexture')
       for t in [grp.get(slot)] if isinstance(t, dict) and 'index' in t]
assert atlas in std, 'rebaked slots do not sample the atlas set'
print('ok')
EOF
    then
        ok "ext-uv: extension texCoords preserved, atlas uses a free set"
    else
        bad "ext-uv: extension UV wiring broken"
    fi
expect "ext-uv pinned" 0 "$BIN" regenerate "$EXTUV" -o "$WORK/ext-pin" \
    --atlas-mode scene --target-uv 1 --quiet
if grep -q "referenced by a material extension" "$WORK/ext-pin/report.txt"; then
    ok "ext-uv: pinned --target-uv warns instead of moving"
else
    bad "ext-uv: pinned --target-uv did not warn"
fi
if grep -q "material extensions sample" "$WORK/factor-scene/report.txt"; then
    bad "ext-uv: false positive without extensions"
else
    ok "ext-uv: models without extensions keep target UV"
fi

# M4.3: the atlas rewrite must not leave unreachable materials, textures,
# images or files behind.
expect "prune: report line" 0 "$BIN" regenerate "$TT" -o "$WORK/prune" \
    --atlas-mode scene --format gltf --quiet
if grep -q "no primitive references after the atlas rewrite" "$WORK/prune/report.txt"; then
    ok "prune: reported in report.txt"
else
    bad "prune: nothing reported"
fi
for dir in prune ext-auto factor-scene factor-split; do
    if python3 "$ROOT/scripts/verify/check_orphans.py" "$WORK/$dir" >/dev/null 2>&1; then
        ok "prune: $dir has no orphan material/texture/image/file"
    else
        bad "prune: $dir still carries unreachable entries"
        python3 "$ROOT/scripts/verify/check_orphans.py" "$WORK/$dir" 2>&1 |
            sed -n '1,4p' | sed 's/^/      /'
    fi
done
# Extension texture references survive the renumbering: the set of image files
# reachable from KHR_materials_* must stay exactly the input's.
if [ -f "$WORK/ext-auto/TextureTransformMultiTest.gltf" ] && python3 - "$EXTUV" \
        "$WORK/ext-auto/TextureTransformMultiTest.gltf" <<'EOF'
import json, os, sys
def ext_uris(path):
    d = json.load(open(path))
    base = os.path.dirname(path)
    uris = []
    for m in d.get('materials', []):
        for en, ev in m.get('extensions', {}).items():
            if not en.startswith('KHR_materials_'):
                continue
            for v in ev.values():
                if isinstance(v, dict) and isinstance(v.get('index'), int):
                    uri = d['images'][d['textures'][v['index']]['source']].get('uri', '')
                    if not uri.startswith('data:'):
                        uris.append(os.path.basename(uri))
                    assert os.path.exists(os.path.join(base, uri)), f'missing {uri}'
    return sorted(uris)
a = ext_uris(sys.argv[1])
b = ext_uris(sys.argv[2])
assert a, 'sample has no extension textures'
assert a == b, f'extension textures changed: {a} vs {b}'
assert not [u for u in b if u.startswith('scene_0')], 'extension points at baked atlas'
print('ok')
EOF
    then
        ok "prune: extension texture indices still resolve to the original images"
    else
        bad "prune: extension texture indices broken"
    fi

echo
echo "verify: $PASS passed, $FAIL failed"
rm -rf "$WORK"
[ "$FAIL" = 0 ]
