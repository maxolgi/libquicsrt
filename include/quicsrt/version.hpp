#ifndef QUICSRT_VERSION_HPP
#define QUICSRT_VERSION_HPP

namespace quicsrt
{

// Numeric components of the library version (mirrors the CMake project version).
struct Version
{
    int major;
    int minor;
    int patch;
};

// Library version as a string, e.g. "0.1.0". Points to static storage.
const char* version_string();

// Library version as numeric components.
Version version();

} // namespace quicsrt

#endif // QUICSRT_VERSION_HPP
