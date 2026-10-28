#pragma once

#include <functional>
#include <string>

#include "core/grouping.h"
#include "core/image.h"
#include "core/report.h"

namespace uvremap {

struct RegenOptions {
    std::string output;
    AtlasMode mode = AtlasMode::Material;
    int resolutionW = 0, resolutionH = 0;
    int padding = 4;
    float texelsPerUnit = 0.f;
    int sourceUv = 0;
    int targetUv = 0;
    int selfCheck = 0;  // >0: sample N points per baked image and report agreement
    Filter filter = Filter::Linear;
    bool bakeFactors = false;      // fold material factors into texture-less slots
    bool splitMaterials = false;   // emit one baked image per material (default: shared)
    int textureW = 0, textureH = 0;
    bool binary = true;            // glb default unless --format gltf
    bool embedTextures = false;    // gltf: base64 data URIs instead of files
    bool formatSet = false;
    bool dryRun = false;
    bool force = false;
    bool verbose = false;
    bool quiet = false;
    bool writePreviews = false;
    bool json = false;  // write report.json alongside report.txt
    // (step 1..5, label, ok) — called after each pipeline phase.
    std::function<void(int, const std::string&, bool)> progress;
};

struct RegenResult {
    ExitCode code = ExitCode::Success;
    Report report;
    std::string message;  // main error text when code != Success
};

RegenResult runRegenerate(const std::string& input, const RegenOptions& opt);

}  // namespace uvremap
