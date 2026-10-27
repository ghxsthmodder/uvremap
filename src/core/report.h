#pragma once

#include <string>
#include <vector>

namespace uvremap {

enum class ExitCode {
    Success = 0,
    Internal = 1,
    BadArgs = 2,
    LoadFailure = 3,
    InsufficientData = 4,
    ProcessingFailure = 5,
    WriteFailure = 6,
};

struct Stats {
    int primitivesTotal = 0;
    int primitivesProcessed = 0;
    int primitivesSkipped = 0;
    int atlasCount = 0;
    int atlasWidth = 0;
    int atlasHeight = 0;
    int textureMapsBaked = 0;
    int imagesWritten = 0;
};

class Report {
  public:
    void warn(const std::string& msg) { warnings_.push_back(msg); }
    void info(const std::string& msg) { infos_.push_back(msg); }
    void error(const std::string& msg) { errors_.push_back(msg); }

    const std::vector<std::string>& warnings() const { return warnings_; }
    const std::vector<std::string>& errors() const { return errors_; }
    int warningCount() const { return int(warnings_.size()); }

    Stats stats;

    std::string renderText(const std::string& header) const;

  private:
    std::vector<std::string> warnings_, infos_, errors_;
};

}  // namespace uvremap
