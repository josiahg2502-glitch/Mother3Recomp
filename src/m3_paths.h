#pragma once
#include <string>

namespace m3paths {

// A file:// URL for an absolute or relative path, with spaces and non-ASCII
// bytes escaped. Windows drive paths become file:///C:/...
std::string file_url(const std::string& path);

}  // namespace m3paths
