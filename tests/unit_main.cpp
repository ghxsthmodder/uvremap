// Minimal self-contained unit checks for uvremap core.
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/gltf_io.h"
#include "core/grouping.h"
#include "core/image.h"
#include "core/normal_math.h"
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
        const Image& imgOut = b.image;
        int red = 0;
        for (size_t i = 0; i + 3 < imgOut.rgba.size(); i += 4)
            if (imgOut.rgba[i] > 200 && imgOut.rgba[i + 1] < 60 && imgOut.rgba[i + 2] < 60)
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
            const Image& im = b.image;
            for (size_t i = 0; i + 3 < im.rgba.size(); i += 4) {
                if (im.rgba[i + 3] == 0) continue;  // neutral fill
                int r = im.rgba[i], g = im.rgba[i + 1], b2 = im.rgba[i + 2];
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
        for (size_t i = 0; i + 3 < b.image.rgba.size(); i += 4) {
            if (b.image.rgba[i + 3] == 0) continue;
            if (b.image.rgba[i + wanted] > 200 && b.image.rgba[i + (1 - wanted)] < 60)
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
            for (size_t i = 0; i + 3 < b.image.rgba.size(); i += 4) {
                if (b.image.rgba[i + 3] == 0) continue;
                bool m0 = b.image.rgba[i] > 180 && b.image.rgba[i + 1] < 70;
                bool m1 = b.image.rgba[i + 1] > 180 && b.image.rgba[i] < 70;
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
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all unit checks passed\n");
    return 0;
}
