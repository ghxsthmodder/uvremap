#include "core/texture_baker.h"

#include <algorithm>
#include <cmath>
#include <cfloat>

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
    int uvSet = -1;                // source TEXCOORD index, -1 = unavailable
    V2 su[3];                      // transformed source UVs, new-triangle corner order
    TriFrame srcFrame;
    bool useFrame = false;         // valid for normal maps
};

}  // namespace

bool bakeGroup(const Model& model, const AtlasGroup& group, const AtlasGen& gen,
               const BakeOptions& opt,
               std::vector<std::pair<TexType, Image>>& outBaked, Report& report,
               std::string& err) {
    const int texW = opt.textureW > 0 ? opt.textureW : gen.width;
    const int texH = opt.textureH > 0 ? opt.textureH : gen.height;
    if (texW <= 0 || texH <= 0) {
        err = "invalid atlas/texture size";
        return false;
    }
    const float scaleX = float(texW) / float(gen.width);
    const float scaleY = float(texH) / float(gen.height);

    std::vector<int> matIds;
    for (int pid : group.primitives) {
        int mat = model.primitives[pid].material;
        if (mat < 0) continue;
        if (std::find(matIds.begin(), matIds.end(), mat) == matIds.end())
            matIds.push_back(mat);
    }

    bool needed[kTexCount] = {};
    for (int mat : matIds) {
        const Material& m = model.materials[mat];
        for (int t = 0; t < kTexCount; t++) {
            const TextureRef& ref = m.tex[t];
            if (!ref.present) continue;
            if (ref.imageIndex < 0 || ref.imageIndex >= int(model.images.size()) ||
                !model.images[ref.imageIndex].decoded) {
                if (!needed[t])
                    report.warn(std::string("atlas '") + group.name + "': " + texTypeName(TexType(t)) +
                                " texture could not be decoded; that slot will not be rebuilt");
                continue;
            }
            needed[t] = true;
        }
    }

    std::vector<Raster> layers(kTexCount);
    for (int t = 0; t < kTexCount; t++) {
        if (!needed[t]) continue;
        layers[t].resize(texW, texH);
        uint8_t n[4];
        neutralTexel(TexType(t), n);
        layers[t].fill(n[0], n[1], n[2], n[3]);
    }

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

            const int mat = prim.material;
            PerType pt[kTexCount];
            bool anyActive = false;
            if (mat >= 0) {
                const Material& m = model.materials[mat];
                for (int t = 0; t < kTexCount; t++) {
                    if (!needed[t]) continue;
                    const TextureRef& ref = m.tex[t];
                    if (!ref.present) continue;
                    if (ref.imageIndex < 0 || !model.images[ref.imageIndex].decoded) continue;
                    pt[t].image = &model.images[ref.imageIndex];
                    pt[t].xform = &ref.transform;
                    int uvSet = ref.texCoord;
                    if (size_t(uvSet) >= uvSets) uvSet = opt.sourceUv;
                    if (size_t(uvSet) >= uvSets) continue;  // no coordinates to sample with
                    pt[t].uvSet = uvSet;
                    V2 raw[3] = {prim.vertices[st.v0].uvsets[uvSet],
                                 prim.vertices[st.v1].uvsets[uvSet],
                                 prim.vertices[st.v2].uvsets[uvSet]};
                    for (int c = 0; c < 3; c++)
                        pt[t].su[c] = transformUv(raw[cornerOf[c]], *pt[t].xform);
                    anyActive = true;
                }
            }
            if (!anyActive) continue;

            V3 dpos[3] = {prim.vertices[g0.xref].position, prim.vertices[g1.xref].position,
                          prim.vertices[g2.xref].position};
            V2 duv[3] = {{g0.u, g0.v}, {g1.u, g1.v}, {g2.u, g2.v}};
            TriFrame dstFrame;
            bool haveDstFrame = triangleFrame(dpos, duv, dstFrame);

            if (needed[int(TexType::Normal)] && pt[int(TexType::Normal)].uvSet >= 0) {
                PerType& pn = pt[int(TexType::Normal)];
                V3 spos[3] = {prim.vertices[st.v0].position, prim.vertices[st.v1].position,
                              prim.vertices[st.v2].position};
                pn.useFrame = triangleFrame(spos, pn.su, pn.srcFrame) && haveDstFrame;
            }

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

            for (int y = minY; y <= maxY; y++) {
                uint8_t* rowBase[kTexCount];
                for (int t = 0; t < kTexCount; t++)
                    rowBase[t] = needed[t] ? layers[t].rgba.data() + size_t(y) * texW * 4 : nullptr;
                for (int x = minX; x <= maxX; x++) {
                    float p[2] = {x + 0.5f, y + 0.5f};
                    float w[3];
                    if (!barycentric(p, tv0, tv1, tv2, w)) continue;
                    if (w[0] < -1e-6f || w[1] < -1e-6f || w[2] < -1e-6f) continue;
                    for (int t = 0; t < kTexCount; t++) {
                        if (!pt[t].image || pt[t].uvSet < 0) continue;
                        V2 suv{w[0] * pt[t].su[0].u + w[1] * pt[t].su[1].u + w[2] * pt[t].su[2].u,
                               w[0] * pt[t].su[0].v + w[1] * pt[t].su[1].v + w[2] * pt[t].su[2].v};
                        float px[4];
                        sampleBytes(pt[t].image->w, pt[t].image->h, pt[t].image->rgba.data(),
                                    suv.u, suv.v, opt.filter, true, px);
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

    for (int t = 0; t < kTexCount; t++) {
        if (!needed[t] || layers[t].rgba.empty()) continue;
        Image out;
        out.name = group.name + "_" + texTypeName(TexType(t));
        imageFromRaster(layers[t], out);
        outBaked.emplace_back(TexType(t), std::move(out));
    }
    return true;
}

}  // namespace uvremap
