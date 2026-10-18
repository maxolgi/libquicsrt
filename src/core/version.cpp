#include "quicsrt/version.hpp"

namespace quicsrt
{

namespace
{

// Defined by CMake from the project version.
constexpr Version kVersion = {QUICSRT_VERSION_MAJOR, QUICSRT_VERSION_MINOR, QUICSRT_VERSION_PATCH};

} // namespace

const char* version_string()
{
    return QUICSRT_VERSION_STRING;
}

Version version()
{
    return kVersion;
}

} // namespace quicsrt
