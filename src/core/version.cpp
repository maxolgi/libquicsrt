#include "websrt/version.hpp"

namespace websrt
{

namespace
{

// Defined by CMake from the project version.
constexpr Version kVersion = {WEBSRT_VERSION_MAJOR, WEBSRT_VERSION_MINOR, WEBSRT_VERSION_PATCH};

} // namespace

const char* version_string()
{
    return WEBSRT_VERSION_STRING;
}

Version version()
{
    return kVersion;
}

} // namespace websrt
