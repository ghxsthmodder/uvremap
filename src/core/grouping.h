#pragma once

#include <vector>

#include "core/report.h"
#include "core/types.h"

namespace uvremap {

enum class AtlasMode { Material, Mesh, Scene };

bool parseAtlasMode(const std::string& s, AtlasMode& out);
const char* atlasModeName(AtlasMode m);

struct AtlasGroup {
    int id = -1;
    std::string name;         // output naming base
    std::vector<int> primitives;  // eligible primitive ids
    std::vector<int> materials;   // distinct original materials in group
};

// Groups eligible primitives into atlas groups per the mode. Primitives that
// are ineligible (validate.eligibilityReason) are left out. Incompatible data
// is never merged just to force fewer atlases.
std::vector<AtlasGroup> buildGroups(const Model& model, AtlasMode mode, int sourceUv,
                                    Report& report);

}  // namespace uvremap
