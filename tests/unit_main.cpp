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
    std::vector<std::pair<TexType, Image>> baked;
    CHECK(bakeGroup(model, group, gen, bo, baked, rep, err));
    bool sawBaseColor = false;
    for (auto& [t, imgOut] : baked) {
        if (t != TexType::BaseColor) continue;
        sawBaseColor = true;
        int red = 0;
        for (size_t i = 0; i + 3 < imgOut.rgba.size(); i += 4)
            if (imgOut.rgba[i] > 200 && imgOut.rgba[i + 1] < 60 && imgOut.rgba[i + 2] < 60)
                red++;
        CHECK(red > 0);
    }
    CHECK(sawBaseColor);
}

int main() {
    testNormalIdentity();
    testNormalFlatRotation();
    testSampling();
    testNeutral();
    testPngRoundTrip();
    testTriangulate();
    testColocalTwinRegression();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all unit checks passed\n");
    return 0;
}
