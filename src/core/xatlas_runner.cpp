#include "core/xatlas_runner.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "xatlas.h"

namespace uvremap {

bool AtlasGen::findSource(int primId, const GenMesh& m, uint32_t i0, uint32_t i1,
                          uint32_t i2, SourceTri& out) const {
    uint32_t a = canonical(primId, m.vertices[i0].xref);
    uint32_t b = canonical(primId, m.vertices[i1].xref);
    uint32_t c = canonical(primId, m.vertices[i2].xref);
    if (a == b || b == c || a == c)
        return false;  // degenerate after packing
    uint32_t s[3] = {a, b, c};
    std::sort(s, s + 3);
    auto it = sourceTris.find(TriKey{uint32_t(primId), s[0], s[1], s[2]});
    if (it != sourceTris.end()) {
        out = it->second;
        return true;
    }
    return false;
}

namespace {
TriKey makeKey(int primId, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t s[3] = {a, b, c};
    std::sort(s, s + 3);
    return TriKey{uint32_t(primId), s[0], s[1], s[2]};
}

// De-interleaved input arrays kept alive until AddMesh has copied them.
struct Holder {
    int primId;
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> faceMaterials;
    bool hasNormals = false;
    int triCount = 0;
};

xatlas::MeshDecl makeDecl(const Holder& h) {
    xatlas::MeshDecl decl;
    decl.vertexPositionData = h.positions.data();
    decl.vertexPositionStride = sizeof(float) * 3;
    decl.vertexCount = uint32_t(h.positions.size() / 3);
    if (h.hasNormals) {
        decl.vertexNormalData = h.normals.data();
        decl.vertexNormalStride = sizeof(float) * 3;
    }
    if (!h.uvs.empty()) {
        decl.vertexUvData = h.uvs.data();
        decl.vertexUvStride = sizeof(float) * 2;
    }
    decl.indexData = h.indices.data();
    decl.indexCount = uint32_t(h.indices.size());
    decl.indexFormat = xatlas::IndexFormat::UInt32;
    decl.faceMaterialData = h.faceMaterials.data();
    decl.faceCount = uint32_t(h.triCount);
    return decl;
}
}  // namespace

bool generateAtlas(const Model& model, const AtlasGroup& group, int sourceUv,
                   const GenOptions& opt, AtlasGen& out, Report& report,
                   std::string& err) {
    if (group.primitives.empty()) {
        err = "empty atlas group";
        return false;
    }

    std::vector<Holder> holders;
    holders.reserve(group.primitives.size());
    float groupArea = 0.f;  // total world-space surface area of the group

    for (int pid : group.primitives) {
        const Primitive& p = model.primitives[pid];
        Holder h;
        h.primId = pid;
        size_t n = p.vertices.size();
        h.positions.resize(n * 3);
        h.hasNormals = n > 0 && p.vertices[0].hasNormal;
        if (h.hasNormals) h.normals.resize(n * 3);
        h.uvs.resize(n * 2);
        bool hasUv = n > 0 && size_t(sourceUv) < p.vertices[0].uvsets.size();
        for (size_t v = 0; v < n; v++) {
            h.positions[v * 3] = p.vertices[v].position.x;
            h.positions[v * 3 + 1] = p.vertices[v].position.y;
            h.positions[v * 3 + 2] = p.vertices[v].position.z;
            if (h.hasNormals) {
                h.normals[v * 3] = p.vertices[v].normal.x;
                h.normals[v * 3 + 1] = p.vertices[v].normal.y;
                h.normals[v * 3 + 2] = p.vertices[v].normal.z;
            }
            if (hasUv) {
                h.uvs[v * 2] = p.vertices[v].uvsets[sourceUv].u;
                h.uvs[v * 2 + 1] = p.vertices[v].uvsets[sourceUv].v;
            }
        }
        h.indices = p.indices;
        h.triCount = int(h.indices.size() / 3);
        h.faceMaterials.assign(h.triCount, uint32_t(p.material + 1));
        if (!hasUv) {
            // No usable source UV hint; still atlasable, xatlas computes fresh charts.
            h.uvs.clear();
        }

        // Colocal canonicalization: xatlas welds coincident vertices and may
        // report a twin's index as xref on an output triangle. TriKey lookups
        // and corner matching use the group's first vertex instead.
        {
            struct PosKey {
                uint32_t b[3];
                bool operator==(const PosKey& o) const { return b[0]==o.b[0] && b[1]==o.b[1] && b[2]==o.b[2]; }
            };
            struct PosHash {
                size_t operator()(const PosKey& k) const {
                    uint64_t hsh = 1469598103934665603ull;
                    for (uint32_t v : k.b) { hsh ^= v; hsh *= 1099511628211ull; }
                    return size_t(hsh);
                }
            };
            auto bits = [](float f) {
                uint32_t u;
                std::memcpy(&u, &f, 4);
                if (u == 0x80000000u) u = 0;  // -0.0 == +0.0
                return u;
            };
            std::unordered_map<PosKey, uint32_t, PosHash> first;
            std::vector<uint32_t>& canon = out.canonByPrim[pid];
            canon.resize(n);
            for (size_t v = 0; v < n; v++) {
                PosKey k{{bits(h.positions[v * 3]), bits(h.positions[v * 3 + 1]),
                          bits(h.positions[v * 3 + 2])}};
                auto it = first.find(k);
                if (it == first.end()) {
                    first.emplace(k, uint32_t(v));
                    canon[v] = uint32_t(v);
                } else {
                    canon[v] = it->second;
                }
            }
        }
        const std::vector<uint32_t>& canon = out.canonByPrim[pid];

        // Build the source triangle table (keys use canonical colocal indices).
        for (int t = 0; t < h.triCount; t++) {
            uint32_t a = h.indices[t * 3], b = h.indices[t * 3 + 1], c = h.indices[t * 3 + 2];
            auto key = makeKey(pid, canon[a], canon[b], canon[c]);
            SourceTri st{a, b, c, pid};
            auto it = out.sourceTris.find(key);
            if (it == out.sourceTris.end()) {
                out.sourceTris[key] = st;
            } else if (it->second.v0 != a || it->second.v1 != b || it->second.v2 != c) {
                out.duplicateTriKeys++;
            }
        }
        for (int t = 0; t < h.triCount; t++) {
            const float* p0 = &h.positions[h.indices[t * 3] * 3];
            const float* p1 = &h.positions[h.indices[t * 3 + 1] * 3];
            const float* p2 = &h.positions[h.indices[t * 3 + 2] * 3];
            float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
            float e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
            float cx = e1[1] * e2[2] - e1[2] * e2[1];
            float cy = e1[2] * e2[0] - e1[0] * e2[2];
            float cz = e1[0] * e2[1] - e1[1] * e2[0];
            groupArea += 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);
        }
        holders.push_back(std::move(h));
    }

    xatlas::ChartOptions chartOpts;
    chartOpts.fixWinding = true;

    xatlas::PackOptions packOpts;
    packOpts.padding = uint32_t(std::max(1, opt.padding));
    int res = 0;
    if (opt.resolutionW > 0 || opt.resolutionH > 0)
        res = std::max(opt.resolutionW, opt.resolutionH);
    if (res > 0) packOpts.resolution = uint32_t(res);
    // The non-square renormalization below compresses the longer axis by
    // sv; widen pack padding so the smaller axis keeps the requested texel
    // gutter after the scale.
    if (res > 0 && opt.resolutionW > 0 && opt.resolutionH > 0 && opt.resolutionW != opt.resolutionH) {
        float sv = std::min(float(opt.resolutionW) / float(res), float(opt.resolutionH) / float(res));
        packOpts.padding = uint32_t(std::max(1, int(std::ceil(opt.padding / sv))));
    }
    float tpu = 0.f;
    if (opt.texelsPerUnit > 0.f) {
        tpu = opt.texelsPerUnit;
    } else if (res > 0 && groupArea > 1e-12f) {
        // Default density: aim for a well-filled atlas given the group's surface
        // area; xatlas' own estimator leaves small models far too sparse and a
        // fixed texel count ignores model scale. Sized for the requested area
        // (sqrt(W*H)), not the square xatlas packs into, so the non-square
        // renormalization does not overflow into extra pages.
        float side = float(res);
        if (opt.resolutionW > 0 && opt.resolutionH > 0)
            side = std::sqrt(float(opt.resolutionW) * float(opt.resolutionH));
        tpu = std::sqrt(0.6f) * side / std::sqrt(groupArea);
    }
    packOpts.bilinear = true;

    // Charts that do not fit one page spill into extra sub-atlases; the baker
    // stacks every page into one tall texture, so nothing is lost.
    xatlas::Atlas* atlas = xatlas::Create();
    if (!atlas) {
        err = "xatlas::Create failed";
        return false;
    }
    for (const Holder& h : holders) {
        xatlas::AddMeshError e = xatlas::AddMesh(atlas, makeDecl(h));
        if (e != xatlas::AddMeshError::Success) {
            xatlas::Destroy(atlas);
            err = "xatlas::AddMesh failed for primitive " + std::to_string(h.primId) +
                  " (code " + std::to_string(int(e)) + ")";
            return false;
        }
    }
    packOpts.texelsPerUnit = tpu;
    xatlas::Generate(atlas, chartOpts, packOpts);

    if (atlas->meshCount != group.primitives.size()) {
        xatlas::Destroy(atlas);
        err = "xatlas output mesh count mismatch";
        return false;
    }
    out.width = int(atlas->width);
    out.height = int(atlas->height);
    // xatlas packs square atlases; for a non-square request, rescale the
    // packed square (per-page pixel coords) to the requested W x H. All
    // downstream consumers derive sizes from width/height.
    float su = 1.f, sv = 1.f;
    if (opt.resolutionW > 0 && opt.resolutionH > 0 && opt.resolutionW != opt.resolutionH &&
        atlas->width && atlas->height) {
        su = float(opt.resolutionW) / float(atlas->width);
        sv = float(opt.resolutionH) / float(atlas->height);
        out.width = opt.resolutionW;
        out.height = opt.resolutionH;
        report.info("atlas '" + group.name + "': packed square " + std::to_string(atlas->width) +
                    " and renormalized to " + std::to_string(out.width) + "x" +
                    std::to_string(out.height) + " (pack padding widened before the scale)");
    }
    out.pageCount = std::max(1, int(atlas->atlasCount));
    out.meshes.resize(atlas->meshCount);
    for (uint32_t m = 0; m < atlas->meshCount; m++) {
        const xatlas::Mesh& om = atlas->meshes[m];
        GenMesh& gm = out.meshes[m];
        gm.primId = group.primitives[m];
        out.primOrder.push_back(gm.primId);
        gm.vertices.resize(om.vertexCount);
        for (uint32_t v = 0; v < om.vertexCount; v++) {
            gm.vertices[v].u = om.vertexArray[v].uv[0] * su;
            gm.vertices[v].v = om.vertexArray[v].uv[1] * sv;
            gm.vertices[v].atlasIndex = om.vertexArray[v].atlasIndex;
            gm.vertices[v].xref = om.vertexArray[v].xref;
        }
        gm.indices.assign(om.indexArray, om.indexArray + om.indexCount);
    }
    if (out.pageCount > 1)
        report.info("atlas '" + group.name + "': charts needed " +
                    std::to_string(out.pageCount) +
                    " pages; baked textures stack them vertically (" + std::to_string(out.width) +
                    "x" + std::to_string(out.height * out.pageCount) + ")");
    xatlas::Destroy(atlas);
    return true;
}

}  // namespace uvremap
