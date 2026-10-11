cmake_minimum_required(VERSION 3.24)

project(websrt
    VERSION 0.1.0
    DESCRIPTION "SRT over WebTransport: client library, gateway, and FFmpeg protocol"
    LANGUAGES CXX)

# Strict warnings for OUR targets only; submodules build with their own settings.
add_library(websrt_warnings INTERFACE)
target_compile_options(websrt_warnings INTERFACE -Wall -Wextra -Wpedantic)

# srt fork: static, no built-in crypto (WebTransport's TLS provides it),
# C++ standard sync primitives instead of pthreads.
set(ENABLE_ENCRYPTION OFF CACHE BOOL "" FORCE)
set(ENABLE_STDCXX_SYNC ON CACHE BOOL "" FORCE)
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
