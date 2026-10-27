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

bool renderUvCompare(const Model& model, int srcUv, const AtlasGen& gen,
                     const std::string& outPath, Report& report, std::string& err) {
    const int S = 1024;
    const int GUT = 16;
    Canvas c(S * 2 + GUT, S);
    // Left: source UV wireframe, darker line per primitive.
    for (size_t pi = 0; pi < model.primitives.size(); pi++) {
        const Primitive& p = model.primitives[pi];
        if (p.vertices.empty() || size_t(srcUv) >= p.vertices[0].uvsets.size()) continue;
        uint8_t shade = uint8_t(30 + (pi * 47) % 200);
        for (size_t f = 0; f + 2 < p.indices.size(); f += 3) {
            const V2& a = p.vertices[p.indices[f]].uvsets[srcUv];
            const V2& b = p.vertices[p.indices[f + 1]].uvsets[srcUv];
            const V2& cc = p.vertices[p.indices[f + 2]].uvsets[srcUv];
            c.line(a.u * S, a.v * S, b.u * S, b.v * S, shade);
            c.line(b.u * S, b.v * S, cc.u * S, cc.v * S, shade);
            c.line(cc.u * S, cc.v * S, a.u * S, a.v * S, shade);
        }
    }
    // Right: new atlas fill, pages stacked vertically like renderAtlasLayout.
    const int pages = std::max(1, gen.pageCount);
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
            c.tri(GUT + S + v0.u * sx, (poff + v0.v) * sy, GUT + S + v1.u * sx,
                  (poff + v1.v) * sy, GUT + S + v2.u * sx, (poff + v2.v) * sy, shade);
        }
        meshIdx++;
    }
    for (int y = 0; y < S; y++)
        for (int x = S; x < S + GUT; x++) {
            uint8_t* p = c.img.pixel(x, y);
            p[0] = p[1] = p[2] = 128;
            p[3] = 255;
        }
    (void)report;
    return writeCanvas(c, outPath, err);
}

namespace {
// Blue (sparse) -> green -> red (dense) on t in [0,1].
void heatColor(float t, uint8_t out[3]) {
    t = std::clamp(t, 0.f, 1.f);
    if (t < 0.5f) {
        float k = t / 0.5f;
        out[0] = uint8_t(30 + k * (60 - 30));
        out[1] = uint8_t(60 + k * (200 - 60));
        out[2] = uint8_t(220 + k * (120 - 220));
    } else {
        float k = (t - 0.5f) / 0.5f;
        out[0] = uint8_t(30 + k * (230 - 30));
        out[1] = uint8_t(200 - k * (200 - 50));
        out[2] = uint8_t(120 - k * (120 - 40));
    }
}
}  // namespace

bool renderAtlasHeatmaps(const Model& model, const AtlasGen& gen, const std::string& outPath,
                         Report& report, std::string& err) {
    const int S = 1024;
    const int GUT = 16;
    const int pages = std::max(1, gen.pageCount);
    Canvas c(S * 2 + GUT, S);
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S * 2 + GUT; x++) {
            uint8_t* p = c.img.pixel(x, y);
            p[0] = p[1] = p[2] = 0;
            p[3] = 255;
        }
    for (int y = 0; y < S; y++)
        for (int x = S; x < S + GUT; x++) {
            uint8_t* p = c.img.pixel(x, y);
            p[0] = p[1] = p[2] = 90;
            p[3] = 255;
        }
    float sx = gen.width ? float(S) / gen.width : 1.f;
    float sy = gen.height ? float(S) / float(gen.height * pages) : 1.f;

    // First pass: per-triangle texel density = atlas-pixel area / 3D area.
    // Pixels span a square page of size max(W,H) scaled into S, so pixel-area
    // ratios are resolution-correct up to a constant per group.
    std::vector<std::vector<float>> dens(gen.meshes.size());
    float lo = 0.f, hi = 0.f;
    for (size_t mi = 0; mi < gen.meshes.size(); mi++) {
        const GenMesh& gm = gen.meshes[mi];
        dens[mi].assign(gm.indices.size() / 3, -1.f);
        if (gm.primId < 0 || size_t(gm.primId) >= model.primitives.size()) continue;
        const Primitive& p = model.primitives[gm.primId];
        for (size_t f = 0; f + 2 < gm.indices.size(); f += 3) {
            const GenVertex& v0 = gm.vertices[gm.indices[f]];
            const GenVertex& v1 = gm.vertices[gm.indices[f + 1]];
            const GenVertex& v2 = gm.vertices[gm.indices[f + 2]];
            if (v0.atlasIndex < 0 || v1.atlasIndex < 0 || v2.atlasIndex < 0) continue;
            if (v0.xref >= p.vertices.size() || v1.xref >= p.vertices.size() ||
                v2.xref >= p.vertices.size())
                continue;
            const SourceVertex& a = p.vertices[v0.xref];
            const SourceVertex& b = p.vertices[v1.xref];
            const SourceVertex& e = p.vertices[v2.xref];
            V3 ab{b.position.x - a.position.x, b.position.y - a.position.y,
                  b.position.z - a.position.z};
            V3 ae{e.position.x - a.position.x, e.position.y - a.position.y,
                  e.position.z - a.position.z};
            V3 cr{ab.y * ae.z - ab.z * ae.y, ab.z * ae.x - ab.x * ae.z,
                  ab.x * ae.y - ab.y * ae.x};
            float area3d = 0.5f * std::sqrt(cr.x * cr.x + cr.y * cr.y + cr.z * cr.z);
            if (!(area3d > 0.f)) continue;  // degenerate or NaN
            float pixA = std::fabs((v1.u - v0.u) * (v2.v - v0.v) - (v2.u - v0.u) * (v1.v - v0.v)) *
                         0.5f;
            if (!(pixA > 0.f)) continue;
            float d = pixA / area3d;
            dens[mi][f / 3] = d;
            if (lo == 0.f || d < lo) lo = d;
            if (d > hi) hi = d;
        }
    }
    float llo = lo > 0.f ? std::log2(lo) : 0.f, lhi = hi > lo ? std::log2(hi) : llo + 1.f;

    for (size_t mi = 0; mi < gen.meshes.size(); mi++) {
        const GenMesh& gm = gen.meshes[mi];
        for (size_t f = 0; f + 2 < gm.indices.size(); f += 3) {
            const GenVertex& v0 = gm.vertices[gm.indices[f]];
            const GenVertex& v1 = gm.vertices[gm.indices[f + 1]];
            const GenVertex& v2 = gm.vertices[gm.indices[f + 2]];
            if (v0.atlasIndex < 0 || v1.atlasIndex < 0 || v2.atlasIndex < 0) continue;
            const float poff = float(v0.atlasIndex) * gen.height;
            float x0 = v0.u * sx, y0 = (poff + v0.v) * sy;
            float x1 = v1.u * sx, y1 = (poff + v1.v) * sy;
            float x2 = v2.u * sx, y2 = (poff + v2.v) * sy;
            c.tri(x0, y0, x1, y1, x2, y2, 255);  // occupancy: white on black
            float d = dens[mi][f / 3];
            if (d < 0.f) continue;  // no density data (degenerate triangle)
            uint8_t gcol[3];
            heatColor((std::log2(d) - llo) / (lhi - llo), gcol);
            int minX = std::max(0, int(std::floor(std::min({x0, x1, x2}))));
            int maxX = std::min(S - 1, int(std::ceil(std::max({x0, x1, x2}))));
            int minY = std::max(0, int(std::floor(std::min({y0, y1, y2}))));
            int maxY = std::min(S - 1, int(std::ceil(std::max({y0, y1, y2}))));
            float dd = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
            if (std::fabs(dd) < 1e-9f) continue;
            for (int y = minY; y <= maxY; y++)
                for (int x = minX; x <= maxX; x++) {
                    float px = x + .5f, py = y + .5f;
                    float c1 = ((x1 - x0) * (py - y0) - (px - x0) * (y1 - y0));
                    float c2 = ((x2 - x1) * (py - y1) - (px - x1) * (y2 - y1));
                    float c3 = ((x0 - x2) * (py - y2) - (px - x2) * (y0 - y2));
                    bool in = (c1 >= 0 && c2 >= 0 && c3 >= 0) ||
                              (c1 <= 0 && c2 <= 0 && c3 <= 0);
                    if (!in) continue;
                    uint8_t* q = c.img.pixel(S + GUT + x, y);
                    q[0] = gcol[0];
                    q[1] = gcol[1];
                    q[2] = gcol[2];
                    q[3] = 255;
                }
        }
    }
    report.info("preview heatmaps: density range " + std::to_string(lo) + ".." +
                std::to_string(hi) + " (log2 scale, pages stacked vertically)");
    (void)err;
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
