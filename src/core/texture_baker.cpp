#include "core/texture_baker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>

#include "core/normal_math.h"

namespace uvremap {

void neutralTexel(TexType t, uint8_t out[4]) {
    switch (t) {
        case TexType::BaseColor: out[0] = out[1] = out[2] = 0; out[3] = 0; break;
        case TexType::MetallicRoughness: out[0] = 0; out[1] = 255; out[2] = 0; out[3] = 255; break;
        case TexType::Occlusion: out[0] = 255; out[1] = 0; out[2] = 0; out[3] = 255; break;
        case TexType::Emissive: out[0] = out[1] = out[2] = 0; out[3] = 255; break;
        case TexType::Normal: out[0] = 128; out[1] = 128; out[2] = 255; out[3] = 255; break;
        default: out[0] = out[1] = out[2] = out[3] = 0; break;
    }
}

namespace {

// Factor-baking helpers: a texture-less slot with a non-neutral factor can be
// painted into the atlas when --bake-factors is set, so the exported material
// carries appearance in the texture alone.
double linearToSrgb(double c) {
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

uint8_t linearToByte(double c) { return uint8_t(std::clamp(linearToSrgb(c), 0.0, 1.0) * 255.0 + 0.5); }
uint8_t directToByte(double c) { return uint8_t(std::clamp(c, 0.0, 1.0) * 255.0 + 0.5); }

bool nearly(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// A texture-less slot whose island must carry its material's value even when
// that value is the glTF default: the shared raster fills with transparent
// black (basecolor) and roughness 0 (metallic-roughness), so leaving the
// default material unpainted would punch a hole in the rebuilt atlas. Not
// emissive or occlusion: painting their defaults would create a map for a
// material that never had one.
bool identityPaintSlot(TexType t) {
    return t == TexType::BaseColor || t == TexType::MetallicRoughness;
}

// Fill value for a texture-less slot, or false when there is nothing to bake:
// either the factor is neutral and paintNeutral is off, or the slot has no
// foldable factor at all.
bool factorTexel(const Material& m, TexType t, uint8_t out[4], bool paintNeutral) {
    switch (t) {
        case TexType::BaseColor: {
            const V4& f = m.baseColorFactor;
            out[0] = linearToByte(f.r);
            out[1] = linearToByte(f.g);
            out[2] = linearToByte(f.b);
            out[3] = directToByte(f.a);
            if (nearly(f.r, 1) && nearly(f.g, 1) && nearly(f.b, 1) && nearly(f.a, 1))
                return paintNeutral;
            return true;
        }
        case TexType::MetallicRoughness: {
            out[0] = 0;
            out[1] = directToByte(m.metallicFactor);
            out[2] = directToByte(m.roughnessFactor);
            out[3] = 255;
            // glTF spec defaults (1, 1) need no baking.
            if (nearly(m.metallicFactor, 1) && nearly(m.roughnessFactor, 1)) return paintNeutral;
            return true;
        }
        case TexType::Occlusion:
            // occlusionStrength only modulates an existing occlusion texture;
            // without one there is nothing to fold, and adding a texture where
            // the material had none would change shading.
            return false;
        case TexType::Emissive: {
            out[0] = linearToByte(m.emissiveFactor.x);
            out[1] = linearToByte(m.emissiveFactor.y);
            out[2] = linearToByte(m.emissiveFactor.z);
            out[3] = 255;
            if (nearly(m.emissiveFactor.x, 0) && nearly(m.emissiveFactor.y, 0) &&
                nearly(m.emissiveFactor.z, 0))
                return false;
            return true;
        }
        default:
            return false;  // normal maps have no factor
    }
}

// Source UV for a triangle corner, honoring KHR_texture_transform.
// Spec order: uv' = offset + R(rotation) * (uv * scale) — GLSL in
// KHR_texture_transform README: matrix = translation * rotation * scale.
V2 transformUv(V2 uv, const TexTransform& x) {
    if (x.identity) return uv;
    double u = uv.u * x.scaleU, v = uv.v * x.scaleV;
    if (x.rotation != 0.0) {
        double c = std::cos(x.rotation), s = std::sin(x.rotation);
        double ru = u * c - v * s, rv = u * s + v * c;
        u = ru;
        v = rv;
    }
    return V2{float(u + x.offsetU), float(v + x.offsetV)};
}

bool barycentric(const float p[2], const float v0[2], const float v1[2], const float v2[2],
                 float w[3]) {
    float d00 = v1[0] - v0[0], d01 = v2[0] - v0[0];
    float d10 = v1[1] - v0[1], d11 = v2[1] - v0[1];
    float det = d00 * d11 - d10 * d01;
    if (std::fabs(det) < 1e-20f) return false;
    float px = p[0] - v0[0], py = p[1] - v0[1];
    float inv = 1.f / det;
    w[1] = (px * d11 - py * d01) * inv;
    w[2] = (py * d00 - px * d10) * inv;
    w[0] = 1.f - w[1] - w[2];
    return true;
}

struct PerType {
    const Image* image = nullptr;  // source image for this triangle's material
    const TexTransform* xform = nullptr;
    Wrap wrapS = Wrap::Repeat, wrapT = Wrap::Repeat;  // sampler of the source texture
    int uvSet = -1;                // source TEXCOORD index, -1 = unavailable
    V2 su[3];                      // transformed source UVs, new-triangle corner order
    TriFrame srcFrame;
    bool useFrame = false;         // valid for normal maps
};

}  // namespace

bool bakeGroup(const Model& model, const AtlasGroup& group, const AtlasGen& gen,
               const BakeOptions& opt, std::vector<BakedImage>& outBaked, Report& report,
               std::string& err) {
    const int texW = opt.textureW > 0 ? opt.textureW : gen.width;
    const int texH = opt.textureH > 0 ? opt.textureH : gen.height;
    if (texW <= 0 || texH <= 0) {
        err = "invalid atlas/texture size";
        return false;
    }
    const float scaleX = float(texW) / float(gen.width);
    const float scaleY = float(texH) / float(gen.height);

    // Candidate materials per primitive: the authored one plus every material
    // reachable through KHR_materials_variants mappings.
    std::map<int, std::vector<int>> cand;
    std::vector<int> matIds;
    bool anyMulti = false;
    for (int pid : group.primitives) {
        const Primitive& p = model.primitives[pid];
        std::vector<int>& cs = cand[pid];
        auto add = [&](int m) {
            if (m < 0 || m >= int(model.materials.size())) return;
            if (std::find(cs.begin(), cs.end(), m) == cs.end()) cs.push_back(m);
        };
        add(p.material);
        for (const VariantMapping& vm : p.variantMappings) add(vm.material);
        if (cs.size() > 1) anyMulti = true;
        for (int m : cs)
            if (std::find(matIds.begin(), matIds.end(), m) == matIds.end()) matIds.push_back(m);
    }

    // Per-material slot needs: decodable texture, or (with --bake-factors) a
    // non-neutral factor for a texture-less slot.
    bool typeNeeded[kTexCount];
    std::memset(typeNeeded, 0, sizeof(typeNeeded));
    std::map<int, std::array<bool, kTexCount>> perMatNeed;
    std::map<int, int> perMatFactorBits;
    std::map<int, std::array<std::array<uint8_t, 4>, kTexCount>> perMatFacRgb;
    for (int mat : matIds) {
        auto& needs = perMatNeed[mat];
        needs.fill(false);
        perMatFacRgb[mat].fill({0, 0, 0, 0});
        const Material& m = model.materials[mat];
        bool warnedDecode = false;
        for (int t = 0; t < kTexCount; t++) {
            const TextureRef& ref = m.tex[t];
            if (!ref.present) {
                if (opt.bakeFactors) {
                    uint8_t f[4];
                    if (factorTexel(m, TexType(t), f, identityPaintSlot(TexType(t)))) {
                        needs[t] = true;
                        perMatFactorBits[mat] |= factorBit(TexType(t));
                        for (int c = 0; c < 4; c++) perMatFacRgb[mat][t][c] = f[c];
                    }
                }
                continue;
            }
            if (ref.imageIndex < 0 || ref.imageIndex >= int(model.images.size()) ||
                !model.images[ref.imageIndex].decoded) {
                if (!typeNeeded[t] && !warnedDecode) {
                    report.warn(std::string("atlas '") + group.name + "': " +
                                texTypeName(TexType(t)) +
                                " texture could not be decoded; that slot will not be rebuilt");
                    warnedDecode = true;
                }
                continue;
            }
            needs[t] = true;
            typeNeeded[t] = true;
        }
    }

    // Rasters: one shared set (material -1) plus per-material sets when any
    // primitive carries multiple candidate materials (variants). Every set has
    // one layer per atlas page, allocated lazily on first touch and stacked
    // vertically into one image per (material, type) at emission.
    const int pages = std::max(1, gen.pageCount);
    std::vector<std::vector<Raster>> sharedLayers(pages, std::vector<Raster>(kTexCount));
    auto ensureLayer = [&](Raster& r, TexType t) {
        if (!r.rgba.empty()) return;
        r.resize(texW, texH);
        uint8_t n[4];
        neutralTexel(t, n);
        r.fill(n[0], n[1], n[2], n[3]);
    };
    // Dedicated per-material layers are used only when variant candidates share
    // atlas regions, or when --split-materials asks for one image per material.
    // Factor-baked materials otherwise paint inline on the shared layer, at
    // exact triangle coverage, so their flat color never touches a neighbour's
    // texels.
    bool perMatUsed = false;
    if (anyMulti) {
        perMatUsed = true;
        for (int pid : group.primitives) {
            const std::vector<int>& cs = cand[pid];
            if (cs.size() <= 1) continue;
            for (int m : cs) {
                bool any = false;
                for (int t = 0; t < kTexCount; t++)
                    any = any || perMatNeed[m][t];
                if (!any) perMatUsed = false;
            }
        }
    }
    if (opt.splitMaterials) perMatUsed = true;
    std::map<int, std::vector<std::vector<Raster>>> perMatLayers;  // mat -> page -> layers
    auto matPage = [&](int mat) -> std::vector<std::vector<Raster>>& {
        std::vector<std::vector<Raster>>& pl = perMatLayers[mat];
        if (pl.empty()) pl.assign(pages, std::vector<Raster>(kTexCount));
        return pl;
    };
    // Texel-coverage masks for factor painting on dedicated layers, keyed
    // (material, page).
    std::map<std::pair<int, int>, std::vector<uint8_t>> fillMasks;
    // Which materials the shared layer carries a painted factor for, per slot.
    // The exporter needs this per material: the union in the emitted
    // factorBaked bits cannot tell an island that holds its color from one
    // that was never stamped.
    std::map<int, int> sharedFactorMats;

    int trianglesUnmapped = 0;
    int trianglesDegenerate = 0;  // zero-area faces xatlas legitimately ignores

    for (const GenMesh& gm : gen.meshes) {
        const Primitive& prim = model.primitives[gm.primId];
        size_t uvSets = prim.vertices.empty() ? 0 : prim.vertices[0].uvsets.size();
        for (size_t f = 0; f + 2 < gm.indices.size(); f += 3) {
            uint32_t i0 = gm.indices[f], i1 = gm.indices[f + 1], i2 = gm.indices[f + 2];
            const GenVertex& g0 = gm.vertices[i0];
            const GenVertex& g1 = gm.vertices[i1];
            const GenVertex& g2 = gm.vertices[i2];
            if (g0.atlasIndex < 0 || g1.atlasIndex < 0 || g2.atlasIndex < 0) {
                // xatlas drops faces whose 3D or UV area is ~0; they cover no
                // visible pixels, so do not treat them as a transfer failure.
                const V3& a = prim.vertices[g0.xref].position;
                const V3& b = prim.vertices[g1.xref].position;
                const V3& c = prim.vertices[g2.xref].position;
                float e1[3] = {b.x - a.x, b.y - a.y, b.z - a.z};
                float e2[3] = {c.x - a.x, c.y - a.y, c.z - a.z};
                float cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                               e1[0] * e2[1] - e1[1] * e2[0]};
                float area3d = 0.5f * std::sqrt(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
                if (area3d <= FLT_EPSILON)
                    trianglesDegenerate++;
                else
                    trianglesUnmapped++;
                continue;
            }
            SourceTri st;
            if (!gen.findSource(gm.primId, gm, i0, i1, i2, st)) {
                trianglesUnmapped++;
                continue;
            }
            // Match new-triangle corners to source-triangle corners. Chart xrefs
            // may name a colocal twin of the corner the source triangle indexes,
            // so compare canonicalized on both sides; sampling still uses the
            // source triangle's own (raw) corner indices via the slot number.
            uint32_t xref[3] = {gen.canonical(gm.primId, g0.xref),
                                gen.canonical(gm.primId, g1.xref),
                                gen.canonical(gm.primId, g2.xref)};
            uint32_t sc[3] = {gen.canonical(gm.primId, st.v0),
                              gen.canonical(gm.primId, st.v1),
                              gen.canonical(gm.primId, st.v2)};
            int cornerOf[3] = {-1, -1, -1};
            for (int a = 0; a < 3; a++)
                for (int b = 0; b < 3; b++)
                    if (xref[a] == sc[b]) cornerOf[a] = b;
            if (cornerOf[0] < 0 || cornerOf[1] < 0 || cornerOf[2] < 0) {
                trianglesUnmapped++;
                continue;
            }

            V3 dpos[3] = {prim.vertices[g0.xref].position, prim.vertices[g1.xref].position,
                          prim.vertices[g2.xref].position};
            V2 duv[3] = {{g0.u, g0.v}, {g1.u, g1.v}, {g2.u, g2.v}};
            TriFrame dstFrame;
            bool haveDstFrame = triangleFrame(dpos, duv, dstFrame);

            // All three corners share one chart, hence one page.
            const int page = g0.atlasIndex;

            // Stamp this triangle once per candidate material of the primitive.
            static const std::vector<int> kNoMats;
            auto csIt = cand.find(gm.primId);
            const std::vector<int>& candMats =
                csIt != cand.end() ? csIt->second : kNoMats;

            float tv0[2] = {g0.u * scaleX, g0.v * scaleY};
            float tv1[2] = {g1.u * scaleX, g1.v * scaleY};
            float tv2[2] = {g2.u * scaleX, g2.v * scaleY};
            int minX = int(std::floor(std::min({tv0[0], tv1[0], tv2[0]})));
            int maxX = int(std::ceil(std::max({tv0[0], tv1[0], tv2[0]})));
            int minY = int(std::floor(std::min({tv0[1], tv1[1], tv2[1]})));
            int maxY = int(std::ceil(std::max({tv0[1], tv1[1], tv2[1]})));
            minX = std::max(minX, 0);
            minY = std::max(minY, 0);
            maxX = std::min(maxX, texW - 1);
            maxY = std::min(maxY, texH - 1);

            // Flat-factor paints extend 0.5px beyond the triangle edges so
            // abutting charts of the same material form no hairline seams on
            // the shared layer. Per-edge value of w[c] in pixels is h[c]
            // (triangle height over edge c), so w[c] >= -0.5/h[c] means the
            // pixel is within half a pixel outside edge c.
            float facTol[3] = {0.f, 0.f, 0.f};
            {
                const float area2 = std::fabs((tv1[0] - tv0[0]) * (tv2[1] - tv0[1]) -
                                              (tv2[0] - tv0[0]) * (tv1[1] - tv0[1]));
                if (area2 > 1e-12f)
                    for (int c = 0; c < 3; c++) {
                        const float* q = c == 0 ? tv1 : (c == 1 ? tv2 : tv0);
                        const float* r = c == 0 ? tv2 : (c == 1 ? tv0 : tv1);
                        float elen = std::hypot(q[0] - r[0], q[1] - r[1]);
                        if (elen > 1e-9f) facTol[c] = std::min(0.5f * elen / area2, 1e4f);
                    }
            }

            for (int mat : candMats) {
                const Material& m = model.materials[mat];
                PerType pt[kTexCount];
                Raster* target[kTexCount];
                bool isFactor[kTexCount];
                uint8_t facRgb[kTexCount][4];
                bool anyActive = false;
                for (int t = 0; t < kTexCount; t++) {
                    target[t] = nullptr;
                    isFactor[t] = false;
                    if (!perMatNeed[mat][t]) continue;
                    Raster& layer = perMatUsed ? matPage(mat)[page][t] : sharedLayers[page][t];
                    if (perMatFactorBits[mat] & factorBit(TexType(t))) {
                        ensureLayer(layer, TexType(t));
                        target[t] = &layer;
                        if (!perMatUsed) {
                            isFactor[t] = true;  // paint inline on the shared layer
                            sharedFactorMats[mat] |= factorBit(TexType(t));
                            for (int c = 0; c < 4; c++)
                                facRgb[t][c] = perMatFacRgb[mat][t][c];
                        }
                        anyActive = true;
                        continue;
                    }
                    const TextureRef& ref = m.tex[t];
                    if (ref.imageIndex < 0 || !model.images[ref.imageIndex].decoded) continue;
                    PerType& p = pt[t];
                    p.image = &model.images[ref.imageIndex];
                    p.xform = &ref.transform;
                    p.wrapS = ref.wrapS;
                    p.wrapT = ref.wrapT;
                    int uvSet = ref.texCoord;
                    if (size_t(uvSet) >= uvSets) uvSet = opt.sourceUv;
                    if (size_t(uvSet) >= uvSets) continue;  // no coordinates to sample with
                    p.uvSet = uvSet;
                    V2 raw[3] = {prim.vertices[st.v0].uvsets[uvSet],
                                 prim.vertices[st.v1].uvsets[uvSet],
                                 prim.vertices[st.v2].uvsets[uvSet]};
                    for (int c = 0; c < 3; c++)
                        p.su[c] = transformUv(raw[cornerOf[c]], *p.xform);
                    if (t == int(TexType::Normal)) {
                        V3 spos[3] = {prim.vertices[st.v0].position, prim.vertices[st.v1].position,
                                      prim.vertices[st.v2].position};
                        p.useFrame =
                            triangleFrame(spos, p.su, p.srcFrame) && haveDstFrame;
                    }
                    // Shared layer when every prim has one material; per-material
                    // layer when variants make several materials share regions.
                    ensureLayer(layer, TexType(t));
                    target[t] = &layer;
                    anyActive = true;
                }
                if (!anyActive) continue;

                // Flat-factor paints also accept pixels just outside the
                // triangle edges (0.5px band), so charts of the same material
                // meeting at a shared edge leave no hairline gaps.
                bool wantBand = false;
                if (!perMatUsed)
                    for (int t = 0; t < kTexCount; t++)
                        wantBand = wantBand || isFactor[t];

                // Track coverage for the factor final-pass painting, which only
                // applies to dedicated per-material layers.
                if (perMatUsed && perMatFactorBits[mat]) {
                    std::vector<uint8_t>& mask = fillMasks[{mat, page}];
                    if (mask.empty()) mask.assign(size_t(texW) * texH, 0);
                    for (int y = minY; y <= maxY; y++)
                        std::fill(mask.begin() + size_t(y) * texW + minX,
                                  mask.begin() + size_t(y) * texW + maxX + 1, uint8_t(1));
                }

                for (int y = minY; y <= maxY; y++) {
                    uint8_t* rowBase[kTexCount];
                    for (int t = 0; t < kTexCount; t++)
                        rowBase[t] = target[t] && !target[t]->rgba.empty()
                                         ? target[t]->rgba.data() + size_t(y) * texW * 4
                                         : nullptr;
                    for (int x = minX; x <= maxX; x++) {
                        float p[2] = {x + 0.5f, y + 0.5f};
                        float w[3];
                        if (!barycentric(p, tv0, tv1, tv2, w)) continue;
                        bool strict = w[0] >= -1e-6f && w[1] >= -1e-6f && w[2] >= -1e-6f;
                        if (!strict) {
                            if (!wantBand) continue;
                            if (w[0] < -facTol[0] || w[1] < -facTol[1] || w[2] < -facTol[2])
                                continue;
                        }
                        for (int t = 0; t < kTexCount; t++) {
                            if (!rowBase[t]) continue;
                            if (isFactor[t]) {
                                uint8_t* dst = rowBase[t] + size_t(x) * 4;
                                dst[0] = facRgb[t][0];
                                dst[1] = facRgb[t][1];
                                dst[2] = facRgb[t][2];
                                dst[3] = facRgb[t][3];
                                continue;
                            }
                            if (!strict) continue;  // texture sampling keeps exact coverage
                            if (!pt[t].image || pt[t].uvSet < 0) continue;
                            V2 suv{w[0] * pt[t].su[0].u + w[1] * pt[t].su[1].u +
                                       w[2] * pt[t].su[2].u,
                                   w[0] * pt[t].su[0].v + w[1] * pt[t].su[1].v +
                                       w[2] * pt[t].su[2].v};
                            float px[4];
                            sampleBytes(pt[t].image->w, pt[t].image->h, pt[t].image->rgba.data(),
                                        suv.u, suv.v, opt.filter, pt[t].wrapS, pt[t].wrapT, px);
                            uint8_t* dst = rowBase[t] + size_t(x) * 4;
                            if (t == int(TexType::Normal)) {
                                if (!pt[t].useFrame) continue;
                                V3 nts{px[0] * 2.f - 1.f, px[1] * 2.f - 1.f, px[2] * 2.f - 1.f};
                                V3 n = rebakeNormal(pt[t].srcFrame, dstFrame, nts);
                                dst[0] = uint8_t(std::clamp(n.x * .5f + .5f, 0.f, 1.f) * 255.f + .5f);
                                dst[1] = uint8_t(std::clamp(n.y * .5f + .5f, 0.f, 1.f) * 255.f + .5f);
                                dst[2] = uint8_t(std::clamp(n.z * .5f + .5f, 0.f, 1.f) * 255.f + .5f);
                                dst[3] = 255;
                            } else {
                                dst[0] = uint8_t(std::clamp(px[0], 0.f, 1.f) * 255.f + .5f);
                                dst[1] = uint8_t(std::clamp(px[1], 0.f, 1.f) * 255.f + .5f);
                                dst[2] = uint8_t(std::clamp(px[2], 0.f, 1.f) * 255.f + .5f);
                                dst[3] = uint8_t(std::clamp(px[3], 0.f, 1.f) * 255.f + .5f);
                            }
                        }
                    }
                }
            }
        }
    }

    // Paint the flat factors into the covered texels of every page they were
    // stamped on.
    for (auto& mk : fillMasks) {
        int matKey = mk.first.first;
        int page = mk.first.second;
        std::vector<uint8_t>& mask = mk.second;
        for (int t = 0; t < kTexCount; t++) {
            if (!(perMatFactorBits[matKey] & factorBit(TexType(t)))) continue;
            const uint8_t* fc = perMatFacRgb[matKey][t].data();
            Raster& ls = matPage(matKey)[page][t];
            ensureLayer(ls, TexType(t));
            for (size_t i = 0; i < mask.size(); i++) {
                if (!mask[i]) continue;
                uint8_t* d = ls.rgba.data() + i * 4;
                d[0] = fc[0];
                d[1] = fc[1];
                d[2] = fc[2];
                d[3] = fc[3];
            }
        }
    }

    if (trianglesUnmapped > 0)
        report.warn("atlas '" + group.name + "': " + std::to_string(trianglesUnmapped) +
                    " triangle(s) could not be mapped back to source geometry; those texels "
                    "keep neutral values");
    if (trianglesDegenerate > 0)
        report.info("atlas '" + group.name + "': " + std::to_string(trianglesDegenerate) +
                    " zero-area source triangle(s) left out of the atlas (invisible)");
    if (gen.duplicateTriKeys > 0)
        report.warn("atlas '" + group.name + "': " + std::to_string(gen.duplicateTriKeys) +
                    " duplicated source triangle(s) (same corners, different UVs possible); "
                    "first occurrence used");

    // Emit one image per (material, type): all pages stacked vertically,
    // untouched pages left neutral-filled. Each image is PNG-encoded
    // immediately and its raw pixels dropped: a scene-mode atlas can carry
    // dozens of full-size images, and holding them decoded exhausts memory
    // on Windows (bad allocation) long before Linux's overcommit would.
    bool emitOk = true;
    auto emit = [&](int mat, TexType t, const std::vector<Raster*>& pageLayers, int bits,
                    const std::vector<int>& facMats) {
        if (!emitOk) return;
        bool any = false;
        for (Raster* r : pageLayers)
            if (r && !r->rgba.empty()) any = true;
        if (!any) return;
        Image out;
        out.w = texW;
        out.h = texH * pages;
        out.name = group.name + "_" + texTypeName(t);
        if (mat >= 0) out.name += "_mat" + std::to_string(mat);
        uint8_t n[4];
        neutralTexel(t, n);
        out.rgba.assign(size_t(out.w) * out.h * 4, 0);
        for (size_t i = 0; i < out.rgba.size(); i += 4) {
            out.rgba[i] = n[0];
            out.rgba[i + 1] = n[1];
            out.rgba[i + 2] = n[2];
            out.rgba[i + 3] = n[3];
        }
        for (int p = 0; p < pages; p++) {
            const Raster* r = pageLayers[p];
            if (!r || r->rgba.empty()) continue;
            std::memcpy(out.rgba.data() + size_t(p) * texW * texH * 4, r->rgba.data(),
                        size_t(texW) * texH * 4);
        }
        Raster r;
        r.w = out.w;
        r.h = out.h;
        r.rgba.swap(out.rgba);
        std::vector<uint8_t> png;
        if (!encodePng(r, png)) {
            emitOk = false;
            return;
        }
        out.raw = std::move(png);
        out.decoded = false;  // out.rgba now empty; decode out.raw when needed
        outBaked.push_back(BakedImage{mat, t, std::move(out), bits, facMats});
    };

    if (perMatUsed) {
        for (int mat : matIds) {
            auto plIt = perMatLayers.find(mat);
            if (plIt == perMatLayers.end()) continue;
            for (int t = 0; t < kTexCount; t++) {
                std::vector<Raster*> ls{size_t(pages)};
                for (int p = 0; p < pages; p++) ls[p] = &plIt->second[p][t];
                std::vector<int> facMats;
                if (perMatFactorBits[mat] & factorBit(TexType(t))) facMats.push_back(mat);
                emit(mat, TexType(t), ls, perMatFactorBits[mat], facMats);
            }
        }
    }
    for (int t = 0; t < kTexCount; t++) {
        std::vector<Raster*> ls{size_t(pages)};
        for (int p = 0; p < pages; p++) ls[p] = &sharedLayers[p][t];
        int bits = 0;
        std::vector<int> facMats;
        for (const auto& fb : sharedFactorMats) {
            if (!(fb.second & factorBit(TexType(t)))) continue;
            bits |= factorBit(TexType(t));
            facMats.push_back(fb.first);
        }
        emit(-1, TexType(t), ls, bits, facMats);
    }
    if (!emitOk) {
        err = "failed to encode baked texture '" + group.name + "' to PNG";
        return false;
    }
    return true;
}

bool selfCheckGroup(const Model& model, const AtlasGroup& group, const AtlasGen& gen,
                    const BakeOptions& opt, const std::vector<BakedImage>& baked, int nSamples,
                    Report& report) {
    if (nSamples <= 0) return true;
    const int texW = opt.textureW > 0 ? opt.textureW : gen.width;
    const int texH = opt.textureH > 0 ? opt.textureH : gen.height;
    if (texW <= 0 || texH <= 0) return true;
    const float scaleX = float(texW) / float(gen.width);
    const float scaleY = float(texH) / float(gen.height);

    // Candidate materials per primitive (mirror of bakeGroup).
    std::map<int, std::vector<int>> cand;
    for (int pid : group.primitives) {
        const Primitive& p = model.primitives[pid];
        std::vector<int>& cs = cand[pid];
        auto add = [&](int m) {
            if (m < 0 || m >= int(model.materials.size())) return;
            if (std::find(cs.begin(), cs.end(), m) == cs.end()) cs.push_back(m);
        };
        add(p.material);
        for (const VariantMapping& vm : p.variantMappings) add(vm.material);
    }

    // Triangles that map to a source triangle with all corners matched.
    struct Tri {
        int gmIdx;
        size_t f;
        SourceTri st;
        int cornerOf[3];
    };
    std::vector<Tri> tris;
    for (size_t mi = 0; mi < gen.meshes.size(); mi++) {
        const GenMesh& gm = gen.meshes[mi];
        for (size_t f = 0; f + 2 < gm.indices.size(); f += 3) {
            uint32_t i0 = gm.indices[f], i1 = gm.indices[f + 1], i2 = gm.indices[f + 2];
            const GenVertex& g0 = gm.vertices[i0];
            const GenVertex& g1 = gm.vertices[i1];
            const GenVertex& g2 = gm.vertices[i2];
            if (g0.atlasIndex < 0 || g1.atlasIndex < 0 || g2.atlasIndex < 0) continue;
            Tri t;
            t.gmIdx = int(mi);
            t.f = f;
            if (!gen.findSource(gm.primId, gm, i0, i1, i2, t.st)) continue;
            uint32_t xref[3] = {gen.canonical(gm.primId, g0.xref),
                                gen.canonical(gm.primId, g1.xref),
                                gen.canonical(gm.primId, g2.xref)};
            uint32_t sc[3] = {gen.canonical(gm.primId, t.st.v0),
                              gen.canonical(gm.primId, t.st.v1),
                              gen.canonical(gm.primId, t.st.v2)};
            t.cornerOf[0] = t.cornerOf[1] = t.cornerOf[2] = -1;
            for (int a = 0; a < 3; a++)
                for (int b = 0; b < 3; b++)
                    if (xref[a] == sc[b]) t.cornerOf[a] = b;
            if (t.cornerOf[0] < 0 || t.cornerOf[1] < 0 || t.cornerOf[2] < 0) continue;
            tris.push_back(t);
        }
    }

    // Weight the sample pick by each triangle's footprint in the atlas. A
    // triangle that collapses to a sliver holds no representative texel, and
    // drawing triangles uniformly spends samples on gutter pixels instead.
    std::vector<double> triWeight;
    triWeight.reserve(tris.size());
    double totalWeight = 0.0;
    for (const Tri& tri : tris) {
        const GenMesh& gm = gen.meshes[tri.gmIdx];
        const GenVertex* gv[3] = {&gm.vertices[gm.indices[tri.f]],
                                  &gm.vertices[gm.indices[tri.f + 1]],
                                  &gm.vertices[gm.indices[tri.f + 2]]};
        float dx1 = gv[1]->u * scaleX - gv[0]->u * scaleX;
        float dy1 = gv[1]->v * scaleY - gv[0]->v * scaleY;
        float dx2 = gv[2]->u * scaleX - gv[0]->u * scaleX;
        float dy2 = gv[2]->v * scaleY - gv[0]->v * scaleY;
        double area = 0.5 * std::fabs(dx1 * dy2 - dy1 * dx2);
        triWeight.push_back(area);
        totalWeight += area;
    }

    for (const BakedImage& b : baked) {
        const int t = int(b.type);
        if (b.type == TexType::Normal) {
            report.info("self-check '" + group.name + "' normal: skipped (tangent-frame rebake "
                        "not verifiable by texel compare)");
            continue;
        }
        if (b.image.w != texW || b.image.h < texH) continue;
        if (tris.empty()) {
            report.info("self-check '" + group.name + "' " + texTypeName(b.type) +
                        ": no mappable triangles");
            continue;
        }
        // Baked images are stored PNG-encoded; decode one at a time on demand.
        Raster decoded;
        if (b.image.rgba.empty()) {
            std::string derr;
            if (!decodeImage(b.image.raw, decoded, derr)) {
                report.warn("self-check '" + group.name + "': cannot decode baked " +
                            texTypeName(b.type) + " image");
                continue;
            }
        }
        const std::vector<uint8_t>& atlasPx =
            b.image.rgba.empty() ? decoded.rgba : b.image.rgba;
        if (totalWeight <= 0.0) {
            report.info("self-check '" + group.name + "' " + texTypeName(b.type) +
                        ": no area-mappable triangles");
            continue;
        }
        std::mt19937 rng(7u + size_t(b.material) * 131u + size_t(t) * 17u);
        std::discrete_distribution<size_t> pick(triWeight.begin(), triWeight.end());
        std::uniform_real_distribution<float> urand(0.f, 1.f);
        int ok = 0, n = 0, skip = 0;
        for (int s = 0; s < nSamples; s++) {
            const Tri& tri = tris[pick(rng)];
            const GenMesh& gm = gen.meshes[tri.gmIdx];
            const Primitive& prim = model.primitives[gm.primId];
            const GenVertex* gv[3] = {&gm.vertices[gm.indices[tri.f]],
                                      &gm.vertices[gm.indices[tri.f + 1]],
                                      &gm.vertices[gm.indices[tri.f + 2]]};
            // Which material's textures were sampled into this region?
            const std::vector<int>& cs = cand[gm.primId];
            int mat = b.material;
            if (mat < 0) {
                if (cs.empty()) continue;
                mat = prim.material >= 0 && std::find(cs.begin(), cs.end(), prim.material) !=
                                                        cs.end()
                          ? prim.material
                          : cs[0];
            } else if (std::find(cs.begin(), cs.end(), mat) == cs.end()) {
                continue;  // prim not stamped into this material's copy
            }
            const Material& m = model.materials[mat];
            const TextureRef& ref = m.tex[t];

            float r1 = urand(rng), r2 = urand(rng);
            if (r2 < r1) std::swap(r1, r2);
            const float w[3] = {1.f - r2, r2 - r1, r1};

            // Baked atlas texel (pages stack vertically).
            float au = 0.f, av = 0.f;
            for (int c = 0; c < 3; c++) {
                au += w[c] * gv[c]->u;
                av += w[c] * gv[c]->v;
            }
            int px = int(std::clamp(au * scaleX, 0.f, float(texW - 1)));
            int py = int(std::clamp(av * scaleY, 0.f, float(texH - 1)));
            py += std::max(0, gv[0]->atlasIndex) * texH;
            if (py >= b.image.h) continue;

            // The bake wrote one source sample per atlas texel, taken at the
            // texel centre, so the expectation has to be read at that same
            // centre: comparing an arbitrary point inside the texel against a
            // chart minified below its source resolution can never agree.
            float ew[3] = {w[0], w[1], w[2]};
            {
                const float x0 = gv[0]->u * scaleX, y0 = gv[0]->v * scaleY;
                const float rx = float(px) + 0.5f - x0;
                const float ry = float(py - std::max(0, gv[0]->atlasIndex) * texH) + 0.5f - y0;
                const float dx1 = gv[1]->u * scaleX - x0, dy1 = gv[1]->v * scaleY - y0;
                const float dx2 = gv[2]->u * scaleX - x0, dy2 = gv[2]->v * scaleY - y0;
                const float det = dx1 * dy2 - dy1 * dx2;
                if (std::fabs(det) > 1e-12f) {
                    float b1 = (rx * dy2 - ry * dx2) / det;
                    float b2 = (dx1 * ry - dy1 * rx) / det;
                    float b0 = 1.f - b1 - b2;
                    if (b0 >= -1e-3f && b1 >= -1e-3f && b2 >= -1e-3f) {
                        ew[0] = b0;
                        ew[1] = b1;
                        ew[2] = b2;
                    }
                }
            }
            const uint8_t* bk = &atlasPx[(size_t(py) * b.image.w + px) * 4];

            // Expected source texel.
            float exp[4];
            bool have = false;
            if (ref.present && ref.imageIndex >= 0 &&
                size_t(ref.imageIndex) < model.images.size() &&
                model.images[ref.imageIndex].decoded) {
                const Image& src = model.images[ref.imageIndex];
                int uvSet = ref.texCoord;
                size_t uvSets = prim.vertices.empty() ? 0 : prim.vertices[0].uvsets.size();
                if (size_t(uvSet) >= uvSets) uvSet = opt.sourceUv;
                if (size_t(uvSet) < uvSets) {
                    V2 suv{0.f, 0.f};
                    for (int c = 0; c < 3; c++) {
                        const SourceVertex& sv =
                            prim.vertices[std::min<size_t>(
                                tri.cornerOf[c] == 0 ? tri.st.v0
                                : tri.cornerOf[c] == 1 ? tri.st.v1
                                                       : tri.st.v2,
                                prim.vertices.size() - 1)];
                        V2 raw = sv.uvsets[uvSet];
                        V2 st = transformUv(raw, ref.transform);
                        suv.u += ew[c] * st.u;
                        suv.v += ew[c] * st.v;
                    }
                    float got[4];
                    sampleBytes(src.w, src.h, src.rgba.data(), suv.u, suv.v, Filter::Nearest,
                                ref.wrapS, ref.wrapT, got);
                    for (int i = 0; i < 4; i++) exp[i] = got[i] * 255.f;
                    have = true;
                }
            } else {
                uint8_t fc[4];
                if (opt.bakeFactors && factorTexel(m, b.type, fc, identityPaintSlot(b.type))) {
                    for (int i = 0; i < 4; i++) exp[i] = fc[i];
                    have = true;
                }
            }
            if (!have) {
                // Nothing was expected under this texel: with --bake-factors
                // every island carries at least its material's default value,
                // so a non-zero count here means an atlas region was left on
                // the neutral fill while its material's slot was reassigned.
                skip++;
                continue;
            }
            n++;
            bool agree = true;
            for (int i = 0; i < 4 && agree; i++)
                if (std::fabs(float(bk[i]) - exp[i]) >= 40.f) agree = false;
            if (agree) ok++;
        }
        std::string tag = "self-check '" + group.name + "' " + texTypeName(b.type);
        if (b.material >= 0) tag += " (mat " + std::to_string(b.material) + ")";
        if (n == 0) {
            report.info(tag + ": no covered samples");
            continue;
        }
        float pct = 100.f * float(ok) / float(n);
        std::string line = tag + ": " + std::to_string(ok) + "/" + std::to_string(n) + " (" +
                           [&] {
                               std::ostringstream os;
                               os << std::fixed << std::setprecision(1) << pct;
                               return os.str();
                           }() +
                           "%)";
        if (skip > 0)
            line += "; " + std::to_string(skip) + " sample(s) had no value to compare";
        report.info(line);
        if (pct < 85.f)
            report.warn(tag + ": low agreement " + std::to_string(int(pct)) +
                        "% — texture transfer may be inaccurate");
    }
    return true;
}

}  // namespace uvremap
