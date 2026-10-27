#pragma once

#include "core/types.h"

namespace uvremap {

// Per-triangle tangent frame derived from positions + a UV parametrization.
struct TriFrame {
    V3 T, B, N;  // N is the frame normal implied by the UV parametrization
};

// Returns false if the parametrization is degenerate (zero uv area).
bool triangleFrame(const V3 p[3], const V2 uv[3], TriFrame& out);

V3 normalize(V3 v);
V3 cross(V3 a, V3 b);
float dot(V3 a, V3 b);

// tangent-space vector -> world/object space via frame
V3 tangentToWorld(const TriFrame& f, V3 n);
// world/object space vector -> tangent space via frame
V3 worldToTangent(const TriFrame& f, V3 n);

// Transform a tangent-space normal from the source parametrization to the
// destination parametrization of the same triangle (via object space).
V3 rebakeNormal(const TriFrame& src, const TriFrame& dst, V3 srcNormalTangentSpace);

}  // namespace uvremap
