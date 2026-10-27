#pragma once

#include <string>

#include "core/report.h"
#include "core/types.h"
#include "core/xatlas_runner.h"

namespace uvremap {

// Draws the wireframe of one UV set of all primitives into a diagnostic PNG.
bool renderUvLayout(const Model& model, int uvIndex, const std::string& outPath,
                    Report& report, std::string& err);

// Draws atlas coverage (filled triangles per chart) into a diagnostic PNG.
bool renderAtlasLayout(const Model& model, const AtlasGen& gen, const std::string& outPath,
                       Report& report, std::string& err);

// Side-by-side diagnostic: source-UV wireframe (left) vs new atlas fill
// (right) in one PNG, separated by a gray divider.
bool renderUvCompare(const Model& model, int srcUv, const AtlasGen& gen,
                     const std::string& outPath, Report& report, std::string& err);

// Side-by-side diagnostics over the atlas: white-on-black chart occupancy
// (left) and a log-scaled texel density heatmap (right, 3D area per atlas
// area; blue = sparse, red = dense). Pages stack vertically.
bool renderAtlasHeatmaps(const Model& model, const AtlasGen& gen, const std::string& outPath,
                         Report& report, std::string& err);

}  // namespace uvremap
