// Minimal self-contained unit checks for uvremap core.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

#include "core/gltf_io.h"
#include "core/grouping.h"
#include "core/image.h"
#include "core/normal_math.h"
#include "core/preview.h"
#include "core/texture_baker.h"
#include "core/xatlas_runner.h"

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            failures++;                                                      \
        }                                                                    \
    } while (0)

using namespace uvremap;

// Baked images are stored PNG-encoded (b.image.rgba empty, b.image.raw set);
// decode back to RGBA for pixel assertions in tests.
static const std::vector<uint8_t>& bakedRgba(const BakedImage& b, Raster& scratch) {
    if (!b.image.rgba.empty()) return b.image.rgba;
    static std::string derr;
    decodeImage(b.image.raw, scratch, derr);
    return scratch.rgba;
}

static void testNormalIdentity() {
    // Same parametrization in and out => rebake must be (almost) identity.
    V3 p[3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    V2 uv[3] = {{0, 0}, {1, 0}, {0, 1}};
    TriFrame f;
    CHECK(triangleFrame(p, uv, f));
    V3 n{0.3f, -0.4f, 0.866f};
    V3 out = rebakeNormal(f, f, n);
    float dotv = dot(n, out);
    CHECK(std::fabs(dotv - 1.f) < 1e-4f);
}

static void testNormalFlatRotation() {
    // Uniform scaling of source UVs must not change the world normal.
    V3 p[3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    V2 uvA[3] = {{0, 0}, {1, 0}, {0, 1}};
    V2 uvB[3] = {{0, 0}, {2, 0}, {0, 2}};
    TriFrame fa, fb;
    CHECK(triangleFrame(p, uvA, fa));
    CHECK(triangleFrame(p, uvB, fb));
    V3 n{0, 0, 1};  // flat normal stays flat
    V3 out = rebakeNormal(fa, fb, n);
    CHECK(std::fabs(out.z - 1.f) < 1e-4f);
}

static void testSampling() {
    Raster r;
    r.resize(2, 2);
    r.fill(10, 20, 30, 255);
    float out[4];
    samplePoint(r, 0.25f, 0.25f, Filter::Nearest, false, out);
    CHECK(std::fabs(out[0] * 255.f - 10.f) < 1.f);
    CHECK(std::fabs(out[3] - 1.f) < 1e-6f);
    // uv=1.0 with repeat wraps to same image
    samplePoint(r, 1.0f, 1.0f, Filter::Nearest, true, out);
    CHECK(std::fabs(out[2] * 255.f - 30.f) < 1.f);
}

static void testNeutral() {
    uint8_t n[4];
    neutralTexel(TexType::Normal, n);
    CHECK(n[0] == 128 && n[1] == 128 && n[2] == 255);
    neutralTexel(TexType::MetallicRoughness, n);
    CHECK(n[1] == 255 && n[2] == 0);  // roughness G=1, metal B=0
}

static void testPngRoundTrip() {
    Raster r;
    r.resize(4, 3);
    for (int i = 0; i < 4 * 3 * 4; i++) r.rgba[i] = uint8_t(i % 251);
    std::vector<uint8_t> png;
    CHECK(encodePng(r, png));
    Raster back;
    std::string err;
    CHECK(decodeImage(png, back, err));
    CHECK(back.w == 4 && back.h == 3);
    CHECK(back.rgba == r.rgba);
}

static void testTriangulate() {
    // TRIANGLE_STRIP: even (a,b,c), odd (a,c,b) to keep winding consistent.
    std::vector<uint32_t> strip = {0, 1, 2, 3};
    std::vector<uint32_t> wantStrip = {0, 1, 2, 1, 3, 2};
    CHECK(triangulateIndices(5, strip) == wantStrip);
    // TRIANGLE_FAN: every triangle shares the apex.
    std::vector<uint32_t> wantFan = {0, 1, 2, 0, 2, 3};
    CHECK(triangulateIndices(6, strip) == wantFan);
    // LIST passes through untouched.
    CHECK(triangulateIndices(4, strip) == strip);
    // Trailing degenerate pair is dropped by the 3-multiple resize at load;
    // expansion itself must stay well-formed on short input.
    std::vector<uint32_t> shortStrip = {0, 1};
    CHECK(triangulateIndices(5, shortStrip).size() < 3);
}

static void testColocalTwinRegression() {
    // doghouse.glb case: xatlas welds colocal duplicated vertices and may
    // xref a corner's bit-identical twin instead of the source triangle's
    // own index. Corner matching must still map every triangle.
    Model model;
    model.inputPath = "synthetic";

    Image img;
    img.name = "red";
    img.w = 4;
    img.h = 4;
    img.decoded = true;
    img.rgba.assign(4 * 4 * 4, 0);
    for (int i = 0; i < 4 * 4; i++) {
        img.rgba[i * 4 + 0] = 255;
        img.rgba[i * 4 + 3] = 255;
    }
    model.images.push_back(std::move(img));

    Material m;
    m.name = "m0";
    m.tex[int(TexType::BaseColor)].present = true;
    m.tex[int(TexType::BaseColor)].imageIndex = 0;
    m.tex[int(TexType::BaseColor)].texCoord = 0;
    model.materials.push_back(m);

    Primitive p;
    p.meshIndex = 0;
    p.primIndex = 0;
    p.material = 0;
    const V3 pos[4] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    const V2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int copy = 0; copy < 2; copy++)
        for (int c = 0; c < 4; c++) {
            SourceVertex v;
            v.position = pos[c];
            v.hasNormal = true;
            v.normal = {0, 0, 1};
            v.uvsets.push_back(uv[c]);
            p.vertices.push_back(v);
        }
    // Triangle A uses the first copies, triangle B the colocal twins.
    p.indices = {0, 1, 2, 6, 7, 4};
    model.primitives.push_back(std::move(p));

    AtlasGroup group;
    group.id = 0;
    group.name = "t";
    group.primitives = {0};
    group.materials = {0};

    Report rep;
    std::string err;
    GenOptions go;
    go.resolutionW = go.resolutionH = 64;
    go.padding = 1;
    AtlasGen gen;
    CHECK(generateAtlas(model, group, 0, go, gen, rep, err));
    if (gen.unmappedTriangles != 0) {
        std::printf("  unmappedTriangles=%d err=%s\n", gen.unmappedTriangles, err.c_str());
    }
    CHECK(gen.unmappedTriangles == 0);
    for (const auto& w : rep.warnings())
        CHECK(w.find("could not be mapped") == std::string::npos);

    BakeOptions bo;
    bo.filter = Filter::Nearest;
    std::vector<BakedImage> baked;
    CHECK(bakeGroup(model, group, gen, bo, baked, rep, err));
    bool sawBaseColor = false;
    for (const BakedImage& b : baked) {
        if (b.type != TexType::BaseColor) continue;
        sawBaseColor = true;
        Raster sc;
        const std::vector<uint8_t>& imgOut = bakedRgba(b, sc);
        int red = 0;
        for (size_t i = 0; i + 3 < imgOut.size(); i += 4)
            if (imgOut[i] > 200 && imgOut[i + 1] < 60 && imgOut[i + 2] < 60)
                red++;
        CHECK(red > 0);
    }
    CHECK(sawBaseColor);
}

static Model makeQuadModel() {
    Model model;
    model.inputPath = "synthetic";
    Primitive p;
    p.meshIndex = 0;
    p.primIndex = 0;
    p.material = 0;
    const V3 pos[4] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    const V2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int c = 0; c < 4; c++) {
        SourceVertex v;
        v.position = pos[c];
        v.hasNormal = true;
        v.normal = {0, 0, 1};
        v.uvsets.push_back(uv[c]);
        p.vertices.push_back(v);
    }
    p.indices = {0, 1, 2, 0, 2, 3};
    model.primitives.push_back(std::move(p));
    return model;
}

static void testTextureTransformRotation() {
    // KHR_texture_transform spec example: offset [0,1], scale [.5,.5],
    // rotation pi/2. Mesh UV [0,1]^2 maps to texture region u'=[.5,1],
    // v'=[0,.5] (upper-right storage quadrant), so the baked atlas must be
    // that quadrant's color only.
    Model model = makeQuadModel();

    Image img;
    img.name = "quadrants";
    img.w = 4;
    img.h = 4;
    img.decoded = true;
    img.rgba.assign(4 * 4 * 4, 0);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            uint8_t* p = &img.rgba[(y * 4 + x) * 4];
            bool right = x >= 2, top = y < 2;
            if (top && !right) { p[0] = 255; }            // TL red
            else if (top && right) { p[1] = 255; }        // TR green
            else if (!right) { p[2] = 255; }              // BL blue
            else { p[0] = p[1] = p[2] = 255; }            // BR white
            p[3] = 255;
        }
    Material m;
    m.name = "m0";
    m.tex[int(TexType::BaseColor)].present = true;
    m.tex[int(TexType::BaseColor)].imageIndex = 0;
    m.tex[int(TexType::BaseColor)].texCoord = 0;
    m.tex[int(TexType::BaseColor)].hasTransform = true;
    TexTransform& x = m.tex[int(TexType::BaseColor)].transform;
    x.identity = false;
    model.images.push_back(std::move(img));
    model.materials.push_back(m);

    AtlasGroup group;
    group.id = 0;
    group.name = "t";
    group.primitives = {0};
    group.materials = {0};
    GenOptions go;
    go.resolutionW = go.resolutionH = 64;
    go.padding = 1;

    auto bake = [&](bool withTransform, std::vector<BakedImage>& out) {
        TexTransform& xform = model.materials[0].tex[int(TexType::BaseColor)].transform;
        model.materials[0].tex[int(TexType::BaseColor)].hasTransform = withTransform;
        xform.offsetU = 0.0;
        xform.offsetV = withTransform ? 1.0 : 0.0;
        xform.scaleU = xform.scaleV = withTransform ? 0.5 : 1.0;
        xform.rotation = withTransform ? 1.5707963267948966 : 0.0;
        xform.identity = !withTransform;
        Report rep;
        std::string err;
        AtlasGen gen;
        CHECK(generateAtlas(model, group, 0, go, gen, rep, err));
        BakeOptions bo;
        bo.filter = Filter::Nearest;
        CHECK(bakeGroup(model, group, gen, bo, out, rep, err));
        for (const auto& w : rep.warnings()) {
            CHECK(w.find("could not be mapped") == std::string::npos);
            CHECK(w.find("rotation") == std::string::npos);
        }
    };

    std::vector<BakedImage> plain, rotated;
    bake(false, plain);
    bake(true, rotated);

    auto colorStats = [](const std::vector<BakedImage>& baked, int counts[4]) {
        counts[0] = counts[1] = counts[2] = counts[3] = 0;  // red green blue white
        int total = 0;
        for (const BakedImage& b : baked) {
            if (b.type != TexType::BaseColor) continue;
            Raster sc;
            const std::vector<uint8_t>& rgba = bakedRgba(b, sc);
            for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
                if (rgba[i + 3] == 0) continue;  // neutral fill
                int r = rgba[i], g = rgba[i + 1], b2 = rgba[i + 2];
                if (r > 180 && g < 80 && b2 < 80) counts[0]++;
                else if (g > 180 && r < 80 && b2 < 80) counts[1]++;
                else if (b2 > 180 && r < 80 && g < 80) counts[2]++;
                else if (r > 180 && g > 180 && b2 > 180) counts[3]++;
                total++;
            }
        }
        return total;
    };

    int plainCounts[4], rotCounts[4];
    int plainTotal = colorStats(plain, plainCounts);
    int rotTotal = colorStats(rotated, rotCounts);
    CHECK(plainTotal > 0);
    // Without transform all four quadrants are sampled.
    CHECK(plainCounts[0] > 0 && plainCounts[1] > 0 && plainCounts[2] > 0 && plainCounts[3] > 0);
    // With the spec transform only the TR (green) quadrant is sampled.
    CHECK(rotTotal > 0);
    CHECK(rotCounts[1] * 10 >= rotTotal * 9);
}

static void testMaterialsVariantsBake() {
    // One quad prim, two variant materials (red / green) over the same region:
    // the baker must emit one dedicated BaseColor copy per material.
    Model model = makeQuadModel();
    model.variantNames = {"A", "B"};
    for (int mat = 0; mat < 2; mat++) {
        Image img;
        img.name = mat == 0 ? "red" : "green";
        img.w = img.h = 4;
        img.decoded = true;
        img.rgba.assign(4 * 4 * 4, 0);
        for (int i = 0; i < 4 * 4; i++) {
            img.rgba[i * 4 + mat] = 255;
            img.rgba[i * 4 + 3] = 255;
        }
        model.images.push_back(std::move(img));
        Material m;
        m.name = "m" + std::to_string(mat);
        m.tex[int(TexType::BaseColor)].present = true;
        m.tex[int(TexType::BaseColor)].imageIndex = int(model.images.size()) - 1;
        m.tex[int(TexType::BaseColor)].texCoord = 0;
        model.materials.push_back(m);
    }
    Primitive& p = model.primitives[0];
    p.material = 0;
    p.variantMappings = {{0, {0}}, {1, {1}}};

    AtlasGroup group;
    group.id = 0;
    group.name = "t";
    group.primitives = {0};
    group.materials = {0, 1};

    Report rep;
    std::string err;
    GenOptions go;
    go.resolutionW = go.resolutionH = 64;
    go.padding = 1;
    AtlasGen gen;
    CHECK(generateAtlas(model, group, 0, go, gen, rep, err));
    CHECK(gen.unmappedTriangles == 0);

    BakeOptions bo;
    bo.filter = Filter::Nearest;
    std::vector<BakedImage> baked;
    CHECK(bakeGroup(model, group, gen, bo, baked, rep, err));

    int copies[2] = {0, 0};
    for (const BakedImage& b : baked) {
        if (b.type != TexType::BaseColor) continue;
        if (b.material < 0 || b.material > 1) continue;
        copies[b.material]++;
        int wanted = b.material;  // green channel for mat 1, red for mat 0
        int hits = 0, wrong = 0;
        Raster sc;
        const std::vector<uint8_t>& rgba = bakedRgba(b, sc);
        for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
            if (rgba[i + 3] == 0) continue;
            if (rgba[i + wanted] > 200 && rgba[i + (1 - wanted)] < 60)
                hits++;
            else
                wrong++;
        }
        CHECK(hits > 0);
        CHECK(wrong == 0);
    }
    CHECK(copies[0] == 1);
    CHECK(copies[1] == 1);
}

static void testBakeFactors() {
    // Two texture-less materials (red / green + occlusion strength): without
    // --bake-factors nothing is emitted; with it, each material gets its own
    // painted copy flagged via factorBaked bits.
    Model model = makeQuadModel();
    model.materials.clear();
    for (int mat = 0; mat < 2; mat++) {
        Material m;
        m.name = "m" + std::to_string(mat);
        if (mat == 0) {
            m.baseColorFactor = {0.8f, 0.05f, 0.05f, 1.f};
        } else {
            m.baseColorFactor = {0.05f, 0.8f, 0.05f, 1.f};
            m.occlusionStrength = 0.5f;
            m.metallicFactor = 0.25f;
            m.roughnessFactor = 0.4f;
            m.emissiveFactor = {0.f, 0.2f, 0.f};
        }
        model.materials.push_back(m);
    }
    model.primitives[0].material = 0;
    model.variantNames = {"a", "b"};
    model.primitives[0].variantMappings = {{0, {0}}, {1, {1}}};

    AtlasGroup group;
    group.id = 0;
    group.name = "t";
    group.primitives = {0};
    group.materials = {0, 1};

    Report rep;
    std::string err;
    GenOptions go;
    go.resolutionW = go.resolutionH = 64;
    go.padding = 1;
    AtlasGen gen;
    CHECK(generateAtlas(model, group, 0, go, gen, rep, err));

    BakeOptions bo;
    bo.filter = Filter::Nearest;
    std::vector<BakedImage> none;
    CHECK(bakeGroup(model, group, gen, bo, none, rep, err));
    CHECK(none.empty());

    bo.bakeFactors = true;
    std::vector<BakedImage> baked;
    CHECK(bakeGroup(model, group, gen, bo, baked, rep, err));

    int basecolorCopies = 0, occlCopies = 0;
    for (const BakedImage& b : baked) {
        if (b.type == TexType::BaseColor) {
            basecolorCopies++;
            CHECK(b.material >= 0);
            CHECK(b.factorBaked & factorBit(TexType::BaseColor));
            int hits = 0;
            Raster sc;
            const std::vector<uint8_t>& rgba = bakedRgba(b, sc);
            for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
                if (rgba[i + 3] == 0) continue;
                bool m0 = rgba[i] > 180 && rgba[i + 1] < 70;
                bool m1 = rgba[i + 1] > 180 && rgba[i] < 70;
                if ((b.material == 0 && m0) || (b.material == 1 && m1)) hits++;
            }
            CHECK(hits > 0);
        }
        if (b.type == TexType::Occlusion) {
            occlCopies++;
        }
    }
    CHECK(basecolorCopies == 2);
    // occlusion strength is not folded when no occlusion texture exists
    CHECK(occlCopies == 0);
    // metallicRoughness folds only the material with non-default factors
    int mrCopies = 0;
    for (const BakedImage& b : baked)
        if (b.type == TexType::MetallicRoughness) mrCopies++;
    CHECK(mrCopies == 1);
}

static void testSharedFactorBake() {
    // Two texture-less materials in one atlas group without variants: the
    // baker emits a single shared image per type (material -1) carrying both
    // factor colors; --split-materials restores the per-material layout.
    Model model = makeQuadModel();
    Primitive p2 = model.primitives[0];
    for (auto& v : p2.vertices) v.position.x += 2.f;
    p2.primIndex = 1;
    p2.meshIndex = 1;
    p2.material = 1;
    model.primitives.push_back(std::move(p2));
    model.materials.clear();
    Material m0;
    m0.name = "red";
    m0.baseColorFactor = {0.8f, 0.05f, 0.05f, 1.f};
    Material m1;
    m1.name = "green";
    m1.baseColorFactor = {0.05f, 0.8f, 0.05f, 1.f};
    model.materials = {m0, m1};

    AtlasGroup group;
    group.id = 0;
    group.name = "t";
    group.primitives = {0, 1};
    group.materials = {0, 1};

    Report rep;
    std::string err;
    GenOptions go;
    go.resolutionW = go.resolutionH = 64;
    go.padding = 1;
    AtlasGen gen;
    CHECK(generateAtlas(model, group, 0, go, gen, rep, err));

    BakeOptions bo;
    bo.filter = Filter::Nearest;
    bo.bakeFactors = true;
    std::vector<BakedImage> baked;
    CHECK(bakeGroup(model, group, gen, bo, baked, rep, err));
    int bc = 0;
    int red = 0, green = 0;
    for (const BakedImage& b : baked) {
        if (b.type != TexType::BaseColor) continue;
        bc++;
        CHECK(b.material == -1);
        CHECK(b.factorBaked & factorBit(TexType::BaseColor));
        Raster sc;
        const std::vector<uint8_t>& rgba = bakedRgba(b, sc);
        for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
            if (rgba[i + 3] == 0) continue;
            if (rgba[i] > 180 && rgba[i + 1] < 70) red++;
            if (rgba[i + 1] > 180 && rgba[i] < 70) green++;
        }
    }
    CHECK(bc == 1);
    CHECK(red > 0);
    CHECK(green > 0);

    bo.splitMaterials = true;
    std::vector<BakedImage> splitBaked;
    CHECK(bakeGroup(model, group, gen, bo, splitBaked, rep, err));
    int sbc = 0;
    for (const BakedImage& b : splitBaked) {
        if (b.type != TexType::BaseColor) continue;
        sbc++;
        CHECK(b.material >= 0);
    }
    CHECK(sbc == 2);
}

static void testMultiPageAtlas() {
    // Four 1x1 quads packed at high texel density against a tiny fixed
    // resolution force several xatlas sub-atlases; the baker stacks them
    // vertically into one tall image and every vertex names a real page.
    Model model = makeQuadModel();
    for (int k = 1; k < 4; k++) {
        Primitive p = model.primitives[0];
        p.meshIndex = k;
        p.primIndex = 0;
        for (auto& v : p.vertices) v.position.x += 2.f * k;
        model.primitives.push_back(std::move(p));
    }
    Image img;
    img.name = "flat";
    img.w = 4;
    img.h = 4;
    img.decoded = true;
    img.rgba.assign(4 * 4 * 4, 0);
    for (size_t i = 0; i < 16; i++) {
        img.rgba[i * 4 + 1] = 220;
        img.rgba[i * 4 + 3] = 255;
    }
    Material m;
    m.name = "m0";
    m.tex[int(TexType::BaseColor)].present = true;
    m.tex[int(TexType::BaseColor)].imageIndex = 0;
    m.tex[int(TexType::BaseColor)].texCoord = 0;
    model.images.push_back(std::move(img));
    model.materials.push_back(m);

    AtlasGroup group;
    group.id = 0;
    group.name = "t";
    group.primitives = {0, 1, 2, 3};
    group.materials = {0};
    GenOptions go;
    go.resolutionW = go.resolutionH = 16;
    go.padding = 1;
    go.texelsPerUnit = 12.f;

    Report rep;
    std::string err;
    AtlasGen gen;
    CHECK(generateAtlas(model, group, 0, go, gen, rep, err));
    CHECK(gen.pageCount >= 2);
    for (const GenMesh& gm : gen.meshes)
        for (const GenVertex& gv : gm.vertices) {
            CHECK(gv.atlasIndex >= -1 && gv.atlasIndex < gen.pageCount);
            if (gv.atlasIndex < 0) continue;
            CHECK(gv.u >= 0.f && gv.u <= float(gen.width));
            CHECK(gv.v >= 0.f && gv.v <= float(gen.height));
        }

    BakeOptions bo;
    bo.filter = Filter::Nearest;
    std::vector<BakedImage> baked;
    CHECK(bakeGroup(model, group, gen, bo, baked, rep, err));
    int bc = 0;
    for (const BakedImage& b : baked)
        if (b.type == TexType::BaseColor) bc++;
    CHECK(bc == 1);
    for (const BakedImage& b : baked) {
        if (b.type != TexType::BaseColor) continue;
        CHECK(b.image.w == gen.width);
        CHECK(b.image.h == gen.height * gen.pageCount);
        int greenPages = 0;
        Raster sc;
        const std::vector<uint8_t>& rgba = bakedRgba(b, sc);
        for (int p = 0; p < gen.pageCount; p++) {
            bool green = false;
            for (int y = p * gen.height; y < (p + 1) * gen.height && !green; y++)
                for (int x = 0; x < gen.width && !green; x++) {
                    const uint8_t* px = &rgba[(size_t(y) * gen.width + x) * 4];
                    if (px[1] > 180 && px[0] < 60) green = true;
                }
            if (green) greenPages++;
        }
        CHECK(greenPages >= 2);
    }
}

static void testNonSquareAtlas() {
    // xatlas packs square; the runner rescales chart pixels to the requested
    // W x H so width/height (and everything derived from them) are non-square.
    Model model = makeQuadModel();
    AtlasGroup group;
    group.id = 0;
    group.name = "t";
    group.primitives = {0};
    group.materials = {0};
    GenOptions go;
    go.resolutionW = 64;
    go.resolutionH = 16;
    go.padding = 1;
    go.texelsPerUnit = 4.f;

    Report rep;
    std::string err;
    AtlasGen gen;
    CHECK(generateAtlas(model, group, 0, go, gen, rep, err));
    CHECK(gen.width == 64);
    CHECK(gen.height == 16);
    bool any = false;
    for (const GenMesh& gm : gen.meshes)
        for (const GenVertex& gv : gm.vertices) {
            if (gv.atlasIndex < 0) continue;
            any = true;
            CHECK(gv.u >= 0.f && gv.u <= 64.f);
            CHECK(gv.v >= 0.f && gv.v <= 16.f);
        }
    CHECK(any);

    std::vector<BakedImage> baked;
    BakeOptions bo;
    bo.filter = Filter::Nearest;
    CHECK(bakeGroup(model, group, gen, bo, baked, rep, err));
    for (const BakedImage& b : baked) {
        CHECK(b.image.w == 64);
        CHECK(b.image.h == 16 * gen.pageCount);
    }
}

static void testSelfCheck() {
    // A quadrant texture distinguishes all four regions; the statistical
    // self-check must agree with the source and flag nothing.
    Model model = makeQuadModel();
    Image img;
    img.name = "quadrants";
    img.w = 4;
    img.h = 4;
    img.decoded = true;
    img.rgba.assign(4 * 4 * 4, 0);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            uint8_t* p = &img.rgba[(y * 4 + x) * 4];
            bool right = x >= 2, top = y < 2;
            if (top && !right) { p[0] = 255; }
            else if (top && right) { p[1] = 255; }
            else if (!right) { p[2] = 255; }
            else { p[0] = p[1] = p[2] = 255; }
            p[3] = 255;
        }
    Material m;
    m.name = "m0";
    m.tex[int(TexType::BaseColor)].present = true;
    m.tex[int(TexType::BaseColor)].imageIndex = 0;
    m.tex[int(TexType::BaseColor)].texCoord = 0;
    model.images.push_back(std::move(img));
    model.materials.push_back(m);

    AtlasGroup group;
    group.id = 0;
    group.name = "t";
    group.primitives = {0};
    group.materials = {0};
    GenOptions go;
    go.resolutionW = go.resolutionH = 64;
    go.padding = 1;
    Report rep;
    std::string err;
    AtlasGen gen;
    CHECK(generateAtlas(model, group, 0, go, gen, rep, err));
    BakeOptions bo;
    bo.filter = Filter::Nearest;
    std::vector<BakedImage> baked;
    CHECK(bakeGroup(model, group, gen, bo, baked, rep, err));

    selfCheckGroup(model, group, gen, bo, baked, 400, rep);
    bool sawLine = false;
    for (const auto& i : rep.infos())
        if (i.find("self-check 't' basecolor:") == 0) sawLine = true;
    CHECK(sawLine);
    for (const auto& w : rep.warnings())
        CHECK(w.find("low agreement") == std::string::npos);
}

static void testPreviewRenderers() {
    Model model = makeQuadModel();
    AtlasGroup group;
    group.id = 0;
    group.name = "t";
    group.primitives = {0};
    GenOptions go;
    go.resolutionW = go.resolutionH = 64;
    go.padding = 1;
    Report rep;
    std::string err;
    AtlasGen gen;
    CHECK(generateAtlas(model, group, 0, go, gen, rep, err));
    const char* paths[2] = {"unit_uv_compare.png", "unit_heatmaps.png"};
    CHECK(renderUvCompare(model, 0, gen, paths[0], rep, err));
    CHECK(renderAtlasHeatmaps(model, gen, paths[1], rep, err));
    bool sawHeatInfo = false;
    for (const auto& i : rep.infos())
        if (i.find("preview heatmaps: density range") == 0) sawHeatInfo = true;
    CHECK(sawHeatInfo);
    for (const char* p : paths) {
        std::ifstream f(p, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
        Raster r;
        CHECK(decodeImage(bytes, r, err));
        CHECK(r.w == 2064 && r.h == 1024);  // 2 x 1024 panels + 16 px divider
        std::remove(p);
    }
}

// Material extensions are copied to the output verbatim; their textureInfo
// texCoord sets must be detected at load so the atlas can avoid them.
static void testExtTexCoordDetection() {
    const char* path = "unit_ext_texcoord.gltf";
    {
        std::ofstream f(path);
        f << R"json({
"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1,"TEXCOORD_1":2},
"indices":3,"material":0}]}],
"materials":[{"name":"m","extensions":{"KHR_materials_clearcoat":{"clearcoatFactor":1.0,
"clearcoatRoughnessTexture":{"index":0,"texCoord":1},
"clearcoatNormalTexture":{"index":0}}}}],
"textures":[{"source":0,"sampler":0}],"samplers":[{"magFilter":9729,"minFilter":9729}],
"images":[{"uri":"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4//8/AAX+Av4N70a4AAAAAElFTkSuQmCC"}],
"buffers":[{"byteLength":96,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAD8AAAA/AAAAAAAAAAAAAIA+AAAAAAEAAAACAAAA"}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},
{"buffer":0,"byteOffset":36,"byteLength":24,"target":34962},
{"buffer":0,"byteOffset":60,"byteLength":24,"target":34962},
{"buffer":0,"byteOffset":84,"byteLength":12,"target":34963}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},
{"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},
{"bufferView":3,"componentType":5125,"count":3,"type":"SCALAR"}]
})json";
    }
    Model model;
    GltfDocument doc;
    Report rep;
    std::string err;
    CHECK(doc.load(path, model, rep, err));
    std::remove(path);
    if (model.materials.empty()) return;
    const std::vector<int>& sets = model.materials[0].extTexCoordSets;
    CHECK(sets.size() == 2);
    bool saw0 = false, saw1 = false;
    for (int s : sets) {
        if (s == 0) saw0 = true;  // implicit texCoord
        if (s == 1) saw1 = true;
    }
    CHECK(saw0 && saw1);
    CHECK(model.primitives.empty() || model.primitives[0].vertices.size() == 3);
    if (!model.primitives.empty()) {
        const SourceVertex& v = model.primitives[0].vertices[1];
        CHECK(v.uvsets.size() == 2);
        if (v.uvsets.size() == 2) CHECK(v.uvsets[1].u == 0.5f && v.uvsets[1].v == 0.f);
    }
}

static void testRenderJson() {
    Report rep;
    rep.stats.primitivesTotal = 3;
    rep.stats.atlasCount = 2;

    rep.warn("quote\" and newline\n");
    rep.info("plain");
    ReportMeta meta;
    meta.tool = "uvremap";
    meta.version = "0.0.0";
    meta.input = "in.glb";
    meta.output = "out";
    meta.atlasMode = "per-material";
    std::string j = rep.renderJson(meta);
    CHECK(j.find("\"tool\": \"uvremap\"") != std::string::npos);
    CHECK(j.find("\"primitivesTotal\": 3") != std::string::npos);
    CHECK(j.find("\\\"") != std::string::npos);   // escaped quote
    CHECK(j.find("\\n") != std::string::npos);    // escaped newline
    CHECK(j.find("\"atlasMode\": \"per-material\"") != std::string::npos);
}

int main() {
    testNormalIdentity();
    testNormalFlatRotation();
    testSampling();
    testNeutral();
    testPngRoundTrip();
    testTriangulate();
    testColocalTwinRegression();
    testTextureTransformRotation();
    testMaterialsVariantsBake();
    testBakeFactors();
    testSharedFactorBake();
    testMultiPageAtlas();
    testNonSquareAtlas();
    testSelfCheck();
    testPreviewRenderers();
    testExtTexCoordDetection();
    testRenderJson();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all unit checks passed\n");
    return 0;
}
