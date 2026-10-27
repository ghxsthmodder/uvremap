#include "core/grouping.h"

#include <map>
#include <set>

#include "core/validate.h"

namespace uvremap {

namespace {
std::string sanitizeForFile(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (std::isalnum(uint8_t(c)) || c == '_' || c == '-' || c == '.')
            out.push_back(c);
        else
            out.push_back('_');
    }
    return out.empty() ? "mesh" : out;
}
}  // namespace

bool parseAtlasMode(const std::string& s, AtlasMode& out) {
    if (s == "material") out = AtlasMode::Material;
    else if (s == "mesh") out = AtlasMode::Mesh;
    else if (s == "scene") out = AtlasMode::Scene;
    else return false;
    return true;
}

const char* atlasModeName(AtlasMode m) {
    switch (m) {
        case AtlasMode::Material: return "material";
        case AtlasMode::Mesh: return "mesh";
        case AtlasMode::Scene: return "scene";
    }
    return "?";
}

std::vector<AtlasGroup> buildGroups(const Model& model, AtlasMode mode, int sourceUv,
                                    Report& report) {
    std::vector<int> eligible;
    for (size_t i = 0; i < model.primitives.size(); i++)
        if (eligibilityReason(model, int(i), sourceUv).empty())
            eligible.push_back(int(i));

    std::vector<AtlasGroup> groups;
    auto newGroup = [&](const std::string& name) -> int {
        AtlasGroup g;
        g.id = int(groups.size());
        g.name = name;
        groups.push_back(g);
        return g.id;
    };

    std::map<int, int> byMaterial, byMesh;  // key -> group id
    int sceneGroup = -1;

    for (int pid : eligible) {
        const Primitive& p = model.primitives[pid];
        int gid = -1;
        switch (mode) {
            case AtlasMode::Material: {
                auto it = byMaterial.find(p.material);
                if (it == byMaterial.end()) {
                    std::string name = "material_" +
                        (p.material >= 0 ? std::to_string(p.material) : "none_" + std::to_string(pid));
                    gid = newGroup(name);
                    byMaterial[p.material] = gid;
                } else {
                    gid = it->second;
                }
                break;
            }
            case AtlasMode::Mesh: {
                auto it = byMesh.find(p.meshIndex);
                if (it == byMesh.end()) {
                    std::string nm = p.meshName.empty() ? ("mesh_" + std::to_string(p.meshIndex))
                                                         : ("mesh_" + sanitizeForFile(p.meshName));
                    gid = newGroup(nm);
                    byMesh[p.meshIndex] = gid;
                } else {
                    gid = it->second;
                }
                break;
            }
            case AtlasMode::Scene: {
                if (sceneGroup < 0) sceneGroup = newGroup("scene_0");
                gid = sceneGroup;
                break;
            }
        }
        groups[gid].primitives.push_back(pid);
    }

    // Distinct materials per group + warnings about what grouping means.
    for (auto& g : groups) {
        std::set<int> mats;
        for (int pid : g.primitives) mats.insert(model.primitives[pid].material);
        g.materials.assign(mats.begin(), mats.end());
        if (mode != AtlasMode::Material && g.materials.size() > 1) {
            report.info("atlas group '" + g.name + "' merges " +
                        std::to_string(g.materials.size()) +
                        " materials; each material gets its own rebuilt textures "
                        "inside the shared atlas");
        }
    }
    return groups;
}

}  // namespace uvremap
