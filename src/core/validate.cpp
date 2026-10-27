#include "core/validate.h"

#include <cmath>
#include <string>

namespace uvremap {

bool textureBakeable(const Model& model, int matIdx, TexType t) {
    if (matIdx < 0 || matIdx >= int(model.materials.size())) return false;
    const TextureRef& ref = model.materials[matIdx].tex[int(t)];
    if (!ref.present) return false;
    if (ref.hasTransform && std::fabs(ref.transform.rotation) > 1e-9) return false;
    if (ref.imageIndex < 0 || ref.imageIndex >= int(model.images.size())) return false;
    return model.images[ref.imageIndex].decoded;
}

std::string eligibilityReason(const Model& model, int primIdx, int sourceUv) {
    const Primitive& p = model.primitives[primIdx];
    if (p.hasMorphTargets)
        return "has morph targets (unsupported for regeneration in this version)";
    if (!p.extensionNames.empty()) {
        std::string names;
        for (const std::string& e : p.extensionNames) {
            if (!names.empty()) names += ", ";
            names += e;
        }
        return "has primitive extensions (" + names +
               ") that cannot be safely remapped in this version";
    }
    if (p.indices.empty())
        return "has no triangles";
    if (p.vertices.empty())
        return "has no vertices";
    if (sourceUv < 0 || size_t(sourceUv) >= p.vertices[0].uvsets.size())
        return "has no TEXCOORD_" + std::to_string(sourceUv) +
               " set to sample textures from";
    // Textures that cannot be transferred safely must not be altered.
    if (p.material >= 0) {
        const Material& m = model.materials[p.material];
        for (int t = 0; t < int(TexType::Count); t++) {
            const TextureRef& ref = m.tex[t];
            if (ref.present && ref.hasTransform &&
                std::fabs(ref.transform.rotation) > 1e-9)
                return std::string("material uses KHR_texture_transform rotation on ") +
                       texTypeName(TexType(t)) + " (not transferable in this version)";
        }
    }
    // Source UVs must have non-zero area somewhere, otherwise nothing can be
    // sampled through them.
    bool anyArea = false;
    for (size_t t = 0; t + 2 < p.indices.size() && !anyArea; t += 3) {
        const V2& a = p.vertices[p.indices[t]].uvsets[sourceUv];
        const V2& b = p.vertices[p.indices[t + 1]].uvsets[sourceUv];
        const V2& c = p.vertices[p.indices[t + 2]].uvsets[sourceUv];
        float area = (b.u - a.u) * (c.v - a.v) - (c.u - a.u) * (b.v - a.v);
        if (std::fabs(area) > 1e-10f) anyArea = true;
    }
    if (!anyArea)
        return "source UV set " + std::to_string(sourceUv) +
               " is degenerate (zero area) for all triangles";
    return "";
}

bool validateForRegenerate(const Model& model, int sourceUv, Report& report) {
    int eligible = 0;
    for (size_t i = 0; i < model.primitives.size(); i++) {
        const Primitive& p = model.primitives[i];
        std::string reason = eligibilityReason(model, int(i), sourceUv);
        if (reason.empty()) {
            eligible++;
        } else {
            report.warn("mesh[" + std::to_string(p.meshIndex) + "] (" + p.meshName +
                        ") prim[" + std::to_string(p.primIndex) + "]: left unchanged — " + reason);
        }
    }
    if (eligible == 0) {
        report.error("no primitive in the model has geometry + source UVs sufficient "
                     "for texture transfer");
        return false;
    }
    return true;
}

}  // namespace uvremap
