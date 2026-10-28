#include "core/gltf_io.h"

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include "tiny_gltf.h"

#include "core/image.h"
#include "core/validate.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace uvremap {

bool isCompressionExtName(const std::string& n) {
    return n == "KHR_draco_mesh_compression" || n == "EXT_meshopt_compression" ||
           n == "KHR_meshopt_compression";
}

std::vector<uint32_t> triangulateIndices(int mode, const std::vector<uint32_t>& in) {
    if (mode != 5 && mode != 6) return in;
    std::vector<uint32_t> out;
    if (mode == 5) {  // TRIANGLE_STRIP: alternate winding per new triangle
        for (size_t i = 0; i + 2 < in.size(); i++) {
            if (i % 2 == 0) {
                out.push_back(in[i]);
                out.push_back(in[i + 1]);
                out.push_back(in[i + 2]);
            } else {
                out.push_back(in[i]);
                out.push_back(in[i + 2]);
                out.push_back(in[i + 1]);
            }
        }
    } else {  // TRIANGLE_FAN
        if (in.size() >= 3)
            for (size_t i = 1; i + 1 < in.size(); i++) {
                out.push_back(in[0]);
                out.push_back(in[i]);
                out.push_back(in[i + 1]);
            }
    }
    return out;
}

namespace {

// ---------------------------------------------------------------- utilities

std::string dirOf(const std::string& path) {
    fs::path p(path);
    return p.parent_path().string();
}

bool readFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    out.resize(size_t(f.tellg()));
    f.seekg(0, std::ios::beg);
    if (!out.empty()) f.read(reinterpret_cast<char*>(out.data()), out.size());
    return bool(f);
}

const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool base64Decode(const std::string& in, std::vector<uint8_t>& out) {
    int bits = 0, value = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        const char* p = std::find(std::begin(kB64), std::end(kB64) - 1, c);
        if (p == std::end(kB64) - 1) return false;
        value = (value << 6) + int(p - kB64);
        bits += 6;
        if (bits >= 8) {
            out.push_back(uint8_t((value >> (bits - 8)) & 0xff));
            bits -= 8;
        }
    }
    return true;
}

fs::path rp_filename(const std::string& s) { return fs::path(s).filename(); }

std::string sanitizeName(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (std::isalnum(uint8_t(c)) || c == '_' || c == '-' || c == '.')
            out.push_back(c);
        else
            out.push_back('_');
    }
    if (out.empty()) out = "img";
    return out;
}

// ---------------------------------------------------------------- accessors

struct AccView {
    const uint8_t* ptr = nullptr;
    size_t count = 0;
    size_t stride = 0;
    int componentType = -1;
    int type = -1;
    bool normalized = false;
    size_t components = 0;
};

int getAttr(const tinygltf::Primitive& p, const std::string& name) {
    auto it = p.attributes.find(name);
    return it == p.attributes.end() ? -1 : it->second;
}

bool accessorView(const tinygltf::Model& m, int accIdx, AccView& out) {
    if (accIdx < 0 || accIdx >= int(m.accessors.size())) return false;
    const tinygltf::Accessor& a = m.accessors[accIdx];
    if (a.bufferView < 0) return false;
    const tinygltf::BufferView& bv = m.bufferViews[a.bufferView];
    if (bv.buffer < 0 || bv.buffer >= int(m.buffers.size())) return false;
    const size_t compSize = tinygltf::GetComponentSizeInBytes(uint32_t(a.componentType));
    const size_t numComp = size_t(tinygltf::GetNumComponentsInType(uint32_t(a.type)));
    const size_t elemSize = compSize * numComp;
    if (!elemSize || !compSize) return false;
    out.components = numComp;
    out.stride = bv.byteStride ? bv.byteStride : elemSize;
    const uint8_t* base = nullptr;
    if (!m.buffers[bv.buffer].data.empty())
        base = m.buffers[bv.buffer].data.data();
    else if (a.count == 0)
        base = nullptr;
    else
        return false;
    out.ptr = base + bv.byteOffset + a.byteOffset;
    out.count = a.count;
    out.componentType = a.componentType;
    out.type = a.type;
    out.normalized = a.normalized;
    return true;
}

float intToFloat(int ct, bool normalized, const void* p) {
    float v = 0.f;
    switch (ct) {
        case TINYGLTF_COMPONENT_TYPE_BYTE: v = float(*reinterpret_cast<const int8_t*>(p)); break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: v = float(*reinterpret_cast<const uint8_t*>(p)); break;
        case TINYGLTF_COMPONENT_TYPE_SHORT: v = float(*reinterpret_cast<const int16_t*>(p)); break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: v = float(*reinterpret_cast<const uint16_t*>(p)); break;
        case TINYGLTF_COMPONENT_TYPE_INT: v = float(*reinterpret_cast<const int32_t*>(p)); break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: v = float(*reinterpret_cast<const uint32_t*>(p)); break;
        default: return 0.f;
    }
    if (normalized) {
        switch (ct) {
            case TINYGLTF_COMPONENT_TYPE_BYTE:
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: v /= 255.f; break;
            case TINYGLTF_COMPONENT_TYPE_SHORT:
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: v /= 65535.f; break;
            default: break;
        }
    }
    return v;
}

bool readFloats(const tinygltf::Model& m, int accIdx, std::vector<float>& out) {
    AccView v;
    if (!accessorView(m, accIdx, v)) return false;
    out.resize(v.count * v.components);
    for (size_t i = 0; i < v.count; i++) {
        const uint8_t* row = v.ptr + i * v.stride;
        for (size_t c = 0; c < v.components; c++) {
            const uint8_t* e = row + c * tinygltf::GetComponentSizeInBytes(uint32_t(v.componentType));
            if (v.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT)
                std::memcpy(&out[i * v.components + c], e, 4);
            else
                out[i * v.components + c] = intToFloat(v.componentType, v.normalized, e);
        }
    }
    return true;
}

bool readUints(const tinygltf::Model& m, int accIdx, std::vector<uint32_t>& out) {
    AccView v;
    if (!accessorView(m, accIdx, v)) return false;
    out.resize(v.count * v.components);
    for (size_t i = 0; i < v.count; i++) {
        const uint8_t* row = v.ptr + i * v.stride;
        for (size_t c = 0; c < v.components; c++) {
            const uint8_t* e = row + c * tinygltf::GetComponentSizeInBytes(uint32_t(v.componentType));
            uint32_t val = 0;
            switch (v.componentType) {
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: val = *e; break;
                case TINYGLTF_COMPONENT_TYPE_BYTE: val = uint32_t(*reinterpret_cast<const int8_t*>(e)); break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: std::memcpy(&val, e, 2); break;
                case TINYGLTF_COMPONENT_TYPE_SHORT: { int16_t s; std::memcpy(&s, e, 2); val = uint32_t(s); break; }
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: std::memcpy(&val, e, 4); break;
                default: return false;
            }
            out[i * v.components + c] = val;
        }
    }
    return true;
}

const tinygltf::Value* findExt(const tinygltf::ExtensionMap& exts, const std::string& name) {
    auto it = exts.find(name);
    if (it == exts.end()) return nullptr;
    return &it->second;
}

// Collects texCoord indices from every textureInfo-shaped object (one holding
// an integer "index") inside a material extension value, e.g. the
// clearcoatTexture/clearcoatNormalTexture entries of KHR_materials_clearcoat.
void collectExtTexCoords(const tinygltf::Value& v, std::vector<int>& out, int depth) {
    if (depth > 4) return;
    if (v.IsObject()) {
        if (v.Has("index") && v.Get("index").IsNumber()) {
            int tc = 0;
            if (v.Has("texCoord") && v.Get("texCoord").IsNumber())
                tc = v.Get("texCoord").GetNumberAsInt();
            if (tc >= 0 && std::find(out.begin(), out.end(), tc) == out.end())
                out.push_back(tc);
        }
        for (const std::string& k : v.Keys())
            collectExtTexCoords(v.Get(k), out, depth + 1);
    } else if (v.IsArray()) {
        for (size_t i = 0; i < v.ArrayLen(); i++)
            collectExtTexCoords(v.Get(int(i)), out, depth + 1);
    }
}

template <typename TI>
void parseTexTransform(const TI& ti, TextureRef& ref) {
    const tinygltf::Value* t = findExt(ti.extensions, "KHR_texture_transform");
    if (!t) return;
    ref.hasTransform = true;
    TexTransform& x = ref.transform;
    x.identity = false;
    if (!t->IsObject()) return;
    auto readNumArray = [&](const std::string& key, double* dst, size_t n) {
        if (!t->Has(key)) return;
        const tinygltf::Value& v = t->Get(key);
        if (!v.IsArray()) return;
        for (size_t i = 0; i < n && i < v.ArrayLen(); i++) {
            const tinygltf::Value& e = v.Get(i);
            if (e.IsNumber()) dst[i] = e.GetNumberAsDouble();
        }
    };
    readNumArray("offset", &x.offsetU, 2);
    readNumArray("scale", &x.scaleU, 2);
    if (t->Has("rotation")) {
        const tinygltf::Value& r = t->Get("rotation");
        if (r.IsNumber()) x.rotation = r.GetNumberAsDouble();
    }
    if (t->Has("texCoord")) {
        const tinygltf::Value& tc = t->Get("texCoord");
        if (tc.IsNumber()) ref.texCoord = tc.GetNumberAsInt();
    }
}

// Gets the raw encoded bytes of a glTF image (png/jpg/...).
bool rawImageBytes(const tinygltf::Model& m, const tinygltf::Image& img,
                   const std::string& baseDir, std::vector<uint8_t>& out,
                   std::string& problem) {
    if (img.bufferView >= 0) {
        const tinygltf::BufferView& bv = m.bufferViews[img.bufferView];
        if (bv.buffer < 0 || bv.buffer >= int(m.buffers.size())) {
            problem = "image bufferView points to invalid buffer";
            return false;
        }
        const std::vector<uint8_t>& buf = m.buffers[bv.buffer].data;
        if (bv.byteOffset + bv.byteLength > buf.size()) {
            problem = "image bufferView out of range";
            return false;
        }
        out.assign(buf.begin() + bv.byteOffset, buf.begin() + bv.byteOffset + bv.byteLength);
        return true;
    }
    if (!img.uri.empty()) {
        if (img.uri.rfind("data:", 0) == 0) {
            size_t comma = img.uri.find(',');
            if (comma == std::string::npos) {
                problem = "malformed data URI";
                return false;
            }
            return base64Decode(img.uri.substr(comma + 1), out);
        }
        fs::path p(fs::path(baseDir) / img.uri);
        if (!readFileBytes(p.string(), out)) {
            problem = "referenced image file not found: " + p.string();
            return false;
        }
        return true;
    }
    problem = "image has neither uri nor bufferView";
    return false;
}

// Reads one texture slot (TextureInfo/Normal/Occlusion) into a TextureRef.
template <typename TI>
void readTextureSlot(const tinygltf::Model& m, const TI& ti,
                     TextureRef& ref, Report& rep, const std::string& what) {
    if (ti.index < 0) return;
    ref.present = true;
    ref.texCoord = ti.texCoord;
    const tinygltf::Texture& tex = m.textures[ti.index];
    ref.imageIndex = tex.source;
    parseTexTransform(ti, ref);
}

// tinygltf demands a loader callback; decoding is done by us via stb
// (rawImageBytes + decodeImage), so this only acknowledges the encoded bytes.
bool noopImageLoader(tinygltf::Image*, const int, std::string*, std::string*, int, int,
                     const unsigned char*, int, void*) {
    return true;
}

// meshopt glTF keeps its payload in a uri-less fallback buffer that tinygltf
// rejects outright; detect required compression extensions in the raw JSON text
// first so the failure message is actionable. Returns the extension name, or
// "" if none is required. (Text scan of the extensionsRequired array only, so
// extensions merely declared-but-optional do not trigger rejection.)
std::string jsonCompressionExt(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    size_t key = s.find("\"extensionsRequired\"");
    if (key == std::string::npos) return "";
    size_t open = s.find('[', key);
    size_t close = s.find(']', open);
    if (open == std::string::npos || close == std::string::npos) return "";
    std::string arr = s.substr(open, close - open + 1);
    for (const char* cand : {"KHR_draco_mesh_compression", "EXT_meshopt_compression",
                             "KHR_meshopt_compression"})
        if (arr.find(cand) != std::string::npos) return cand;
    return "";
}

}  // namespace

// ================================================================ Impl

struct GltfDocument::Impl {
    tinygltf::Model tg;
    std::string baseDir;
    std::string err;

    bool load(const std::string& path, Model& out, Report& rep) {
        tinygltf::TinyGLTF loader;
        loader.SetImageLoader(noopImageLoader, nullptr);
        std::string warn;
        bool ret = false;
        if (path.size() >= 4 && path.substr(path.size() - 4) == ".glb")
            ret = loader.LoadBinaryFromFile(&tg, &err, &warn, path);
        else
            ret = loader.LoadASCIIFromFile(&tg, &err, &warn, path);
        if (!warn.empty()) rep.warn("tinygltf: " + warn);
        if (!ret) {
            if (err.empty()) err = "failed to parse " + path;
            return false;
        }
        baseDir = dirOf(path);
        out.inputPath = path;
        out.sourceFormat = (tg.buffers.size() && tg.buffers[0].data.size() &&
                            path.size() >= 4 && path.substr(path.size() - 4) == ".glb")
                               ? "glb" : "gltf";
        if (path.size() >= 4 && path.substr(path.size() - 4) == ".glb")
            out.sourceFormat = "glb";
        out.sceneCount = int(tg.scenes.size());
        out.nodeCount = int(tg.nodes.size());
        out.animationCount = int(tg.animations.size());
        out.extensionsUsed = tg.extensionsUsed;
        for (const auto& e : tg.extensionsRequired) {
            if (isCompressionExtName(e)) {
                err = "input requires " + e +
                      " (compressed geometry); decoding compressed meshes is not "
                      "supported in this version — provide an uncompressed copy";
                return false;
            }
        }

        // KHR_materials_variants: root variant names.
        if (const tinygltf::Value* rv =
                findExt(tg.extensions, "KHR_materials_variants")) {
            if (rv->IsObject() && rv->Has("variants")) {
                const tinygltf::Value& va = rv->Get("variants");
                if (va.IsArray()) {
                    for (size_t i = 0; i < va.ArrayLen(); i++) {
                        const tinygltf::Value& vv = va.Get(i);
                        std::string name;
                        if (vv.IsObject() && vv.Has("name") && vv.Get("name").IsString())
                            name = vv.Get("name").Get<std::string>();
                        if (name.empty()) name = "variant_" + std::to_string(i);
                        out.variantNames.push_back(name);
                    }
                }
            }
        }

        // Images: read raw bytes + decode with stb.
        out.images.resize(tg.images.size());
        for (size_t i = 0; i < tg.images.size(); i++) {
            Image& img = out.images[i];
            const tinygltf::Image& ti = tg.images[i];
            img.name = ti.name.empty() ? ("image_" + std::to_string(i)) : ti.name;
            img.uri = ti.uri;
            std::string problem;
            std::vector<uint8_t> raw;
            if (rawImageBytes(tg, ti, baseDir, raw, problem)) {
                img.raw = std::move(raw);
                Raster r;
                std::string derr;
                if (decodeImage(img.raw, r, derr)) {
                    img.w = r.w;
                    img.h = r.h;
                    img.rgba = std::move(r.rgba);
                    img.decoded = true;
                } else {
                    rep.warn("image[" + std::to_string(i) + "] (" + img.name +
                             "): could not be decoded (" + derr +
                             "); appearance from it cannot be transferred");
                }
            } else {
                rep.warn("image[" + std::to_string(i) + "] (" + img.name + "): " + problem);
            }
        }

        // Materials (1:1 index mapping with tinygltf).
        out.materials.resize(tg.materials.size());
        for (size_t i = 0; i < tg.materials.size(); i++) {
            const tinygltf::Material& tm = tg.materials[i];
            Material& m = out.materials[i];
            m.name = tm.name;
            if (tm.pbrMetallicRoughness.baseColorFactor.size() == 4) {
                const auto& bf = tm.pbrMetallicRoughness.baseColorFactor;
                m.baseColorFactor = V4{float(bf[0]), float(bf[1]), float(bf[2]), float(bf[3])};
            }
            if (tm.emissiveFactor.size() == 3) {
                m.emissiveFactor = V3{float(tm.emissiveFactor[0]), float(tm.emissiveFactor[1]),
                                      float(tm.emissiveFactor[2])};
            }
            m.metallicFactor = float(tm.pbrMetallicRoughness.metallicFactor);
            m.roughnessFactor = float(tm.pbrMetallicRoughness.roughnessFactor);
            m.alphaMode = tm.alphaMode;
            m.alphaCutoff = float(tm.alphaCutoff);
            m.doubleSided = tm.doubleSided;
            m.unlit = findExt(tm.extensions, "KHR_materials_unlit") != nullptr;
            for (const auto& kv : tm.extensions)
                collectExtTexCoords(kv.second, m.extTexCoordSets, 0);
            std::string what = "material[" + std::to_string(i) + "] " + tm.name;
            readTextureSlot(tg, tm.pbrMetallicRoughness.baseColorTexture,
                            m.tex[int(TexType::BaseColor)], rep, what + "/baseColor");
            readTextureSlot(tg, tm.pbrMetallicRoughness.metallicRoughnessTexture,
                            m.tex[int(TexType::MetallicRoughness)], rep, what + "/metallicRoughness");
            if (tm.occlusionTexture.index >= 0) {
                m.occlusionStrength = float(tm.occlusionTexture.strength);
                readTextureSlot(tg, tm.occlusionTexture, m.tex[int(TexType::Occlusion)], rep,
                                what + "/occlusion");
            }
            readTextureSlot(tg, tm.emissiveTexture, m.tex[int(TexType::Emissive)], rep,
                            what + "/emissive");
            readTextureSlot(tg, tm.normalTexture, m.tex[int(TexType::Normal)], rep,
                            what + "/normal");
        }

        // Meshes / primitives.
        for (size_t mi = 0; mi < tg.meshes.size(); mi++) {
            const tinygltf::Mesh& mesh = tg.meshes[mi];
            for (size_t pi = 0; pi < mesh.primitives.size(); pi++) {
                const tinygltf::Primitive& tp = mesh.primitives[pi];
                Primitive p;
                p.meshIndex = int(mi);
                p.primIndex = int(pi);
                p.material = tp.material;
                p.meshName = mesh.name;
                p.hasMorphTargets = !tp.targets.empty();
                for (const auto& e : tp.extensions) p.extensionNames.push_back(e.first);
                {
                    bool compressed = false;
                    for (const auto& e : tp.extensions) {
                        if (isCompressionExtName(e.first)) {
                            rep.warn("mesh[" + std::to_string(mi) + "]/prim[" +
                                     std::to_string(pi) + "]: " + e.first +
                                     " (compressed geometry) cannot be decoded in this "
                                     "version; primitive left unchanged");
                            compressed = true;
                            break;
                        }
                    }
                    if (compressed) continue;
                }
                // KHR_materials_variants mappings for this primitive.
                if (const tinygltf::Value* mv =
                        findExt(tp.extensions, "KHR_materials_variants")) {
                    if (mv->IsObject() && mv->Has("mappings")) {
                        const tinygltf::Value& ma = mv->Get("mappings");
                        if (ma.IsArray()) {
                            for (size_t k = 0; k < ma.ArrayLen(); k++) {
                                const tinygltf::Value& mo = ma.Get(k);
                                if (!mo.IsObject() || !mo.Has("material")) continue;
                                const tinygltf::Value& mm = mo.Get("material");
                                if (!mm.IsInt()) continue;
                                VariantMapping vm;
                                vm.material = mm.GetNumberAsInt();
                                if (vm.material < 0 ||
                                    vm.material >= int(tg.materials.size()))
                                    continue;
                                if (mo.Has("variants") && mo.Get("variants").IsArray()) {
                                    const tinygltf::Value& vi = mo.Get("variants");
                                    for (size_t q = 0; q < vi.ArrayLen(); q++) {
                                        if (!vi.Get(q).IsInt()) continue;
                                        int v = vi.Get(q).GetNumberAsInt();
                                        if (v >= 0 && v < int(out.variantNames.size()))
                                            vm.variants.push_back(v);
                                    }
                                }
                                p.variantMappings.push_back(std::move(vm));
                            }
                        }
                    }
                }

                std::vector<float> pos, nrm, tan, col;
                std::vector<uint32_t> idx, joints;
                std::vector<float> weights;
                std::vector<std::vector<float>> uvs;
                if (!readFloats(tg, getAttr(tp, "POSITION"), pos) || pos.empty()) {
                    rep.warn("mesh[" + std::to_string(mi) + "]/prim[" + std::to_string(pi) +
                             "]: missing or unreadable POSITION; primitive skipped");
                    continue;
                }
                size_t vertCount = pos.size() / 3;
                p.vertices.resize(vertCount);
                for (size_t v = 0; v < vertCount; v++)
                    p.vertices[v].position = {pos[v * 3], pos[v * 3 + 1], pos[v * 3 + 2]};

                int a;
                if ((a = getAttr(tp, "NORMAL")) >= 0) {
                    if (readFloats(tg, a, nrm) && nrm.size() >= vertCount * 3)
                        for (size_t v = 0; v < vertCount; v++) {
                            p.vertices[v].hasNormal = true;
                            p.vertices[v].normal = {nrm[v * 3], nrm[v * 3 + 1], nrm[v * 3 + 2]};
                        }
                }
                if ((a = getAttr(tp, "TANGENTS")) >= 0) {
                    if (readFloats(tg, a, tan) && tan.size() >= vertCount * 4)
                        for (size_t v = 0; v < vertCount; v++) {
                            p.vertices[v].hasTangent = true;
                            p.vertices[v].tangent = {tan[v * 4], tan[v * 4 + 1], tan[v * 4 + 2],
                                                    tan[v * 4 + 3]};
                        }
                }
                if ((a = getAttr(tp, "COLOR_0")) >= 0) {
                    AccView av;
                    if (accessorView(tg, a, av) && av.components == 4) {
                        if (readFloats(tg, a, col) && col.size() >= vertCount * 4)
                            for (size_t v = 0; v < vertCount; v++) {
                                p.vertices[v].hasColor = true;
                                p.vertices[v].color = {col[v * 4], col[v * 4 + 1], col[v * 4 + 2],
                                                       col[v * 4 + 3]};
                            }
                    }
                }
                if ((a = getAttr(tp, "JOINTS_0")) >= 0) {
                    AccView av;
                    if (accessorView(tg, a, av) && av.components == 4 &&
                        readUints(tg, a, joints) && joints.size() >= vertCount * 4) {
                        p.jointsComponentType = av.componentType;
                        for (size_t v = 0; v < vertCount; v++) {
                            p.vertices[v].hasJoints = true;
                            for (int c = 0; c < 4; c++)
                                p.vertices[v].joints[c] = joints[v * 4 + c];
                        }
                    }
                }
                if ((a = getAttr(tp, "WEIGHTS_0")) >= 0) {
                    if (readFloats(tg, a, weights) && weights.size() >= vertCount * 4)
                        for (size_t v = 0; v < vertCount; v++) {
                            p.vertices[v].hasWeights = true;
                            p.vertices[v].weights = {weights[v * 4], weights[v * 4 + 1],
                                                     weights[v * 4 + 2], weights[v * 4 + 3]};
                        }
                }
                for (int k = 0;; k++) {
                    std::string name = "TEXCOORD_" + std::to_string(k);
                    a = getAttr(tp, name);
                    if (a < 0) break;
                    std::vector<float> uvf;
                    if (readFloats(tg, a, uvf) && uvf.size() >= vertCount * 2) {
                        if (int(uvs.size()) <= k) uvs.resize(k + 1);
                        uvs[k] = std::move(uvf);
                    }
                }
                for (size_t v = 0; v < vertCount; v++) {
                    for (size_t k = 0; k < uvs.size(); k++)
                        if (!uvs[k].empty())
                            p.vertices[v].uvsets.push_back(
                                {uvs[k][v * 2], uvs[k][v * 2 + 1]});
                        else
                            p.vertices[v].uvsets.push_back({0, 0});
                }

                if (tp.indices >= 0) {
                    if (!readUints(tg, tp.indices, idx)) {
                        rep.warn("mesh[" + std::to_string(mi) + "]/prim[" + std::to_string(pi) +
                                 "]: unreadable index buffer; primitive skipped");
                        continue;
                    }
                } else {
                    idx.resize(vertCount);
                    for (size_t v = 0; v < vertCount; v++) idx[v] = uint32_t(v);
                }
                p.rawMode = tp.mode;
                if (tp.mode == 5 || tp.mode == 6) {
                    idx = triangulateIndices(tp.mode, idx);
                } else if (tp.mode != 4) {
                    rep.warn("mesh[" + std::to_string(mi) + "]/prim[" + std::to_string(pi) +
                             "]: primitive mode " + std::to_string(tp.mode) +
                             " is not a triangle primitive; left unchanged");
                    continue;
                }
                idx.resize(idx.size() / 3 * 3);  // triangle list
                p.indices = std::move(idx);
                if (p.indices.empty()) {
                    rep.warn("mesh[" + std::to_string(mi) + "]/prim[" + std::to_string(pi) +
                             "]: no triangles; primitive skipped");
                    continue;
                }
                out.primitives.push_back(std::move(p));
            }
        }
        return true;
    }
};

GltfDocument::GltfDocument() : impl_(new Impl()) {}
GltfDocument::~GltfDocument() = default;
GltfDocument::GltfDocument(GltfDocument&&) noexcept = default;
GltfDocument& GltfDocument::operator=(GltfDocument&&) noexcept = default;

bool GltfDocument::load(const std::string& path, Model& out, Report& report,
                        std::string& err) {
    if (!fs::exists(path)) {
        err = "input file does not exist: " + path;
        return false;
    }
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".gltf") {
        std::string comp = jsonCompressionExt(path);
        if (!comp.empty()) {
            err = "input requires " + comp +
                  " (compressed geometry); decoding compressed meshes is not "
                  "supported in this version — provide an uncompressed copy";
            return false;
        }
    }
    if (!impl_->load(path, out, report)) {
        err = impl_->err;
        return false;
    }
    return true;
}

const std::string& GltfDocument::lastError() const { return impl_->err; }

// ================================================================ save

namespace {

struct BufWriter {
    std::vector<uint8_t>* data;
    size_t pos;
    void align4() {
        while (pos % 4) {
            data->push_back(0);
            pos++;
        }
    }
    template <typename T>
    void append(const std::vector<T>& v) {
        align4();
        size_t off = pos;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(v.data());
        data->insert(data->begin() + pos, p, p + v.size() * sizeof(T));
        pos += v.size() * sizeof(T);
        lastOffset = off;
        lastLength = v.size() * sizeof(T);
    }
    void appendRaw(const uint8_t* p, size_t n) {
        align4();
        size_t off = pos;
        data->insert(data->begin() + pos, p, p + n);
        pos += n;
        lastOffset = off;
        lastLength = n;
    }
    size_t lastOffset = 0, lastLength = 0;
};

// Merges all buffers into buffer 0 (needed for glb output).
bool consolidateBuffers(tinygltf::Model& tg, std::string& err) {
    if (tg.buffers.size() <= 1) return true;
    std::vector<size_t> base(tg.buffers.size(), 0);
    size_t total = 0;
    for (size_t i = 0; i < tg.buffers.size(); i++) {
        base[i] = total;
        total += tg.buffers[i].data.size();
        total = (total + 3) & ~size_t(3);
    }
    std::vector<uint8_t> merged(total, 0);
    for (size_t i = 0; i < tg.buffers.size(); i++)
        std::memcpy(merged.data() + base[i], tg.buffers[i].data.data(),
                    tg.buffers[i].data.size());
    for (auto& bv : tg.bufferViews) {
        bv.byteOffset += base[bv.buffer];
        bv.buffer = 0;
    }
    tg.buffers[0].data = std::move(merged);
    tg.buffers[0].uri.clear();
    tg.buffers.resize(1);
    return true;
}

int addBufferViewAccessor(tinygltf::Model& tg, BufWriter& bw,
                          const std::vector<float>& floats, int compType,
                          int type, size_t count, int target,
                          const float* minV, const float* maxV) {
    bw.append(floats);
    tinygltf::BufferView bv;
    bv.buffer = 0;
    bv.byteOffset = bw.lastOffset;
    bv.byteLength = bw.lastLength;
    bv.target = target;
    int bvi = int(tg.bufferViews.size());
    tg.bufferViews.push_back(bv);
    tinygltf::Accessor acc;
    acc.bufferView = bvi;
    acc.byteOffset = 0;
    acc.componentType = compType;
    acc.count = count;
    acc.type = type;
    if (minV && maxV) {
        int n = tinygltf::GetNumComponentsInType(uint32_t(type));
        acc.minValues.assign(minV, minV + n);
        acc.maxValues.assign(maxV, maxV + n);
    }
    tg.accessors.push_back(acc);
    return int(tg.accessors.size()) - 1;
}

int addUintAccessor(tinygltf::Model& tg, BufWriter& bw,
                    const std::vector<uint32_t>& vals, int type, int target) {
    bw.append(vals);
    tinygltf::BufferView bv;
    bv.buffer = 0;
    bv.byteOffset = bw.lastOffset;
    bv.byteLength = bw.lastLength;
    bv.target = target;
    int bvi = int(tg.bufferViews.size());
    tg.bufferViews.push_back(bv);
    tinygltf::Accessor acc;
    acc.bufferView = bvi;
    acc.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
    acc.count = vals.size() / size_t(tinygltf::GetNumComponentsInType(uint32_t(type)));
    acc.type = type;
    tg.accessors.push_back(acc);
    return int(tg.accessors.size()) - 1;
}

int addIndicesAccessor(tinygltf::Model& tg, BufWriter& bw,
                       const std::vector<uint32_t>& idx) {
    bw.append(idx);
    tinygltf::BufferView bv;
    bv.buffer = 0;
    bv.byteOffset = bw.lastOffset;
    bv.byteLength = bw.lastLength;
    bv.target = 34963;
    int bvi = int(tg.bufferViews.size());
    tg.bufferViews.push_back(bv);
    tinygltf::Accessor acc;
    acc.bufferView = bvi;
    acc.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
    acc.count = idx.size();
    acc.type = TINYGLTF_TYPE_SCALAR;
    tg.accessors.push_back(acc);
    return int(tg.accessors.size()) - 1;
}

std::string encodeBase64(const uint8_t* p, size_t n) {
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i + 2 < n; i += 3) {
        uint32_t v = (p[i] << 16) | (p[i + 1] << 8) | p[i + 2];
        out += {kB64[v >> 18], kB64[(v >> 12) & 63], kB64[(v >> 6) & 63], kB64[v & 63]};
    }
    if (n % 3 == 1) {
        uint32_t v = p[n - 1] << 16;
        out += {kB64[v >> 18], kB64[(v >> 12) & 63], '=', '='};
    } else if (n % 3 == 2) {
        uint32_t v = (p[n - 2] << 16) | (p[n - 1] << 8);
        out += {kB64[v >> 18], kB64[(v >> 12) & 63], kB64[(v >> 6) & 63], '='};
    }
    return out;
}

std::string guessImageMime(const std::string& uri, const uint8_t* raw, size_t n) {
    if (raw && n >= 3 && raw[0] == 0xFF && raw[1] == 0xD8) return "image/jpeg";
    if (raw && n >= 8 && raw[1] == 'P' && raw[2] == 'N' && raw[3] == 'G') return "image/png";
    fs::path ext = fs::path(uri).extension();
    std::string e = ext.string();
    for (auto& c : e) c = char(std::tolower(c));
    if (e == ".jpg" || e == ".jpeg") return "image/jpeg";
    return "image/png";
}

std::string dataUri(const std::string& mime, const uint8_t* p, size_t n) {
    return "data:" + mime + ";base64," + encodeBase64(p, n);
}

}  // namespace

bool GltfDocument::save(const Model& core, const std::vector<AtlasOutput>& atlases,
                        const SaveOptions& opt, Report& report, std::string& err) {
    tinygltf::Model& tg = impl_->tg;
    if (opt.binary && !consolidateBuffers(tg, err)) return false;

    // Ensure a single writable buffer 0 for non-binary too.
    if (tg.buffers.empty()) {
        tinygltf::Buffer b;
        tg.buffers.push_back(b);
    }
    // GLB: tinygltf emits the BIN chunk only when buffer 0 has no URI; any
    // leftover .bin/data: URI from the input would be embedded into the JSON.
    if (opt.binary)
        tg.buffers[0].uri.clear();
    else if (tg.buffers[0].uri.empty())
        tg.buffers[0].uri = fs::path(opt.modelFilename).stem().string() + ".bin";

    BufWriter bw{&tg.buffers[0].data, tg.buffers[0].data.size()};

    fs::path outDir(opt.outputDir);
    fs::path texDir = outDir / opt.texturesDir;
    if (!opt.binary && !opt.embedTextures) fs::create_directories(texDir);

    // primId -> atlas
    std::vector<int> primAtlas(core.primitives.size(), -1);
    for (size_t a = 0; a < atlases.size(); a++)
        for (const auto& rp : atlases[a].primitives)
            if (rp.primId >= 0 && rp.primId < int(primAtlas.size()))
                primAtlas[rp.primId] = int(a);

    // New images/textures bookkeeping.
    auto addBakedImage = [&](const AtlasOutput& ao, const BakedImage& bi) -> int {
        // Returns texture index for this baked image.
        std::string base = ao.name + "_" + texTypeName(bi.type);
        if (bi.material >= 0) base += "_mat" + std::to_string(bi.material);
        const Image& img = bi.image;
        int imageIdx = -1;
        std::vector<uint8_t> png;
        if (!img.raw.empty()) {
            png = img.raw;  // baked images arrive PNG-encoded (memory)
        } else {
            Raster r;
            r.w = img.w;
            r.h = img.h;
            r.rgba = img.rgba;
            if (!encodePng(r, png)) return -1;
        }
        if (opt.binary) {
            bw.appendRaw(png.data(), png.size());
            tinygltf::BufferView bv;
            bv.buffer = 0;
            bv.byteOffset = bw.lastOffset;
            bv.byteLength = bw.lastLength;
            bv.target = 0;
            tg.bufferViews.push_back(bv);
            tinygltf::Image ti;
            ti.name = base;
            ti.bufferView = int(tg.bufferViews.size()) - 1;
            ti.mimeType = "image/png";
            tg.images.push_back(ti);
            imageIdx = int(tg.images.size()) - 1;
        } else if (opt.embedTextures) {
            tinygltf::Image ti;
            ti.name = base;
            ti.uri = dataUri("image/png", png.data(), png.size());
            tg.images.push_back(ti);
            imageIdx = int(tg.images.size()) - 1;
        } else {
            fs::path p = texDir / (base + ".png");
            std::ofstream f(p, std::ios::binary);
            if (!f) {
                err = "failed to write " + p.string();
                return -1;
            }
            f.write(reinterpret_cast<char*>(png.data()), png.size());
            f.close();
            tinygltf::Image ti;
            ti.name = base;
            ti.uri = opt.texturesDir + "/" + base + ".png";
            tg.images.push_back(ti);
            imageIdx = int(tg.images.size()) - 1;
        }
        tinygltf::Texture tex;
        tex.source = imageIdx;
        tex.name = base;
        tg.textures.push_back(tex);
        return int(tg.textures.size()) - 1;
    };

    // Clone materials for atlased primitives; map (atlas, oldMat) -> newMat.
    std::map<std::pair<int, int>, int> clonedMats;
    for (size_t ai = 0; ai < atlases.size(); ai++) {
        const AtlasOutput& ao = atlases[ai];
        // Baked textures: per (material, type) with a (shared, type) fallback.
        std::map<std::pair<int, int>, int> texByMatType;  // (material|-1, TexType) -> texture
        std::map<std::pair<int, int>, int> bitsByMatType;  // factor-baked bits
        for (const BakedImage& bi : ao.baked) {
            int texIdx = addBakedImage(ao, bi);
            if (texIdx < 0) return false;
            texByMatType[{bi.material, int(bi.type)}] = texIdx;
            bitsByMatType[{bi.material, int(bi.type)}] = bi.factorBaked;
            report.stats.textureMapsBaked++;
        }
        for (int oldMat : ao.materialIds) {
            auto key = std::make_pair(int(ai), oldMat);
            if (clonedMats.count(key)) continue;
            tinygltf::Material m2 = tg.materials[oldMat];
            int bakedFactors = 0;
            auto setSlot = [&](TexType t, auto& slotRef) {
                const TextureRef& coreRef = core.materials[oldMat].tex[int(t)];
                bool factorOnly = !coreRef.present;
                if (!factorOnly && !textureBakeable(core, oldMat, t)) return;  // keep slot as-is
                auto it = texByMatType.find({oldMat, int(t)});
                if (it == texByMatType.end()) it = texByMatType.find({-1, int(t)});
                if (it == texByMatType.end()) return;
                auto bit = bitsByMatType.find(it->first);
                if (factorOnly && !(bit->second & factorBit(t)))
                    return;  // no texture here and the factor was not painted in
                if (factorOnly) bakedFactors |= factorBit(t);
                slotRef.index = it->second;
                slotRef.texCoord = opt.targetUv;
                slotRef.extensions.erase("KHR_texture_transform");
            };
            setSlot(TexType::BaseColor, m2.pbrMetallicRoughness.baseColorTexture);
            setSlot(TexType::MetallicRoughness, m2.pbrMetallicRoughness.metallicRoughnessTexture);
            setSlot(TexType::Occlusion, m2.occlusionTexture);
            setSlot(TexType::Emissive, m2.emissiveTexture);
            setSlot(TexType::Normal, m2.normalTexture);
            // Neutralize factors that were painted into the baked image.
            if (bakedFactors & factorBit(TexType::BaseColor))
                m2.pbrMetallicRoughness.baseColorFactor = {1.0, 1.0, 1.0, 1.0};
            if (bakedFactors & factorBit(TexType::MetallicRoughness)) {
                m2.pbrMetallicRoughness.metallicFactor = 1.0;
                m2.pbrMetallicRoughness.roughnessFactor = 1.0;
            }
            if (bakedFactors & factorBit(TexType::Occlusion)) {
                m2.occlusionTexture.strength = 1.0;
                if (m2.occlusionTexture.index < 0) m2.occlusionTexture.index = 0;
            }
            if (bakedFactors & factorBit(TexType::Emissive)) m2.emissiveFactor = {0.0, 0.0, 0.0};
            tg.materials.push_back(m2);
            clonedMats[key] = int(tg.materials.size()) - 1;
        }

        // Rebuild atlased primitives.
        for (const RemeshedPrimitive& rp : ao.primitives) {
            const Primitive& cp = core.primitives[rp.primId];
            tinygltf::Mesh& mesh = tg.meshes[cp.meshIndex];
            tinygltf::Primitive& tp = mesh.primitives[cp.primIndex];

            size_t n = rp.vertices.size();
            std::vector<float> pos(n * 3), nrm, tan, col, wts, uvs;
            std::vector<uint32_t> jnts;
            float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
            for (size_t v = 0; v < n; v++) {
                const SourceVertex& sv = rp.vertices[v];
                pos[v * 3] = sv.position.x;
                pos[v * 3 + 1] = sv.position.y;
                pos[v * 3 + 2] = sv.position.z;
                for (int c = 0; c < 3; c++) {
                    mn[c] = std::min(mn[c], pos[v * 3 + c]);
                    mx[c] = std::max(mx[c], pos[v * 3 + c]);
                }
                if (sv.hasNormal) {
                    if (nrm.empty()) nrm.resize(n * 3, 0.f);
                    nrm[v * 3] = sv.normal.x;
                    nrm[v * 3 + 1] = sv.normal.y;
                    nrm[v * 3 + 2] = sv.normal.z;
                }
                if (sv.hasTangent) {
                    if (tan.empty()) tan.resize(n * 4, 0.f);
                    tan[v * 4] = sv.tangent.r;
                    tan[v * 4 + 1] = sv.tangent.g;
                    tan[v * 4 + 2] = sv.tangent.b;
                    tan[v * 4 + 3] = sv.tangent.a;
                }
                if (sv.hasColor) {
                    if (col.empty()) col.resize(n * 4, 1.f);
                    col[v * 4] = sv.color.r;
                    col[v * 4 + 1] = sv.color.g;
                    col[v * 4 + 2] = sv.color.b;
                    col[v * 4 + 3] = sv.color.a;
                }
                if (sv.hasJoints) {
                    if (jnts.empty()) jnts.resize(n * 4, 0);
                    for (int c = 0; c < 4; c++) jnts[v * 4 + c] = sv.joints[c];
                }
                if (sv.hasWeights) {
                    if (wts.empty()) wts.resize(n * 4, 0.f);
                    wts[v * 4] = sv.weights.r;
                    wts[v * 4 + 1] = sv.weights.g;
                    wts[v * 4 + 2] = sv.weights.b;
                    wts[v * 4 + 3] = sv.weights.a;
                }
            }
            // UV sets carried from rp.vertices (contract: uvsets has room for
            // targetUv and uvsets[targetUv] already holds the atlas mapping).
            size_t uvSetCount = 0;
            for (const auto& sv : rp.vertices)
                uvSetCount = std::max(uvSetCount, sv.uvsets.size());
            uvSetCount = std::max(uvSetCount, size_t(opt.targetUv) + 1);
            std::vector<std::vector<float>> perSet(uvSetCount);
            for (size_t v = 0; v < n; v++) {
                const SourceVertex& sv = rp.vertices[v];
                for (size_t k = 0; k < uvSetCount; k++) {
                    if (perSet[k].empty()) perSet[k].assign(n * 2, 0.f);
                    V2 val = k < sv.uvsets.size() ? sv.uvsets[k] : V2{0, 0};
                    perSet[k][v * 2] = val.u;
                    perSet[k][v * 2 + 1] = val.v;
                }
            }

            tp.attributes.clear();
            tp.attributes["POSITION"] =
                addBufferViewAccessor(tg, bw, pos, TINYGLTF_COMPONENT_TYPE_FLOAT,
                                      TINYGLTF_TYPE_VEC3, n, 34962, mn, mx);
            if (!nrm.empty())
                tp.attributes["NORMAL"] =
                    addBufferViewAccessor(tg, bw, nrm, TINYGLTF_COMPONENT_TYPE_FLOAT,
                                          TINYGLTF_TYPE_VEC3, n, 34962, nullptr, nullptr);
            if (!tan.empty())
                tp.attributes["TANGENTS"] =
                    addBufferViewAccessor(tg, bw, tan, TINYGLTF_COMPONENT_TYPE_FLOAT,
                                          TINYGLTF_TYPE_VEC4, n, 34962, nullptr, nullptr);
            if (!col.empty())
                tp.attributes["COLOR_0"] =
                    addBufferViewAccessor(tg, bw, col, TINYGLTF_COMPONENT_TYPE_FLOAT,
                                          TINYGLTF_TYPE_VEC4, n, 34962, nullptr, nullptr);
            if (!jnts.empty())
                tp.attributes["JOINTS_0"] =
                    addUintAccessor(tg, bw, jnts, TINYGLTF_TYPE_VEC4, 34962);
            if (!wts.empty())
                tp.attributes["WEIGHTS_0"] =
                    addBufferViewAccessor(tg, bw, wts, TINYGLTF_COMPONENT_TYPE_FLOAT,
                                          TINYGLTF_TYPE_VEC4, n, 34962, nullptr, nullptr);
            for (size_t k = 0; k < uvSetCount; k++)
                tp.attributes["TEXCOORD_" + std::to_string(k)] =
                    addBufferViewAccessor(tg, bw, perSet[k], TINYGLTF_COMPONENT_TYPE_FLOAT,
                                          TINYGLTF_TYPE_VEC2, n, 34962, nullptr, nullptr);
            tp.indices = addIndicesAccessor(tg, bw, rp.indices);
            tp.mode = 4;  // rebuilt from a triangle list (was possibly strip/fan)

            if (cp.material >= 0) {
                auto it = clonedMats.find({int(ai), cp.material});
                if (it != clonedMats.end()) tp.material = it->second;
            }
            // KHR_materials_variants: point mappings at the cloned materials.
            if (!cp.variantMappings.empty()) {
                tinygltf::Value::Array mappings;
                for (const VariantMapping& vm : cp.variantMappings) {
                    int newMat = vm.material;
                    auto it = clonedMats.find({int(ai), vm.material});
                    if (it != clonedMats.end()) newMat = it->second;
                    tinygltf::Value::Object mo;
                    mo["material"] = tinygltf::Value(newMat);
                    tinygltf::Value::Array varArr;
                    for (int v : vm.variants) varArr.push_back(tinygltf::Value(v));
                    mo["variants"] = tinygltf::Value(std::move(varArr));
                    mappings.push_back(tinygltf::Value(std::move(mo)));
                }
                tinygltf::Value::Object ext;
                ext["mappings"] = tinygltf::Value(std::move(mappings));
                tp.extensions["KHR_materials_variants"] = tinygltf::Value(std::move(ext));
            }
        }
    }

    // Make original images portable: embed for glb; copy files for gltf.
    std::map<std::string, std::string> copiedUris;  // old uri -> new relative uri
    for (size_t i = 0; i < tg.images.size(); i++) {
        tinygltf::Image& ti = tg.images[i];
        bool isNew = i >= core.images.size();
        if (isNew) continue;  // baked images already handled
        const Image& src = core.images[i];
        if (opt.binary) {
            if (ti.bufferView < 0 && !src.raw.empty()) {
                bw.appendRaw(src.raw.data(), src.raw.size());
                tinygltf::BufferView bv;
                bv.buffer = 0;
                bv.byteOffset = bw.lastOffset;
                bv.byteLength = bw.lastLength;
                bv.target = 0;
                tg.bufferViews.push_back(bv);
                ti.bufferView = int(tg.bufferViews.size()) - 1;
                ti.mimeType = src.uri.size() >= 4 && src.uri.substr(src.uri.size() - 4) == ".jpg"
                                  ? "image/jpeg"
                                  : "image/png";
                ti.uri.clear();
            }
        } else if (opt.embedTextures) {
            if (ti.uri.rfind("data:", 0) == 0) continue;  // already embedded
            if (!src.raw.empty()) {
                ti.uri = dataUri(guessImageMime(ti.uri, src.raw.data(), src.raw.size()),
                                 src.raw.data(), src.raw.size());
            } else if (ti.bufferView >= 0 && ti.bufferView < int(tg.bufferViews.size())) {
                const tinygltf::BufferView& bv = tg.bufferViews[ti.bufferView];
                const std::vector<unsigned char>& buf = tg.buffers[bv.buffer].data;
                if (bv.byteOffset + bv.byteLength > buf.size()) {
                    report.warn("image[" + std::to_string(i) +
                                "] buffer view out of range; reference kept");
                    continue;
                }
                ti.uri = dataUri(ti.mimeType.empty() ? std::string("image/png") : ti.mimeType,
                                 buf.data() + bv.byteOffset, bv.byteLength);
            } else if (src.decoded) {
                Raster r;
                r.w = src.w;
                r.h = src.h;
                r.rgba = src.rgba;
                std::vector<uint8_t> png;
                if (!encodePng(r, png)) {
                    report.warn("image[" + std::to_string(i) +
                                "] could not be re-encoded; reference kept");
                    continue;
                }
                ti.uri = dataUri("image/png", png.data(), png.size());
            } else {
                report.warn("image[" + std::to_string(i) +
                            "] could not be embedded; reference kept");
                continue;
            }
            ti.bufferView = -1;
            ti.mimeType.clear();
        } else {
            if (!ti.uri.empty() && ti.bufferView < 0) {
                std::string rel = opt.texturesDir + "/" + sanitizeName(fs::path(ti.uri).filename().string());
                if (!copiedUris.count(ti.uri)) {
                    // Keep filenames unique when different source textures share a name.
                    for (std::set<std::string> taken;;) {
                        bool clash = false;
                        for (const auto& kv : copiedUris)
                            if (kv.second == rel) { clash = true; break; }
                        if (!clash) break;
                        fs::path rp(rel);
                        rel = (rp.parent_path() /
                               (rp.stem().string() + "_" + std::to_string(i) +
                                rp.extension().string()))
                                  .string();
                    }
                    if (!src.raw.empty()) {
                        std::ofstream f(texDir / rp_filename(rel), std::ios::binary);
                        if (!f) {
                            err = "failed to copy texture " + rel;
                            return false;
                        }
                        f.write(reinterpret_cast<const char*>(src.raw.data()), src.raw.size());
                        copiedUris[ti.uri] = rel;
                    } else if (src.decoded) {
                        Raster r;
                        r.w = src.w;
                        r.h = src.h;
                        r.rgba = src.rgba;
                        std::vector<uint8_t> png;
                        encodePng(r, png);
                        std::ofstream f(texDir / rp_filename(rel), std::ios::binary);
                        f.write(reinterpret_cast<const char*>(png.data()), png.size());
                        copiedUris[ti.uri] = rel;
                    } else {
                        report.warn("image[" + std::to_string(i) +
                                    "] could not be copied to output; reference kept");
                        continue;
                    }
                }
                ti.uri = copiedUris[ti.uri];
            }
        }
    }

    // Write.
    tinygltf::TinyGLTF writer;
    fs::path modelPath = outDir / opt.modelFilename;
    bool ok = writer.WriteGltfSceneToFile(
        &tg, modelPath.string(), /*embedImages*/ opt.binary,
        /*embedBuffers*/ opt.binary, /*prettyPrint*/ !opt.binary,
        /*writeBinary*/ opt.binary);
    if (!ok && impl_->err.empty()) impl_->err = "failed to write " + modelPath.string();
    return ok;
}

bool GltfDocument::verify(const std::string& writtenPath, const SaveOptions& opt,
                          const std::vector<AtlasOutput>& atlases, Report& report,
                          std::string& err) {
    tinygltf::Model tg;
    tinygltf::TinyGLTF loader;
    loader.SetImageLoader(noopImageLoader, nullptr);
    std::string w, e;
    bool ret = opt.binary ? loader.LoadBinaryFromFile(&tg, &e, &w, writtenPath)
                          : loader.LoadASCIIFromFile(&tg, &e, &w, writtenPath);
    if (!ret) {
        err = "output model failed to re-parse: " + e;
        return false;
    }
    // Check referenced images exist for gltf output.
    if (!opt.binary) {
        fs::path dir = fs::path(writtenPath).parent_path();
        for (const auto& img : tg.images) {
            if (!img.uri.empty() && img.bufferView < 0) {
                std::string uri = img.uri;
                if (uri.rfind("data:", 0) == 0) continue;
                if (!fs::exists(dir / uri)) {
                    err = "output model references missing image: " + uri;
                    return false;
                }
            }
        }
    }
    // Every primitive must have POSITION.
    for (size_t mi = 0; mi < tg.meshes.size(); mi++) {
        for (size_t pi = 0; pi < tg.meshes[mi].primitives.size(); pi++) {
            const auto& tp = tg.meshes[mi].primitives[pi];
            if (getAttr(tp, "POSITION") < 0) {
                err = "output primitive without POSITION";
                return false;
            }
        }
    }
    return true;
}

// helpers for texType naming
const char* texTypeName(TexType t) {
    switch (t) {
        case TexType::BaseColor: return "basecolor";
        case TexType::MetallicRoughness: return "metallic_roughness";
        case TexType::Occlusion: return "occlusion";
        case TexType::Emissive: return "emissive";
        case TexType::Normal: return "normal";
        default: return "other";
    }
}

const char* texTypeGltfSlot(TexType t) {
    switch (t) {
        case TexType::BaseColor: return "baseColorTexture";
        case TexType::MetallicRoughness: return "metallicRoughnessTexture";
        case TexType::Occlusion: return "occlusionTexture";
        case TexType::Emissive: return "emissiveTexture";
        case TexType::Normal: return "normalTexture";
        default: return "unknown";
    }
}

}  // namespace uvremap
