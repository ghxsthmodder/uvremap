#!/usr/bin/env bash
# Regression against the private samples in samples/private/ (gitignored).
# These models are too large/licensed for the golden suite; run this manually
# after milestones that touch baking or export. MF188 peaks around 2.2 GB of
# RAM and takes roughly a minute per full scene bake.
# Usage: scripts/verify/run_private.sh [build-dir]
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-}/bin/uvremap"
[ -x "$BIN" ] || BIN="$ROOT/build/bin/uvremap"
PRIV="$ROOT/samples/private"
WORK="$(mktemp -d /tmp/uvremap-private.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
PASS=0; FAIL=0

ok()  { PASS=$((PASS+1)); echo "  PASS: $1"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL: $1"; }
expect() { # <desc> <expected-code> <cmd...>
    local desc="$1" want="$2"; shift 2
    "$@" >"$WORK/stdout" 2>"$WORK/stderr"; local got=$?
    if [ "$got" = "$want" ]; then ok "$desc (exit $got)"; else
        bad "$desc (exit $got, want $want)"; sed -n '1,6p' "$WORK/stderr" | sed 's/^/      /'; fi
}

if [ ! -x "$BIN" ]; then echo "private: binary not found at $BIN" >&2; exit 2; fi
if [ ! -d "$PRIV" ]; then echo "private: $PRIV not present; nothing to do"; exit 0; fi

MF188_GLB="$PRIV/MF188_0000_Default_l10n_configuration_onCekisiz_Default__168_.glb"
if [ ! -f "$MF188_GLB" ] || ! ls "$PRIV"/*.png >/dev/null 2>&1; then
    echo "private: MF188 model or its external textures missing; skipping"
else
    echo "== MF188 (scene mode, 18 materials, 2-page 2048x4096 atlas)"
    OUT="$WORK/mf188"
    expect "regenerate scene/gltf" 0 \
        "$BIN" regenerate "$MF188_GLB" -o "$OUT" --format gltf --textures external \
            --bake-factors --atlas-mode scene --self-check 512 --quiet
    [ -f "$OUT/report.txt" ] && ok "report.txt" || bad "report.txt missing"
    MODEL=$(find "$OUT" -maxdepth 1 -name '*.gltf' | head -1)
    [ -n "$MODEL" ] && expect "reparse output" 0 "$BIN" scan "$MODEL" || bad "no gltf in output"

    # M4.1: multi-material scene must bake into shared images only.
    SHARED=$(ls "$OUT/textures" | grep '^scene_0_' | grep -vc '_mat[0-9]')
    PERMAT=$(ls "$OUT/textures" | grep -c '_mat[0-9]')
    if [ "$PERMAT" = 0 ] && [ "$SHARED" -ge 4 ]; then
        ok "shared-by-default: $SHARED images, none per-material"
    else
        bad "shared-by-default: shared=$SHARED per-material=$PERMAT"
    fi
    # M4.3: every original texture of MF188 is rebuilt into the shared atlas, so
    # the pruned output must carry nothing but the baked images.
    NTX=$(ls "$OUT/textures" 2>/dev/null | wc -l)
    if [ "$NTX" = "$SHARED" ]; then ok "texture dir pruned to the $NTX baked atlases"
    else bad "texture dir has $NTX files, expected $SHARED"; fi
    if python3 "$ROOT/scripts/verify/check_orphans.py" "$OUT" >/dev/null 2>&1; then
        ok "no orphan material/texture/image/file in output"
    else
        bad "output still carries unreachable entries"
        python3 "$ROOT/scripts/verify/check_orphans.py" "$OUT" 2>&1 | sed -n '1,4p' |
            sed 's/^/      /'
    fi

    # Self-check agreement on basecolor must stay high (band-seam guard).
    BC=$(python3 - "$OUT/report.txt" <<'EOF'
import re, sys
m = re.search(r"basecolor: (\d+)/(\d+)", open(sys.argv[1]).read())
print(int(m.group(1)) / int(m.group(2)) if m else -1)
EOF
    )
    python3 -c "import sys; sys.exit(0 if float('$BC') >= 0.95 else 1)" \
        && ok "self-check basecolor agreement $BC" \
        || bad "self-check basecolor agreement $BC (<0.95)"
fi

echo
echo "private: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
