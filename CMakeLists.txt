cmake_minimum_required(VERSION 3.24)

project(websrt
    VERSION 0.1.0
    DESCRIPTION "SRT over WebTransport: client library, gateway, and FFmpeg protocol"
    LANGUAGES CXX)

# Strict warnings for OUR targets only; submodules build with their own settings.
add_library(websrt_warnings INTERFACE)
target_compile_options(websrt_warnings INTERFACE -Wall -Wextra -Wpedantic)

# srt fork: static, no built-in crypto (WebTransport's TLS provides it),
# C++ standard sync primitives instead of pthreads, virtual transport seam
# compiled in (datagrams via owner-supplied callback, not OS UDP sockets).
set(ENABLE_ENCRYPTION OFF CACHE BOOL "" FORCE)
set(ENABLE_STDCXX_SYNC ON CACHE BOOL "" FORCE)
set(ENABLE_VIRTUAL_TRANSPORT ON CACHE BOOL "" FORCE)
set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(ENABLE_APPS OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/srt)

add_library(websrt STATIC src/core/version.cpp)
add_library(websrt::websrt ALIAS websrt)
target_compile_features(websrt PUBLIC cxx_std_20)
set_target_properties(websrt PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(websrt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(websrt PRIVATE websrt_warnings)
target_compile_definitions(websrt PRIVATE
    WEBSRT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    WEBSRT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    WEBSRT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    WEBSRT_VERSION_STRING="${PROJECT_VERSION}")

enable_testing()

add_executable(smoke_test tests/smoke_test.cpp)
target_compile_features(smoke_test PRIVATE cxx_std_20)
set_target_properties(smoke_test PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(smoke_test PRIVATE websrt_warnings websrt)
add_test(NAME smoke_test COMMAND smoke_test)

find_package(Threads REQUIRED)

# M0 loopback: two forked srt sockets over an in-memory wire pair (gated
# virtual transport seam, zero OS sockets). The srt CMake uses
# directory-scoped includes, so the headers and the generated version.h are
# referenced explicitly.
add_executable(loopback_srt tests/loopback_srt.cpp)
target_compile_features(loopback_srt PRIVATE cxx_std_20)
set_target_properties(loopback_srt PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(loopback_srt PRIVATE deps/srt/srtcore ${CMAKE_BINARY_DIR}/deps/srt)
target_compile_definitions(loopback_srt PRIVATE SRT_ENABLE_VIRTUAL_TRANSPORT)
target_link_libraries(loopback_srt PRIVATE websrt_warnings Threads::Threads srt_static)
add_test(NAME loopback_srt COMMAND loopback_srt)
