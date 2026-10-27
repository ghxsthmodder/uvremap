#pragma once

#include "core/report.h"
#include "core/types.h"

namespace uvremap {

// Checks whether a primitive has enough data to be atlased and transferred.
// Returns empty string when eligible, otherwise the reason it is not.
std::string eligibilityReason(const Model& model, int primIdx, int sourceUv);

// Validates the whole model for a regenerate operation. Populates report with
// warnings; returns false when nothing at all can be processed (exit 4).
bool validateForRegenerate(const Model& model, int sourceUv, Report& report);

// True when the (material, type) texture can be baked into an atlas.
bool textureBakeable(const Model& model, int matIdx, TexType t);

}  // namespace uvremap
