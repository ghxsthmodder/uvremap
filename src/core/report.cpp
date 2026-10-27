#include "core/report.h"

#include <sstream>

namespace uvremap {

std::string Report::renderText(const std::string& header) const {
    std::ostringstream os;
    if (!header.empty()) os << header << "\n";
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

}  // namespace uvremap
