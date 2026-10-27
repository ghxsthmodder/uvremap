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
assert n >= 2, f'only {n} cloned materials have baseColorTexture'
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
done

echo
echo "verify: $PASS passed, $FAIL failed"
rm -rf "$WORK"
[ "$FAIL" = 0 ]
