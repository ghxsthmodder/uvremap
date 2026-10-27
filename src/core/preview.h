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

}  // namespace uvremap
