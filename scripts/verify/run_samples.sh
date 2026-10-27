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
# Shoe: KHR_materials_variants primitive is left unchanged -> exit 4 (nothing regenerable)
run_model Shoe           "$SAMPLES/MaterialsVariantsShoe/glTF-Binary/MaterialsVariantsShoe.glb" 4

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
expect "invalid --textures value" 2 \
    "$BIN" regenerate "$BBI" -o "$WORK/texbogus" --atlas-mode material --textures bogus

echo
echo "verify: $PASS passed, $FAIL failed"
rm -rf "$WORK"
[ "$FAIL" = 0 ]
