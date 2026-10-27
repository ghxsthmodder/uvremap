#include "core/normal_math.h"

#include <cmath>

namespace uvremap {

V3 normalize(V3 v) {
    float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (l < 1e-12f) return {0, 0, 0};
    return {v.x / l, v.y / l, v.z / l};
}

V3 cross(V3 a, V3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

bool triangleFrame(const V3 p[3], const V2 uv[3], TriFrame& out) {
    V3 e1{p[1].x - p[0].x, p[1].y - p[0].y, p[1].z - p[0].z};
    V3 e2{p[2].x - p[0].x, p[2].y - p[0].y, p[2].z - p[0].z};
    V2 d1{uv[1].u - uv[0].u, uv[1].v - uv[0].v};
    V2 d2{uv[2].u - uv[0].u, uv[2].v - uv[0].v};
    float det = d1.u * d2.v - d2.u * d1.v;
    if (std::fabs(det) < 1e-20f) return false;
    float f = 1.f / det;
    out.T = normalize(V3{f * (d2.v * e1.x - d1.v * e2.x),
                         f * (d2.v * e1.y - d1.v * e2.y),
                         f * (d2.v * e1.z - d1.v * e2.z)});
    out.B = normalize(V3{f * (d1.u * e2.x - d2.u * e1.x),
                         f * (d1.u * e2.y - d2.u * e1.y),
                         f * (d1.u * e2.z - d2.u * e1.z)});
    out.N = normalize(cross(out.T, out.B));
    return true;
}

V3 tangentToWorld(const TriFrame& f, V3 n) {
    return normalize(V3{f.T.x * n.x + f.B.x * n.y + f.N.x * n.z,
                        f.T.y * n.x + f.B.y * n.y + f.N.y * n.z,
                        f.T.z * n.x + f.B.z * n.y + f.N.z * n.z});
}

V3 worldToTangent(const TriFrame& f, V3 n) {
    return normalize(V3{dot(n, f.T), dot(n, f.B), dot(n, f.N)});
}

V3 rebakeNormal(const TriFrame& src, const TriFrame& dst, V3 n) {
    return worldToTangent(dst, tangentToWorld(src, n));
}

}  // namespace uvremap
