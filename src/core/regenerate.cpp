#include "core/regenerate.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "core/gltf_io.h"
#include "core/preview.h"
#include "core/texture_baker.h"
#include "core/validate.h"
#include "core/xatlas_runner.h"

namespace fs = std::filesystem;

namespace uvremap {

namespace {

// Recursively moves src into dst, replacing existing files (same filesystem).
bool publishDir(const fs::path& src, const fs::path& dst, std::string& err) {
    std::error_code ec;
    if (!fs::exists(dst)) {
        fs::rename(src, dst, ec);
        if (!ec) return true;
        err = "failed to publish output: " + ec.message();
        return false;
    }
    fs::create_directories(dst, ec);
    for (auto& e : fs::directory_iterator(src)) {
        fs::path target = dst / e.path().filename();
        if (e.is_directory()) {
            if (!publishDir(e.path(), target, err)) return false;
            fs::remove_all(e.path());
        } else {
            fs::rename(e.path(), target, ec);
            if (ec) {
                fs::copy_file(e.path(), target, fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    err = "failed to write " + target.string() + ": " + ec.message();
                    return false;
                }
                fs::remove(e.path());
            }
        }
    }
    return true;
}

std::string renderDryRunPlan(const Model& model, const std::vector<AtlasGroup>& groups,
                              const RegenOptions& opt) {
    std::ostringstream os;
    os << "Dry run — planned operations for '" << opt.output << "':\n";
    os << "  atlas mode: " << atlasModeName(opt.mode) << "\n";
    os << "  format: " << (opt.binary ? "glb" : "gltf") << "\n";
    if (!opt.binary)
        os << "  textures: " << (opt.embedTextures ? "embedded (base64)" : "external files")
           << "\n";
    os << "  source UV: " << opt.sourceUv << "  target UV: " << opt.targetUv << "\n";
    os << "  filter: " << (opt.filter == Filter::Nearest ? "nearest" : "linear")
       << "  padding: " << opt.padding << " texels\n";
    if (opt.bakeFactors)
        os << "  factor bake: material factors fold into texture-less slots\n";
    os << "  atlases (" << groups.size() << "):\n";
    for (const auto& g : groups) {
        os << "    '" << g.name << "': " << g.primitives.size() << " primitive(s), "
           << g.materials.size() << " material(s)\n";
        for (int mat : g.materials) {
            if (mat < 0) continue;
            for (int t = 0; t < kTexCount; t++)
                if (textureBakeable(model, mat, TexType(t)))
                    os << "      - bake " << texTypeName(TexType(t)) << " from material "
                       << mat << " (" << model.materials[mat].name << ")\n";
        }
    }
    return os.str();
}

}  // namespace

RegenResult runRegenerate(const std::string& input, const RegenOptions& optIn) {
    RegenOptions opt = optIn;
    RegenResult res;
    Report& rep = res.report;
    rep.stats.primitivesTotal = 0;
    auto progress = [&](int n, const std::string& label, bool ok) {
        if (opt.progress) opt.progress(n, label, ok);
    };

    // [1/5] Load.
    Model model;
    GltfDocument doc;
    std::string err;
    if (!doc.load(input, model, rep, err)) {
        progress(1, "Loading model", false);
        res.code = ExitCode::LoadFailure;
        res.message = err;
        return res;
    }
    progress(1, "Loading model", true);
    rep.stats.primitivesTotal = int(model.primitives.size());

    // Infer output format from input if not given.
    RegenOptions o = opt;
    if (!o.formatSet) {
        auto dot = input.rfind('.');
        std::string ext = dot == std::string::npos ? "" : input.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        o.binary = (ext != "gltf");
    }

    // [2/5] Validate + analyze.
    if (!validateForRegenerate(model, o.sourceUv, rep)) {
        progress(2, "Analyzing geometry", false);
        res.code = ExitCode::InsufficientData;
        res.message = rep.errors().empty() ? "insufficient geometry or UVs" : rep.errors()[0];
        return res;
    }
    progress(2, "Analyzing geometry", true);
    if (o.sourceUv == o.targetUv) {
        rep.info("target UV equals source UV (" + std::to_string(o.sourceUv) +
                 "): the original mapping is replaced by the new atlas mapping in the "
                 "output; textures were already sampled from the in-memory copy");
    }

    auto groups = buildGroups(model, o.mode, o.sourceUv, rep);

    if (o.dryRun) {
        res.message = renderDryRunPlan(model, groups, o);
        return res;
    }

    // Output safety: refuse to overwrite without --force.
    fs::path outDir(o.output);
    std::error_code ec;
    fs::path modelPath = outDir /
        (fs::path(input).stem().string() + (o.binary ? ".glb" : ".gltf"));
    if (fs::exists(modelPath) && !o.force) {
        res.code = ExitCode::WriteFailure;
        res.message = "output model already exists (use --force to overwrite): " +
                      modelPath.string();
        return res;
    }

    fs::path staging = outDir;
    staging += ".uvremap-staging";
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    if (ec) {
        res.code = ExitCode::WriteFailure;
        res.message = "cannot create output area: " + ec.message();
        return res;
    }

    // [3/5] Generate atlases.
    std::vector<AtlasGen> gens(groups.size());
    GenOptions go;
    // xatlas auto-estimation produces tiny atlases; default to 2048 unless asked.
    go.resolutionW = o.resolutionW > 0 ? o.resolutionW : 2048;
    go.resolutionH = o.resolutionH > 0 ? o.resolutionH : 2048;
    go.padding = o.padding;
    go.texelsPerUnit = o.texelsPerUnit;
    for (size_t gi = 0; gi < groups.size(); gi++) {
        if (!generateAtlas(model, groups[gi], o.sourceUv, go, gens[gi], rep, err)) {
            progress(3, "Generating UV atlas", false);
            res.code = ExitCode::ProcessingFailure;
            res.message = "atlas generation failed: " + err;
            fs::remove_all(staging, ec);
            return res;
        }
        rep.stats.atlasCount++;
        rep.stats.atlasWidth = gens[gi].width;
        rep.stats.atlasHeight = gens[gi].height * std::max(1, gens[gi].pageCount);
    }
    progress(3, "Generating UV atlas", true);

    // [4/5] Bake textures + rebuild primitives. Baking is the heavy
    // single-threaded stage; run groups concurrently (xatlas generation is
    // already internally parallel, so it stays sequential).
    std::vector<AtlasOutput> atlases(groups.size());
    BakeOptions bo;
    bo.filter = o.filter;
    bo.bakeFactors = o.bakeFactors;
    bo.textureW = o.textureW;
    bo.textureH = o.textureH;
    bo.sourceUv = o.sourceUv;
    std::vector<Report> bakeReports(groups.size());
    std::vector<std::string> bakeErrs(groups.size());
    std::vector<char> bakeOk(groups.size(), 0);
    auto bakeOne = [&](size_t gi) {
        AtlasOutput& ao = atlases[gi];
        ao.name = groups[gi].name;
        const int pages = std::max(1, gens[gi].pageCount);
        const int texW = o.textureW > 0 ? o.textureW : gens[gi].width;
        const int texH = o.textureH > 0 ? o.textureH : gens[gi].height;
        ao.width = texW;
        ao.height = texH * pages;  // pages stack vertically into one texture
        ao.materialIds = groups[gi].materials;

        std::vector<BakedImage> baked;
        if (!bakeGroup(model, groups[gi], gens[gi], bo, baked, bakeReports[gi], bakeErrs[gi]))
            return;
        ao.baked = std::move(baked);

        // Reconstruct vertex data through xref; new UV goes into targetUv.
        for (const GenMesh& gm : gens[gi].meshes) {
            const Primitive& prim = model.primitives[gm.primId];
            RemeshedPrimitive rp;
            rp.primId = gm.primId;
            rp.vertices.resize(gm.vertices.size());
            for (size_t v = 0; v < gm.vertices.size(); v++) {
                const GenVertex& gv = gm.vertices[v];
                const SourceVertex& src = prim.vertices[std::min<size_t>(gv.xref, prim.vertices.size() - 1)];
                SourceVertex& dst = rp.vertices[v];
                dst = src;  // carries position/normal/tangent/color/joints/uvsets
                size_t need = size_t(o.targetUv) + 1;
                if (dst.uvsets.size() < need) dst.uvsets.resize(need, V2{0, 0});
                dst.uvsets[size_t(o.targetUv)] =
                    V2{gv.atlasIndex < 0 ? 0.f : gv.u / float(gens[gi].width),
                       gv.atlasIndex < 0
                           ? 0.f
                           : (float(gv.atlasIndex) + gv.v / float(gens[gi].height)) / float(pages)};
            }
            rp.indices = gm.indices;
            ao.primitives.push_back(std::move(rp));
        }
        bakeOk[gi] = 1;
    };
    const size_t nGroups = groups.size();
    unsigned hw = std::thread::hardware_concurrency();
    size_t nThreads = std::min<size_t>(nGroups, hw > 1 ? hw / 2 : 1);
    if (nThreads > 1 && nGroups > 1) {
        std::atomic<size_t> next{0};
        std::vector<std::thread> pool;
        pool.reserve(nThreads);
        for (size_t t = 0; t < nThreads; t++)
            pool.emplace_back([&] {
                for (size_t gi = next.fetch_add(1); gi < nGroups; gi = next.fetch_add(1))
                    bakeOne(gi);
            });
        for (auto& th : pool) th.join();
    } else {
        for (size_t gi = 0; gi < nGroups; gi++) bakeOne(gi);
    }
    // Deterministic merge: reports and stats in group order, first failure wins.
    for (size_t gi = 0; gi < nGroups; gi++) {
        rep.absorb(bakeReports[gi]);
        if (!bakeOk[gi]) {
            progress(4, "Baking textures", false);
            res.code = ExitCode::ProcessingFailure;
            res.message = "texture transfer failed: " + bakeErrs[gi];
            fs::remove_all(staging, ec);
            return res;
        }
        rep.stats.primitivesProcessed += int(atlases[gi].primitives.size());
    }
    rep.stats.primitivesSkipped =
        rep.stats.primitivesTotal - rep.stats.primitivesProcessed;

    progress(4, "Baking textures", true);

    // [5/5] Export.
    SaveOptions so;
    so.outputDir = staging.string();
    so.modelFilename = modelPath.filename().string();
    so.binary = o.binary;
    so.embedTextures = o.embedTextures && !o.binary;
    so.targetUv = o.targetUv;
    if (!doc.save(model, atlases, so, rep, err)) {
        progress(5, "Exporting assets", false);
        res.code = ExitCode::WriteFailure;
        res.message = "failed to export: " + err;
        fs::remove_all(staging, ec);
        return res;
    }

    // Verify before publishing.
    fs::path stagedModel = staging / so.modelFilename;
    if (!GltfDocument::verify(stagedModel.string(), so, atlases, rep, err)) {
        progress(5, "Exporting assets", false);
        res.code = ExitCode::WriteFailure;
        res.message = "output validation failed: " + err;
        fs::remove_all(staging, ec);
        return res;
    }

    if (o.writePreviews) {
        for (size_t gi = 0; gi < groups.size(); gi++) {
            renderAtlasLayout(model, gens[gi],
                              (staging / (groups[gi].name + "_atlas_preview.png")).string(),
                              rep, err);
        }
    }

    // report.txt
    {
        std::ofstream rf(staging / "report.txt");
        rf << "uvremap " UVREMAP_VERSION "\n\n";
        rf << "Input:  " << input << "\n";
        rf << "Output: " << o.output << "\n";
        rf << "Atlas mode: " << atlasModeName(o.mode) << "\n\n";
        rf << "Summary:\n";
        rf << "  Primitives processed: " << rep.stats.primitivesProcessed << "\n";
        rf << "  Primitives left unchanged: " << rep.stats.primitivesSkipped << "\n";
        rf << "  Atlas count: " << rep.stats.atlasCount << "\n";
        rf << "  Atlas resolution: " << rep.stats.atlasWidth << "x" << rep.stats.atlasHeight
           << "\n";
        rf << "  Texture maps baked: " << rep.stats.textureMapsBaked << "\n";
        rf << "  Warnings: " << rep.warningCount() << "\n";
        rf << "\n" << rep.renderText("");
    }

    if (!o.force && fs::exists(outDir) && !fs::is_empty(outDir)) {
        std::string e2;
        // outDir exists and has content but modelPath didn't exist: still fine to
        // merge; publishDir handles it.
        (void)e2;
    }
    if (!publishDir(staging, outDir, err)) {
        res.code = ExitCode::WriteFailure;
        res.message = err;
        fs::remove_all(staging, ec);
        return res;
    }
    progress(5, "Exporting assets", true);
    fs::remove_all(staging, ec);
    return res;
}

}  // namespace uvremap
