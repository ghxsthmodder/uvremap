#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/grouping.h"
#include "core/report.h"
#include "core/types.h"

namespace uvremap {

struct GenVertex {
    float u = 0, v = 0;  // atlas pixel coordinates, within its page
    int32_t atlasIndex = -1;  // page (0..pageCount-1), -1 = not in any atlas
    uint32_t xref = 0;   // index into the input primitive's vertex array
};

struct GenMesh {
    int primId = -1;
    std::vector<GenVertex> vertices;
    std::vector<uint32_t> indices;  // triangle list
};

// A source triangle identified by its three corner vertex indices.
struct SourceTri {
    uint32_t v0, v1, v2;
    int primId;
};

struct TriKey {
    uint32_t prim;
    uint32_t a, b, c;  // sorted
    bool operator==(const TriKey& o) const {
        return prim == o.prim && a == o.a && b == o.b && c == o.c;
    }
};
struct TriKeyHash {
    size_t operator()(const TriKey& k) const {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t v : {k.prim, k.a, k.b, k.c}) {
            h ^= v;
            h *= 1099511628211ull;
        }
        return size_t(h);
    }
};

struct AtlasGen {
    int width = 0, height = 0;  // per-page size
    int pageCount = 1;          // xatlas sub-atlases; baked as stacked pages
    std::vector<GenMesh> meshes;  // 1:1 with group primitives, in order
    std::vector<int> primOrder;   // meshes[i].primId
    // (primId, sorted corner triple) -> source triangle corners.
    // Indices are canonicalized through canonByPrim first: xatlas welds
    // colocal vertices and may report a corner's twin as the xref.
    std::unordered_map<TriKey, SourceTri, TriKeyHash> sourceTris;
    std::unordered_map<int, std::vector<uint32_t>> canonByPrim;  // vertex -> first vertex with the same position
    int duplicateTriKeys = 0;
    int unmappedTriangles = 0;

    uint32_t canonical(int primId, uint32_t v) const {
        auto it = canonByPrim.find(primId);
        if (it != canonByPrim.end() && v < it->second.size()) return it->second[v];
        return v;
    }

    bool findSource(int primId, const GenMesh& m, uint32_t i0, uint32_t i1, uint32_t i2,
                    SourceTri& out) const;
};

struct GenOptions {
    int resolutionW = 0, resolutionH = 0;  // 0 = auto
    int padding = 4;                       // atlas texels
    float texelsPerUnit = 0.f;
};

// Runs xatlas for one group: generates new charts/parametrization/packing.
bool generateAtlas(const Model& model, const AtlasGroup& group, int sourceUv,
                   const GenOptions& opt, AtlasGen& out, Report& report,
                   std::string& err);

}  // namespace uvremap
