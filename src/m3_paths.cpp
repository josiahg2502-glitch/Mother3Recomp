#include "m3_paths.h"

#include <filesystem>
#include <string>

namespace m3paths {
namespace {

namespace fs = std::filesystem;

std::string escape(const std::string& path) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size());
    for (unsigned char c : path) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                           c == '_' || c == '~' || c == '/' || c == ':';
        if (plain) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

}  // namespace

std::string file_url(const std::string& path) {
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec) absolute = path;
    std::string text = absolute.generic_string();
#if defined(_WIN32)
    if (text.size() > 1 && text[1] == ':') text.insert(text.begin(), '/');
#endif
    return "file://" + escape(text);
}

}  // namespace m3paths
