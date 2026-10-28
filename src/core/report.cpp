#include "core/report.h"

#include <cstdint>
#include <cstdio>
#include <sstream>

namespace uvremap {

std::string Report::renderText(const std::string& header) const {
    std::ostringstream os;
    if (!header.empty()) os << header << "\n";
    if (!usages.empty()) {
        os << "Atlas usage:\n";
        for (const AtlasUsage& u : usages) {
            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "  %s: %dx%d x%d page(s), %.1f%% covered, %.1f texels per unit, "
                          "%.4g units^2 of surface",
                          u.name.c_str(), u.width, u.height, u.pages, u.coverage * 100.0,
                          u.texelsPerUnit, u.surfaceArea);
            os << buf << "\n";
        }
    }
    if (!infos_.empty()) {
        os << "Info:\n";
        for (auto& s : infos_) os << "  " << s << "\n";
    }
    if (!warnings_.empty()) {
        os << "Warnings:\n";
        for (auto& s : warnings_) os << "  " << s << "\n";
    }
    if (!errors_.empty()) {
        os << "Errors:\n";
        for (auto& s : errors_) os << "  " << s << "\n";
    }
    return os.str();
}

namespace {
std::string jsonEsc(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (uint8_t(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", uint8_t(c));
                    out += buf;
                } else
                    out += c;
        }
    }
    return out;
}

void jsonStrings(std::ostringstream& os, const char* key, const std::vector<std::string>& v) {
    os << "  \"" << key << "\": [";
    for (size_t i = 0; i < v.size(); i++) os << (i ? ", " : "") << "\"" << jsonEsc(v[i]) << "\"";
    os << "],\n";
}
}  // namespace

std::string Report::renderJson(const ReportMeta& meta) const {
    std::ostringstream os;
    os << "{\n";
    os << "  \"tool\": \"" << jsonEsc(meta.tool) << "\",\n";
    os << "  \"version\": \"" << jsonEsc(meta.version) << "\",\n";
    os << "  \"input\": \"" << jsonEsc(meta.input) << "\",\n";
    os << "  \"output\": \"" << jsonEsc(meta.output) << "\",\n";
    os << "  \"atlasMode\": \"" << jsonEsc(meta.atlasMode) << "\",\n";
    os << "  \"stats\": {\n";
    os << "    \"primitivesTotal\": " << stats.primitivesTotal << ",\n";
    os << "    \"primitivesProcessed\": " << stats.primitivesProcessed << ",\n";
    os << "    \"primitivesSkipped\": " << stats.primitivesSkipped << ",\n";
    os << "    \"atlasCount\": " << stats.atlasCount << ",\n";
    os << "    \"atlasWidth\": " << stats.atlasWidth << ",\n";
    os << "    \"atlasHeight\": " << stats.atlasHeight << ",\n";
    os << "    \"textureMapsBaked\": " << stats.textureMapsBaked << ",\n";
    os << "    \"imagesWritten\": " << stats.imagesWritten << "\n";
    os << "  },\n";
    if (usages.empty()) {
        os << "  \"atlasUsage\": [],\n";
    } else {
        os << "  \"atlasUsage\": [\n";
        for (size_t i = 0; i < usages.size(); i++) {
            const AtlasUsage& u = usages[i];
            char coverage[40], density[40], area[40];
            std::snprintf(coverage, sizeof coverage, "%.6g", u.coverage);
            std::snprintf(density, sizeof density, "%.6g", u.texelsPerUnit);
            std::snprintf(area, sizeof area, "%.6g", u.surfaceArea);
            os << "    {\"name\": \"" << jsonEsc(u.name) << "\", "
               << "\"width\": " << u.width << ", \"height\": " << u.height << ", \"pages\": "
               << u.pages << ", \"coverage\": " << coverage
               << ", \"texelsPerUnit\": " << density << ", \"surfaceArea\": " << area << "}"
               << (i + 1 < usages.size() ? "," : "") << "\n";
        }
        os << "  ],\n";
    }
    jsonStrings(os, "warnings", warnings_);
    jsonStrings(os, "infos", infos_);
    jsonStrings(os, "errors", errors_);
    os << "  \"warningCount\": " << warningCount() << "\n";
    os << "}\n";
    return os.str();
}

}  // namespace uvremap
