#include <algorithm>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <new>
#include <sstream>
#include <string>
#include <vector>

#include "core/gltf_io.h"
#include "core/preview.h"
#include "core/regenerate.h"
#include "core/report.h"
#include "core/validate.h"

#ifndef UVREMAP_VERSION
#define UVREMAP_VERSION "0.0.0"
#endif

namespace {

using namespace uvremap;

struct Args {
    std::string command;
    std::string input;
    std::vector<std::string> positional;
    std::string output;
    std::string atlasMode, format, filterStr, textures;
    int resolutionW = 0, resolutionH = 0;
    int textureW = 0, textureH = 0;
    int padding = 4;
    float texelsPerUnit = 0.f;
    int sourceUv = 0, targetUv = 0;
    int selfCheck = 0;
    bool dryRun = false, force = false, verbose = false, quiet = false;
    bool preview = false;
    bool json = false;
    bool bakeFactors = false;
    bool splitMaterials = false;
    bool hasUvSet = false;
    bool targetUvExplicit = false;
    std::string error;
};

bool parseSize(const std::string& s, int& w, int& h) {
    auto x = s.find('x');
    if (x == std::string::npos) return false;
    try {
        w = std::stoi(s.substr(0, x));
        h = std::stoi(s.substr(x + 1));
    } catch (...) {
        return false;
    }
    return w > 0 && h > 0;
}

bool parseInt(const std::string& s, int& v) {
    try {
        v = std::stoi(s);
    } catch (...) {
        return false;
    }
    return true;
}

void printUsage(std::ostream& os) {
    os << "uvremap " UVREMAP_VERSION " — UV mapping regeneration for glTF/GLB\n"
       << R"(
usage: uvremap <command> [options] <input>

commands:
  regenerate   generate a new UV mapping and transfer the textures
  scan         analyze the model and its existing UV mapping
  inspect      show primitives, materials, UV sets and images in detail
  preview      write diagnostic images of the source UV layout
  version      print version and build information

regenerate/preview options:
  -o, --output PATH            output directory (default: <input>_remapped)
      --atlas-mode MODE        material | mesh | scene (required for regenerate)
      --resolution WxH         atlas resolution
      --padding PIXELS         UV island margin in atlas texels (default 4)
      --texels-per-unit VALUE  target texel density
      --source-uv INDEX        source TEXCOORD set (default 0)
      --target-uv INDEX        target TEXCOORD set (default 0)
      --texture-filter MODE    nearest | linear (default linear)
      --bake-factors           fold material factors (baseColorFactor,
                               metallicFactor/roughnessFactor,
                               emissiveFactor) into rebuilt textures for
                               slots that have no image, so every atlas
                               island carries its material's appearance; the
                               neutralized factor then lives in the texture
                               only
      --split-materials        emit one baked image per material instead of a
                               single shared image per texture type; forces the
                               pre-0.1 layout (needed for variants anyway)
      --texture-size WxH       resolution of rebuilt textures
      --self-check N           statistically verify baked textures against the
                               source (N random samples per rebuilt image);
                               agreement percentages go to the report
      --format FORMAT          gltf | glb
      --textures MODE          external | embedded (gltf only; default external,
                               glb always embeds)
      --dry-run                validate and print the plan without writing
      --preview                write diagnostic PNGs next to the output: atlas
                               layout, source-vs-atlas comparison, occupancy and
                               texel-density heatmaps
      --json                   write report.json in the output and print the
                               final report as JSON on stdout instead of the
                               human-readable summary
      --force                  allow overwriting existing output files
      --verbose                extra detail
      --quiet                  minimal output
)";
}

// Returns exit code; parses argv into Args.
int parseArgs(int argc, char** argv, Args& a) {
    auto next = [&](int& i, std::string& out) -> bool {
        if (i + 1 >= argc) {
            a.error = std::string(argv[i]) + " requires a value";
            return false;
        }
        out = argv[++i];
        return true;
    };
    if (argc < 2) {
        printUsage(std::cerr);
        return 2;
    }
    a.command = argv[1];
    if (a.command == "-h" || a.command == "--help") {
        printUsage(std::cout);
        return 0;
    }
    for (int i = 2; i < argc; i++) {
        std::string s = argv[i];
        std::string v;
        if (s == "-o" || s == "--output") {
            if (!next(i, v)) return 2;
            a.output = v;
        } else if (s == "--atlas-mode") {
            if (!next(i, v)) return 2;
            a.atlasMode = v;
        } else if (s == "--resolution") {
            if (!next(i, v) || !parseSize(v, a.resolutionW, a.resolutionH)) {
                a.error = "invalid --resolution (expect WIDTHxHEIGHT): " + v;
                return 2;
            }
        } else if (s == "--texture-size") {
            if (!next(i, v) || !parseSize(v, a.textureW, a.textureH)) {
                a.error = "invalid --texture-size (expect WIDTHxHEIGHT): " + v;
                return 2;
            }
        } else if (s == "--padding") {
            if (!next(i, v) || !parseInt(v, a.padding) || a.padding < 1) {
                a.error = "invalid --padding";
                return 2;
            }
        } else if (s == "--self-check") {
            if (!next(i, v) || !parseInt(v, a.selfCheck) || a.selfCheck < 1) {
                a.error = "invalid --self-check (expect sample count >= 1)";
                return 2;
            }
        } else if (s == "--texels-per-unit") {
            if (!next(i, v)) return 2;
            try {
                a.texelsPerUnit = std::stof(v);
            } catch (...) {
                a.error = "invalid --texels-per-unit";
                return 2;
            }
        } else if (s == "--source-uv") {
            if (!next(i, v) || !parseInt(v, a.sourceUv) || a.sourceUv < 0) {
                a.error = "invalid --source-uv";
                return 2;
            }
            a.hasUvSet = true;
        } else if (s == "--target-uv") {
            if (!next(i, v) || !parseInt(v, a.targetUv) || a.targetUv < 0) {
                a.error = "invalid --target-uv";
                return 2;
            }
            a.hasUvSet = true;
            a.targetUvExplicit = true;
        } else if (s == "--texture-filter") {
            if (!next(i, v)) return 2;
            a.filterStr = v;
        } else if (s == "--bake-factors") {
            a.bakeFactors = true;
        } else if (s == "--split-materials") {
            a.splitMaterials = true;
        } else if (s == "--format") {
            if (!next(i, v)) return 2;
            a.format = v;
        } else if (s == "--textures") {
            if (!next(i, v)) return 2;
            a.textures = v;
        } else if (s == "--dry-run") {
            a.dryRun = true;
        } else if (s == "--preview") {
            a.preview = true;
        } else if (s == "--json") {
            a.json = true;
        } else if (s == "--force") {
            a.force = true;
        } else if (s == "--verbose") {
            a.verbose = true;
        } else if (s == "--quiet") {
            a.quiet = true;
        } else if (!s.empty() && s[0] == '-') {
            a.error = "unknown option: " + s;
            return 2;
        } else {
            a.positional.push_back(s);
        }
    }
    if (a.command == "version") return 0;
    if (a.positional.empty()) {
        a.error = "missing input file";
        return 2;
    }
    a.input = a.positional.back();
    return 0;
}

int cmdVersion() {
    std::cout << "uvremap " UVREMAP_VERSION "\n";
    std::cout << "  built:   " << __DATE__ << " " << __TIME__ << "\n";
    std::cout << "  c++:     " << __cplusplus << "\n";
    std::cout << "  xatlas:  vendored (jpcy/xatlas)\n";
    std::cout << "  tinygltf: vendored 2.9.7\n";
    return 0;
}

struct LoadResult {
    Model model;
    GltfDocument doc;
    Report report;
    int code = 0;
    std::string err;
};

LoadResult loadInput(const std::string& input) {
    LoadResult r;
    if (!r.doc.load(input, r.model, r.report, r.err)) r.code = 3;
    return r;
}

std::string indent(int n) { return std::string(n * 2, ' '); }

int cmdScan(LoadResult& L, const Args& a) {
    const Model& m = L.model;
    std::cout << "uvremap " UVREMAP_VERSION "\n\n";
    std::cout << "Input:  " << a.input << "\n";
    std::cout << "Format: " << m.sourceFormat << "\n";
    std::cout << "Scenes: " << m.sceneCount << "  Nodes: " << m.nodeCount
              << "  Animations: " << m.animationCount << "\n\n";
    std::cout << "Meshes/Primitives: " << m.primitives.size() << "\n";
    int mesh = -1;
    for (const auto& p : m.primitives) {
        if (p.meshIndex != mesh) {
            mesh = p.meshIndex;
            std::cout << "  mesh[" << p.meshIndex << "] "
                      << (p.meshName.empty() ? "" : "'" + p.meshName + "'") << "\n";
        }
        size_t uvSets = p.vertices.empty() ? 0 : p.vertices[0].uvsets.size();
        std::cout << indent(2) << "prim[" << p.primIndex << "]: " << p.vertices.size()
                  << " verts, " << p.indices.size() / 3 << " tris, material "
                  << (p.material < 0 ? "(none)" : std::to_string(p.material)) << ", "
                  << uvSets << " UV set(s)"
                  << (p.hasMorphTargets ? ", morph targets" : "")
                  << (p.rawMode == 5 ? ", authored TRIANGLE_STRIP (converted)"
                                      : p.rawMode == 6 ? ", authored TRIANGLE_FAN (converted)"
                                                       : "")
                  << "\n";
    }
    std::cout << "\nMaterials: " << m.materials.size() << "\n";
    for (size_t i = 0; i < m.materials.size(); i++) {
        const Material& mat = m.materials[i];
        std::cout << indent(1) << "material[" << i << "] " << mat.name << ":\n";
        for (int t = 0; t < int(TexType::Count); t++) {
            if (!mat.tex[t].present) continue;
            std::cout << indent(3) << texTypeGltfSlot(TexType(t)) << " -> image "
                      << mat.tex[t].imageIndex << " (TEXCOORD_" << mat.tex[t].texCoord << ")"
                      << (mat.tex[t].hasTransform ? " [KHR_texture_transform]" : "") << "\n";
        }
    }
    std::cout << "\nImages: " << m.images.size() << "\n";
    for (size_t i = 0; i < m.images.size(); i++) {
        const Image& img = m.images[i];
        std::cout << indent(1) << "image[" << i << "] " << img.name << ": "
                  << (img.decoded ? std::to_string(img.w) + "x" + std::to_string(img.h)
                                  : std::string("(not decoded)"))
                  << (!img.uri.empty() ? "  uri=" + img.uri : "") << "\n";
    }
    std::string txt = L.report.renderText("");
    if (!txt.empty() && !a.quiet) std::cout << "\n" << txt;
    return 0;
}

int cmdInspect(LoadResult& L, const Args& a) {
    const Model& m = L.model;
    for (size_t i = 0; i < m.primitives.size(); i++) {
        const Primitive& p = m.primitives[i];
        std::cout << "primitive " << i << " (mesh[" << p.meshIndex << "] prim[" << p.primIndex
                  << "]" << (p.meshName.empty() ? "" : " '" + p.meshName + "'") << ")\n";
        std::cout << indent(1) << "vertices: " << p.vertices.size()
                  << "  triangles: " << p.indices.size() / 3 << "\n";
        bool any = false;
        auto test = [&](bool has, const char* name) {
            if (has) {
                std::cout << indent(1) << "attribute: " << name << "\n";
                any = true;
            }
        };
        if (!p.vertices.empty()) {
            test(true, "POSITION");
            test(p.vertices[0].hasNormal, "NORMAL");
            test(p.vertices[0].hasTangent, "TANGENTS");
            test(p.vertices[0].hasColor, "COLOR_0");
            test(p.vertices[0].hasJoints, "JOINTS_0");
            test(p.vertices[0].hasWeights, "WEIGHTS_0");
            for (size_t k = 0; k < p.vertices[0].uvsets.size(); k++) {
                // UV bounds
                float mnu = 1e30f, mnv = 1e30f, mxu = -1e30f, mxv = -1e30f;
                for (const auto& v : p.vertices) {
                    mnu = std::min(mnu, v.uvsets[k].u);
                    mxu = std::max(mxu, v.uvsets[k].u);
                    mnv = std::min(mnv, v.uvsets[k].v);
                    mxv = std::max(mxv, v.uvsets[k].v);
                }
                std::cout << indent(1) << "attribute: TEXCOORD_" << k << "  range u["
                          << mnu << "," << mxu << "] v[" << mnv << "," << mxv << "]\n";
            }
        }
        (void)any;
        if (p.material >= 0) {
            const Material& mat = m.materials[p.material];
            std::cout << indent(1) << "material[" << p.material << "] " << mat.name
                      << "  alphaMode=" << mat.alphaMode;
            if (mat.unlit) std::cout << "  unlit";
            if (mat.doubleSided) std::cout << "  doubleSided";
            std::cout << "\n";
            for (int t = 0; t < int(TexType::Count); t++)
                if (mat.tex[t].present) {
                    const Image& img = mat.tex[t].imageIndex >= 0 &&
                                               mat.tex[t].imageIndex < int(m.images.size())
                                           ? m.images[mat.tex[t].imageIndex]
                                           : Image();
                    std::cout << indent(2) << texTypeGltfSlot(TexType(t)) << ": image "
                              << mat.tex[t].imageIndex
                              << (img.decoded ? " " + std::to_string(img.w) + "x" +
                                                    std::to_string(img.h)
                                              : " (undecoded)")
                              << " via TEXCOORD_" << mat.tex[t].texCoord << "\n";
                }
        }
        std::string why = eligibilityReason(m, int(i), a.sourceUv);
        if (!why.empty())
            std::cout << indent(1) << "[not regenerable: " << why << "]\n";
    }
    return 0;
}

int cmdPreview(LoadResult& L, const Args& a, bool afterRegen = false) {
    std::string outDir = a.output;
    if (outDir.empty()) outDir = a.input + "_preview";
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec) {
        std::cerr << "failed to create output directory: " << ec.message() << "\n";
        return 6;
    }
    Model& m = L.model;
    size_t uvSets = m.primitives.empty() ? 0 : m.primitives[0].vertices.size()
                                        ? m.primitives[0].vertices[0].uvsets.size()
                                        : 0;
    for (const auto& p : m.primitives)
        if (!p.vertices.empty())
            uvSets = std::max(uvSets, p.vertices[0].uvsets.size());
    int wrote = 0;
    std::string err;
    for (size_t k = 0; k < uvSets; k++) {
        std::string path = outDir + "/uv_layout_" + std::to_string(k) + ".png";
        if (renderUvLayout(m, int(k), path, L.report, err)) {
            if (!a.quiet) std::cout << "wrote " << path << "\n";
            wrote++;
        } else {
            std::cerr << "preview failed: " << err << "\n";
            return 6;
        }
    }
    if (wrote == 0) {
        std::cerr << "no UV sets found to preview\n";
        return 4;
    }
    return 0;
}

int cmdRegenerate(const Args& a) {
    RegenOptions o;
    o.output = a.output;
    if (o.output.empty()) o.output = a.input + "_remapped";
    if (a.atlasMode.empty()) {
        std::cerr << "regenerate requires --atlas-mode material|mesh|scene\n";
        return 2;
    }
    AtlasMode mode;
    if (!parseAtlasMode(a.atlasMode, mode)) {
        std::cerr << "invalid --atlas-mode: " << a.atlasMode << "\n";
        return 2;
    }
    o.mode = mode;
    o.resolutionW = a.resolutionW;
    o.resolutionH = a.resolutionH;
    o.padding = a.padding;
    o.texelsPerUnit = a.texelsPerUnit;
    o.sourceUv = a.sourceUv;
    o.targetUv = a.targetUv;
    o.targetUvExplicit = a.targetUvExplicit;
    o.selfCheck = a.selfCheck;
    o.textureW = a.textureW;
    o.textureH = a.textureH;
    if (!a.filterStr.empty() && a.filterStr != "nearest" && a.filterStr != "linear") {
        std::cerr << "invalid --texture-filter: " << a.filterStr << "\n";
        return 2;
    }
    o.filter = a.filterStr == "nearest" ? Filter::Nearest : Filter::Linear;
    o.bakeFactors = a.bakeFactors;
    o.splitMaterials = a.splitMaterials;
    if (!a.format.empty()) {
        if (a.format == "glb") o.binary = true;
        else if (a.format == "gltf") o.binary = false;
        else {
            std::cerr << "invalid --format: " << a.format << "\n";
            return 2;
        }
        o.formatSet = true;
    }
    if (!a.textures.empty() && a.textures != "external" && a.textures != "embedded") {
        std::cerr << "invalid --textures: " << a.textures << " (external | embedded)\n";
        return 2;
    }
    if (o.binary && a.textures == "external") {
        std::cerr << "--textures external is not valid with glb output (glb embeds "
                     "textures); use --format gltf\n";
        return 2;
    }
    if (a.textures == "embedded") o.embedTextures = true;
    o.dryRun = a.dryRun;
    o.writePreviews = a.preview;
    o.json = a.json;
    o.force = a.force;
    o.verbose = a.verbose;
    o.quiet = a.quiet;

    if (!a.quiet && !a.json)
        std::cout << "uvremap " UVREMAP_VERSION << "\n\nInput:       " << a.input
                  << "\nOutput:      " << o.output << "\nAtlas mode:  " << a.atlasMode
                  << "\n\n";

    auto step = [&](int n, const std::string& name, bool ok) {
        if (a.quiet) return;
        std::ostringstream os;
        os << "[" << n << "/5] " << name;
        std::string s = os.str();
        std::cout << s << std::string(std::max(0, 32 - int(s.size())), '.') << (ok ? " OK\n"
                                                                                    : " FAIL\n");
        std::cout.flush();
    };

    o.progress = [&](int n, const std::string& name, bool ok) {
        if (a.quiet || a.dryRun || a.json) return;
        step(n, name, ok);
    };
    RegenResult r = runRegenerate(a.input, o);
    if (!a.json && !r.message.empty()) std::cout << r.message;
    if (r.code != ExitCode::Success) {
        std::cerr << "uvremap: " << (r.message.empty() ? r.report.renderText("") : "") << "\n";
        return int(r.code);
    }
    if (a.dryRun) return 0;
    if (a.json) {
        ReportMeta meta;
        meta.tool = "uvremap";
        meta.version = UVREMAP_VERSION;
        meta.input = a.input;
        meta.output = o.output;
        meta.atlasMode = a.atlasMode;
        std::cout << r.report.renderJson(meta);
        return 0;
    }
    const Stats& s = r.report.stats;
    if (!a.quiet) {
        std::cout << "\nSummary:\n"
                  << "  Primitives processed: " << s.primitivesProcessed << "\n"
                  << "  Primitives unchanged: " << s.primitivesSkipped << "\n"
                  << "  Atlas count:          " << s.atlasCount << "\n"
                  << "  Atlas resolution:     " << s.atlasWidth << "x" << s.atlasHeight << "\n"
                  << "  Texture maps baked:   " << s.textureMapsBaked << "\n"
                  << "  Warnings:             " << r.report.warningCount() << "\n";
    }
    std::string txt = r.report.renderText("");
    if (!txt.empty() && (a.verbose || !a.quiet)) std::cout << "\n" << txt;
    if (!a.quiet) std::cout << "\nCompleted successfully.\n";
    return 0;
}

int run(int argc, char** argv) {
    Args a;
    int rc = parseArgs(argc, argv, a);
    if (rc != 0) {
        if (!a.error.empty()) std::cerr << "uvremap: " << a.error << "\n";
        return rc;
    }
    if (a.command == "version") return cmdVersion();
    if (a.command == "regenerate") return cmdRegenerate(a);

    LoadResult L = loadInput(a.input);
    if (L.code != 0) {
        std::cerr << "uvremap: failed to load " << a.input << ": " << L.err << "\n";
        return 3;
    }
    if (a.command == "scan") return cmdScan(L, a);
    if (a.command == "inspect") return cmdInspect(L, a);
    if (a.command == "preview") return cmdPreview(L, a);

    std::cerr << "uvremap: unknown command: " << a.command << "\n";
    printUsage(std::cerr);
    return 2;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::bad_alloc&) {
        std::cerr << "uvremap: out of memory. Try a smaller --resolution (e.g. 1024x1024) "
                     "or free up RAM.\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "uvremap: internal error: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "uvremap: internal error (unknown exception)\n";
        return 1;
    }
}
