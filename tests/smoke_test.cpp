#include <cstdio>
#include <cstring>

#include "quicsrt/version.hpp"

// Plain checks (not assert) so the test stays meaningful in Release (-DNDEBUG).
int main()
{
    const char* const str = quicsrt::version_string();
    if (str == nullptr || std::strncmp(str, "0.1", 3) != 0)
    {
        std::fprintf(stderr, "smoke_test: unexpected version string: %s\n", str ? str : "(null)");
        return 1;
    }

    const quicsrt::Version ver = quicsrt::version();
    if (ver.major != 0 || ver.minor != 1 || ver.patch < 0)
    {
        std::fprintf(stderr, "smoke_test: unexpected version components: %d.%d.%d\n", ver.major, ver.minor, ver.patch);
        return 1;
    }

    std::printf("quicsrt version: %s\n", str);
    return 0;
}
