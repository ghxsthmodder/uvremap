#include "core/image.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cmath>

namespace uvremap {

void Raster::resize(int width, int height) {
    w = width;
    h = height;
    rgba.assign(size_t(width) * height * 4, 0);
}

void Raster::fill(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = r;
        rgba[i + 1] = g;
        rgba[i + 2] = b;
        rgba[i + 3] = a;
    }
}

bool decodeImage(const uint8_t* data, size_t len, Raster& out, std::string& err) {
    int w = 0, h = 0, ch = 0;
    stbi_set_flip_vertically_on_load(0);  // keep row 0 = top
    unsigned char* px = stbi_load_from_memory(data, int(len), &w, &h, &ch, 4);
    if (!px) {
        err = "stb decode failure";
        return false;
    }
    out.w = w;
    out.h = h;
    out.rgba.assign(px, px + size_t(w) * h * 4);
    stbi_image_free(px);
    return true;
}

bool decodeImage(const std::vector<uint8_t>& data, Raster& out, std::string& err) {
    return decodeImage(data.data(), data.size(), out, err);
}

namespace {
void pngWriter(void* context, void* data, int size) {
    auto* vec = reinterpret_cast<std::vector<uint8_t>*>(context);
    auto* p = reinterpret_cast<uint8_t*>(data);
    vec->insert(vec->end(), p, p + size);
}
}  // namespace

bool encodePng(const Raster& img, std::vector<uint8_t>& outBytes) {
    outBytes.clear();
    int ok = stbi_write_png_to_func(pngWriter, &outBytes, img.w, img.h, 4,
                                    img.rgba.data(), img.w * 4);
    return ok != 0;
}

bool imageFromRaster(const Raster& r, Image& img) {
    img.w = r.w;
    img.h = r.h;
    img.rgba = r.rgba;
    img.decoded = true;
    return true;
}

namespace {
inline float wrap01(float v, bool repeat) {
    if (repeat) {
        v -= std::floor(v);
        return v;
    }
    if (v < 0.f) return 0.f;
    if (v > 1.f) return 1.f;
    return v;
}
}  // namespace

void samplePoint(const Raster& img, float u, float v, Filter f, bool repeat, float out[4]) {
    sampleBytes(img.w, img.h, img.rgba.data(), u, v, f, repeat, out);
}

void sampleBytes(int w, int h, const uint8_t* rgba, float u, float v, Filter f, bool repeat,
                 float out[4]) {
    if (w == 0 || h == 0) {
        out[0] = out[1] = out[2] = out[3] = 0.f;
        return;
    }
    u = wrap01(u, repeat);
    v = wrap01(v, repeat);
    auto pxAt = [&](int x, int y, float o[4]) {
        const uint8_t* p = rgba + (size_t(y) * w + x) * 4;
        for (int i = 0; i < 4; i++) o[i] = p[i] / 255.f;
    };
    if (f == Filter::Nearest) {
        int x = int(u * w);
        int y = int(v * h);
        if (x >= w) x = w - 1;
        if (y >= h) y = h - 1;
        pxAt(x, y, out);
        return;
    }
    // Bilinear with half-texel centers.
    float fx = u * w - 0.5f;
    float fy = v * h - 0.5f;
    float tx = fx - std::floor(fx), ty = fy - std::floor(fy);
    auto fetch = [&](int x, int y, float o[4]) {
        if (repeat) {
            x = ((x % w) + w) % w;
            y = ((y % h) + h) % h;
        } else {
            if (x < 0) x = 0;
            if (x >= w) x = w - 1;
            if (y < 0) y = 0;
            if (y >= h) y = h - 1;
        }
        pxAt(x, y, o);
    };
    int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    float c00[4], c10[4], c01[4], c11[4];
    fetch(x0, y0, c00);
    fetch(x0 + 1, y0, c10);
    fetch(x0, y0 + 1, c01);
    fetch(x0 + 1, y0 + 1, c11);
    for (int i = 0; i < 4; i++)
        out[i] = (c00[i] * (1 - tx) + c10[i] * tx) * (1 - ty) +
                 (c01[i] * (1 - tx) + c11[i] * tx) * ty;
}

void meanColor(const Raster& img, float out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0.f;
    if (img.rgba.empty()) return;
    size_t n = img.rgba.size() / 4;
    double acc[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < 4; c++) acc[c] += img.rgba[i * 4 + c];
    for (int c = 0; c < 4; c++) out[c] = float(acc[c] / n / 255.0);
}

}  // namespace uvremap
