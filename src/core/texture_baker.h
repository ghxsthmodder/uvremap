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
};

// Bakes every present texture type of the group's materials into atlas-sized
// images. Returns one (TexType, Image) per baked map.
bool bakeGroup(const Model& model, const AtlasGroup& group, const AtlasGen& gen,
               const BakeOptions& opt, std::vector<std::pair<TexType, Image>>& outBaked,
               Report& report, std::string& err);

// Neutral fill values for uncovered texels, per texture type.
void neutralTexel(TexType t, uint8_t out[4]);

}  // namespace uvremap
