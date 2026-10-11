#ifndef WEBSRT_VERSION_HPP
#define WEBSRT_VERSION_HPP

namespace websrt
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

} // namespace websrt

#endif // WEBSRT_VERSION_HPP
