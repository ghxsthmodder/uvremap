#include "core/preview.h"

#include <algorithm>
#include <cmath>
#include <fstream>

#include "core/image.h"

namespace uvremap {

namespace {

struct Canvas {
    Raster img;
    Canvas(int w, int h) {
        img.resize(w, h);
        img.fill(255, 255, 255, 255);
    }
    void line(float x0, float y0, float x1, float y1, uint8_t c) {
        float len = std::hypot(x1 - x0, y1 - y0);
        int n = std::max(1, int(len));
        for (int i = 0; i <= n; i++) {
            float t = float(i) / n;
            int x = int(x0 + (x1 - x0) * t), y = int(y0 + (y1 - y0) * t);
            if (x < 0 || y < 0 || x >= img.w || y >= img.h) continue;
            uint8_t* p = img.pixel(x, y);
            p[0] = p[1] = p[2] = c;
            p[3] = 255;
        }
    }
    void tri(float x0, float y0, float x1, float y1, float x2, float y2, uint8_t c) {
        int minX = std::max(0, int(std::floor(std::min({x0, x1, x2}))));
        int maxX = std::min(img.w - 1, int(std::ceil(std::max({x0, x1, x2}))));
        int minY = std::max(0, int(std::floor(std::min({y0, y1, y2}))));
        int maxY = std::min(img.h - 1, int(std::ceil(std::max({y0, y1, y2}))));
        float d = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (std::fabs(d) < 1e-9f) return;
        for (int y = minY; y <= maxY; y++) {
            for (int x = minX; x <= maxX; x++) {
                float px = x + .5f, py = y + .5f;
                // barycentric via cross signs
                float c1 = ((x1 - x0) * (py - y0) - (px - x0) * (y1 - y0));
                float c2 = ((x2 - x1) * (py - y1) - (px - x1) * (y2 - y1));
                float c3 = ((x0 - x2) * (py - y2) - (px - x2) * (y0 - y2));
                bool in = (c1 >= 0 && c2 >= 0 && c3 >= 0) || (c1 <= 0 && c2 <= 0 && c3 <= 0);
                if (in) {
                    uint8_t* p = img.pixel(x, y);
                    p[0] = p[1] = p[2] = c;
                    p[3] = 255;
                }
            }
        }
    }
};

bool writeCanvas(const Canvas& c, const std::string& path, std::string& err) {
    std::vector<uint8_t> png;
    if (!encodePng(c.img, png)) {
        err = "failed to encode preview png";
        return false;
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        err = "failed to open " + path;
        return false;
    }
    f.write(reinterpret_cast<char*>(png.data()), png.size());
    return true;
}

}  // namespace

bool renderUvLayout(const Model& model, int uvIndex, const std::string& outPath,
                    Report& report, std::string& err) {
    const int S = 1024;
    Canvas c(S, S);
    // Fit all UVs into [0,1] box (uv values outside [0,1] are clipped).
    for (size_t pi = 0; pi < model.primitives.size(); pi++) {
        const Primitive& p = model.primitives[pi];
        if (p.vertices.empty() || size_t(uvIndex) >= p.vertices[0].uvsets.size()) continue;
        uint8_t shade = uint8_t(30 + (pi * 47) % 200);
        for (size_t f = 0; f + 2 < p.indices.size(); f += 3) {
            const V2& a = p.vertices[p.indices[f]].uvsets[uvIndex];
            const V2& b = p.vertices[p.indices[f + 1]].uvsets[uvIndex];
            const V2& cc = p.vertices[p.indices[f + 2]].uvsets[uvIndex];
            float ax = a.u * S, ay = a.v * S, bx = b.u * S, by = b.v * S, cx = cc.u * S,
                  cy = cc.v * S;
            c.line(ax, ay, bx, by, shade);
            c.line(bx, by, cx, cy, shade);
            c.line(cx, cy, ax, ay, shade);
        }
    }
    (void)report;
    return writeCanvas(c, outPath, err);
}

bool renderAtlasLayout(const Model& model, const AtlasGen& gen, const std::string& outPath,
                       Report& report, std::string& err) {
    const int S = 1024;
    const int pages = std::max(1, gen.pageCount);
    Canvas c(S, S);
    float sx = gen.width ? float(S) / gen.width : 1.f;
    float sy = gen.height ? float(S) / float(gen.height * pages) : 1.f;
    size_t meshIdx = 0;
    for (const GenMesh& gm : gen.meshes) {
        uint8_t shade = uint8_t(40 + (meshIdx * 61) % 180);
        for (size_t f = 0; f + 2 < gm.indices.size(); f += 3) {
            const GenVertex& v0 = gm.vertices[gm.indices[f]];
            const GenVertex& v1 = gm.vertices[gm.indices[f + 1]];
            const GenVertex& v2 = gm.vertices[gm.indices[f + 2]];
            if (v0.atlasIndex < 0 || v1.atlasIndex < 0 || v2.atlasIndex < 0) continue;
            const float poff = float(v0.atlasIndex) * gen.height;
            c.tri(v0.u * sx, (poff + v0.v) * sy, v1.u * sx, (poff + v1.v) * sy, v2.u * sx,
                  (poff + v2.v) * sy, shade);
        }
        meshIdx++;
    }
    (void)model;
    (void)report;
    return writeCanvas(c, outPath, err);
}

}  // namespace uvremap
