#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/grouping.h"
#include "core/image.h"
#include "core/report.h"
#include "core/types.h"
#include "core/xatlas_runner.h"

namespace uvremap {

struct BakeOptions {
    Filter filter = Filter::Linear;
    int textureW = 0, textureH = 0;  // 0 = use atlas size
    int sourceUv = 0;                // fallback UV set when a texture's texCoord is missing
    bool bakeFactors = false;        // fold material factors into texture-less slots
};

// Bakes every present texture type of the group's materials into atlas-sized
// images. One BakedImage per (material, type): material -1 is the shared
// layer used when no primitive needs per-material copies (e.g. KHR_materials_variants).
bool bakeGroup(const Model& model, const AtlasGroup& group, const AtlasGen& gen,
               const BakeOptions& opt, std::vector<BakedImage>& outBaked,
               Report& report, std::string& err);

// Neutral fill values for uncovered texels, per texture type.
void neutralTexel(TexType t, uint8_t out[4]);

// Statistically verify baked atlases against the source textures: samples
// nSamples random points per baked image (excluding normal maps, whose
// tangent-frame rebake cannot be compared texel-wise) and reports agreement
// percentages through the report. Non-fatal; meant for --self-check.
bool selfCheckGroup(const Model& model, const AtlasGroup& group, const AtlasGen& gen,
                    const BakeOptions& opt, const std::vector<BakedImage>& baked, int nSamples,
                    Report& report);

}  // namespace uvremap
