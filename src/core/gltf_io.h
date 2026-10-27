#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/report.h"
#include "core/types.h"

namespace uvremap {

struct SaveOptions {
    std::string outputDir;       // directory where model + assets are written
    std::string modelFilename;   // e.g. "model.gltf" / "model.glb"
    bool binary = false;         // true = glb
    bool embedTextures = false;  // gltf: write images as base64 data URIs
                                 // instead of files under texturesDir
    int targetUv = 0;            // TEXCOORD set receiving the new mapping
    std::string texturesDir = "textures";
};

class GltfDocument {
  public:
    GltfDocument();
    ~GltfDocument();
    GltfDocument(GltfDocument&&) noexcept;
    GltfDocument& operator=(GltfDocument&&) noexcept;

    bool load(const std::string& path, Model& out, Report& report, std::string& err);

    // Applies regenerate results (new meshes + baked textures) to a copy of the
    // parsed document and writes model and assets to opt.outputDir.
    bool save(const Model& core, const std::vector<AtlasOutput>& atlases,
              const SaveOptions& opt, Report& report, std::string& err);

    // Reparses the written file and checks integrity of atlased primitives.
    static bool verify(const std::string& writtenPath, const SaveOptions& opt,
                       const std::vector<AtlasOutput>& atlases, Report& report,
                       std::string& err);

    const std::string& lastError() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace uvremap
