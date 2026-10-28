#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace uvremap {

struct V2 {
    float u = 0.f, v = 0.f;
};
struct V3 {
    float x = 0.f, y = 0.f, z = 0.f;
};
struct V4 {
    float r = 0.f, g = 0.f, b = 0.f, a = 0.f;
};

enum class TexType : int {
    BaseColor = 0,
    MetallicRoughness = 1,
    Occlusion = 2,
    Emissive = 3,
    Normal = 4,
    Count = 5,
};

inline constexpr int kTexCount = static_cast<int>(TexType::Count);

// Bit marking that a material's numeric factor for this slot was baked.
constexpr int factorBit(TexType t) { return 1 << (kTexCount + static_cast<int>(t)); }

const char* texTypeName(TexType t);         // "basecolor", "normal", ...
const char* texTypeGltfSlot(TexType t);     // "baseColorTexture", ...

struct TexTransform {
    bool identity = true;
    double offsetU = 0, offsetV = 0;
    double scaleU = 1, scaleV = 1;
    double rotation = 0;  // radians, CCW around origin; applied before offset/scale
};

// Sampler wrap mode of a source texture. The bake follows it: models that tile
// UVs past 1.0 (MF188 does, up to 5x) get the wrong atlas texels if a clamped
// texture is sampled as repeating.
enum class Wrap : uint8_t { Clamp, Repeat, Mirror };

struct TextureRef {
    bool present = false;
    int imageIndex = -1;   // index into Model::images
    int texCoord = 0;      // TEXCOORD set used for sampling
    bool hasTransform = false;
    TexTransform transform;
    // A texture with no sampler declared repeats (glTF spec, section Sampler);
    // a declared sampler whose JSON omits the field defaults to clamp.
    Wrap wrapS = Wrap::Repeat;
    Wrap wrapT = Wrap::Repeat;
};

struct Material {
    std::string name;
    TextureRef tex[kTexCount];
    V4 baseColorFactor{1, 1, 1, 1};
    V3 emissiveFactor{0, 0, 0};
    float metallicFactor = 1.f;
    float roughnessFactor = 1.f;
    float occlusionStrength = 1.f;
    std::string alphaMode = "OPAQUE";
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
    bool unlit = false;
    // UV sets referenced by textureInfo inside KHR_materials_* (and similar)
    // material extensions. These textures are not rebuilt by uvremap, so the
    // sets they sample must survive the atlas rewrite (see regenerate()).
    std::vector<int> extTexCoordSets;
};

// KHR_materials_variants: which material applies to which variants on a prim.
struct VariantMapping {
    int material = -1;
    std::vector<int> variants;  // indices into Model::variantNames
};

struct SourceVertex {    V3 position;
    bool hasNormal = false;
    V3 normal;
    bool hasTangent = false;
    V4 tangent;   // w = handedness
    bool hasColor = false;
    V4 color;     // linear values as stored
    bool hasJoints = false;
    uint32_t joints[4] = {0, 0, 0, 0};
    bool hasWeights = false;
    V4 weights;
    std::vector<V2> uvsets;  // TEXCOORD_0..n
};

struct Primitive {
    int meshIndex = -1;
    int primIndex = -1;
    int material = -1;       // index into Model::materials, -1 = none
    std::vector<SourceVertex> vertices;
    std::vector<uint32_t> indices;  // triangle list, multiple of 3
    int rawMode = 4;                // glTF primitive mode as authored (4/5/6 seen)
    bool hasMorphTargets = false;
    std::vector<std::string> extensionNames;  // primitive-level extensions (draco etc.)
    std::vector<VariantMapping> variantMappings;  // KHR_materials_variants
    int jointsComponentType = 5123;  // TINYGLTF UNSIGNED_SHORT default
    std::string meshName;
};

struct Image {
    std::string name;
    std::string uri;             // original uri (empty when embedded)
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;   // decoded RGBA8, row 0 = top
    std::vector<uint8_t> raw;    // original encoded bytes (png/jpg...), if read
    bool decoded = false;
};

struct Model {
    std::string inputPath;
    std::string sourceFormat;    // "gltf" or "glb"
    std::vector<Primitive> primitives;
    std::vector<Material> materials;
    std::vector<Image> images;
    int sceneCount = 0;
    int nodeCount = 0;
    int animationCount = 0;
    std::vector<std::string> variantNames;  // KHR_materials_variants root list
    std::vector<std::string> loadWarnings;
    std::vector<std::string> extensionsUsed;
};

// Result of atlasing one primitive: new vertex layout (per xatlas output
// vertex, attributes reconstructed via xref) + new index list.
struct RemeshedPrimitive {
    int primId = -1;
    std::vector<SourceVertex> vertices;
    std::vector<uint32_t> indices;
};

// Baked texture for one (material, type) pair within an atlas. material is -1
// only when the atlas has exactly one material and per-material copies are
// unnecessary; the baker emits -1 (shared) in that case to avoid duplication.
// factorBaked has bit kTexCount+slot set when the material's numeric factor for
// that slot (baseColorFactor, metallicRoughnessFactor, occlusionStrength,
// emissiveFactor) was painted into the image instead of sampled from a texture;
// the exporter must then neutralize that factor on the cloned material. For a
// shared image factorBaked is the union over its materials, so
// factorMaterials lists the originals whose factor of this image's type was
// actually painted: a material missing from it keeps its factor on the model
// instead of sampling an island that was never written.
struct BakedImage {
    int material = -1;
    TexType type = TexType::BaseColor;
    Image image;
    int factorBaked = 0;
    std::vector<int> factorMaterials;
};

// Output of one atlas group.
struct AtlasOutput {
    std::string name;  // used for file naming: material_0, mesh_2, scene_0...
    int width = 0, height = 0;
    std::vector<RemeshedPrimitive> primitives;
    std::vector<int> materialIds;             // original materials represented
    std::vector<BakedImage> baked;            // baked images per material+type
};

}  // namespace uvremap
