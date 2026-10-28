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

    std::map<std::set<int>, int> byMatSet;  // material-mode: candidate set -> group
    std::map<int, int> byMesh;              // mesh mode: mesh index -> group
    int sceneGroup = -1;

    // Materials a primitive can render with (authored + variant mappings).
    auto candidateMats = [](const Primitive& p) {
        std::set<int> s;
        s.insert(p.material);
        for (const VariantMapping& vm : p.variantMappings) s.insert(vm.material);
        return s;
    };

    for (int pid : eligible) {
        const Primitive& p = model.primitives[pid];
        int gid = -1;
        switch (mode) {
            case AtlasMode::Material: {
                std::set<int> mats = candidateMats(p);
                auto it = byMatSet.find(mats);
                if (it == byMatSet.end()) {
                    int firstMat = -1;
                    for (int m : mats)
                        if (m >= 0) { firstMat = m; break; }
                    std::string name = "material_" +
                        (firstMat >= 0 ? std::to_string(firstMat)
                                       : "none_" + std::to_string(pid));
                    if (mats.size() > 1) name += "_variants";
                    gid = newGroup(name);
                    byMatSet[mats] = gid;
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
        for (int pid : g.primitives) {
            const Primitive& p = model.primitives[pid];
            for (int m : candidateMats(p)) mats.insert(m);
        }
        g.materials.assign(mats.begin(), mats.end());
        if (mode != AtlasMode::Material && g.materials.size() > 1) {
            report.info("atlas group '" + g.name + "' merges " +
                        std::to_string(g.materials.size()) +
                        " materials; their charts are packed into one shared atlas");
        }
        for (int pid : g.primitives)
            if (!model.primitives[pid].variantMappings.empty()) {
                report.info("atlas group '" + g.name + "': prim " +
                            std::to_string(model.primitives[pid].meshIndex) + "/" +
                            std::to_string(model.primitives[pid].primIndex) + " carries " +
                            std::to_string(model.primitives[pid].variantMappings.size()) +
                            " material variant mapping(s); each mapped material gets a "
                            "baked copy of the atlas");
                break;
            }
    }
    return groups;
}

}  // namespace uvremap
