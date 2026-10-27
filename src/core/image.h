#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/types.h"

namespace uvremap {

struct Raster {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // 4 bytes/pixel, row 0 = top

    void resize(int width, int height);
    void fill(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    uint8_t* pixel(int x, int y) { return &rgba[size_t(y) * w * 4 + size_t(x) * 4]; }
    const uint8_t* pixel(int x, int y) const { return &rgba[size_t(y) * w * 4 + size_t(x) * 4]; }
};

// Decodes image bytes (PNG/JPG/...) into RGBA8. Row 0 = top (glTF convention).
bool decodeImage(const uint8_t* data, size_t len, Raster& out, std::string& err);
bool decodeImage(const std::vector<uint8_t>& data, Raster& out, std::string& err);

// Encodes RGBA8 to PNG bytes in memory.
bool encodePng(const Raster& img, std::vector<uint8_t>& outBytes);

// Decodes a raster already held in an Image struct (Model::images helper).
bool imageFromRaster(const Raster& r, Image& img);

enum class Filter { Nearest, Linear };

// Samples normalized uv. Wrap: repeat (glTF default) or clamp.
// out has 4 floats in [0,1] (raw stored bytes / 255).
void samplePoint(const Raster& img, float u, float v, Filter f, bool repeat, float out[4]);
void sampleBytes(int w, int h, const uint8_t* rgba, float u, float v, Filter f, bool repeat,
                 float out[4]);

// Mean color of all pixels (diagnostics).
void meanColor(const Raster& img, float out[4]);

}  // namespace uvremap
