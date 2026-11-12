#include "m3_paths.h"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
#if defined(_WIN32)
    assert(m3paths::file_url("C:\\Games\\Mother 3") ==
           "file:///C:/Games/Mother%203");
#else
    assert(m3paths::file_url("/tmp/m3 mods #1") ==
           "file:///tmp/m3%20mods%20%231");
#endif
    const std::string relative = m3paths::file_url("relative mods");
    assert(relative.rfind("file:///", 0) == 0);
    assert(relative.find("relative%20mods") != std::string::npos);

    std::puts("file URLs: spaces, # and relative paths escaped");
    return 0;
}
