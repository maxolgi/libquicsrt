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

# WebTransport support: lsquic (submodule) over BoringSSL (fetched by
# ./build.sh setup, never committed). OFF keeps the srt-only fast path
# (M0/CI): no BoringSSL, no lsquic.
option(WEBSRT_ENABLE_WT "Build WebTransport support (lsquic + BoringSSL)" ON)

if(WEBSRT_ENABLE_WT)
    set(WEBSRT_BORINGSSL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/boringssl")
    set(WEBSRT_BORINGSSL_BUILD "${WEBSRT_BORINGSSL_DIR}/build")
    if(NOT (EXISTS "${WEBSRT_BORINGSSL_DIR}/include/openssl/ssl.h"
            AND EXISTS "${WEBSRT_BORINGSSL_BUILD}/libssl.a"
            AND EXISTS "${WEBSRT_BORINGSSL_BUILD}/libcrypto.a"))
        message(FATAL_ERROR
            "WEBSRT_ENABLE_WT requires BoringSSL in deps/boringssl "
            "(run ./build.sh setup, or configure with -DWEBSRT_ENABLE_WT=OFF)")
    endif()

    # lsquic compiles with its own settings: its CMake adjusts only
    # directory-scoped flags/includes inside the add_subdirectory, so it is
    # not an INTERFACE parasite on websrt_warnings. The granular BORINGSSL_*
    # variables are the add_subdirectory integration lsquic's CMake documents;
    # LIBSSL_DIR would instead expect an in-source BoringSSL build.
    set(LSQUIC_LIBSSL "BORINGSSL" CACHE STRING "" FORCE)
    set(BORINGSSL_INCLUDE "${WEBSRT_BORINGSSL_DIR}/include" CACHE PATH "" FORCE)
    set(BORINGSSL_LIB_ssl "${WEBSRT_BORINGSSL_BUILD}/libssl.a" CACHE FILEPATH "" FORCE)
    set(BORINGSSL_LIB_crypto "${WEBSRT_BORINGSSL_BUILD}/libcrypto.a" CACHE FILEPATH "" FORCE)
    # Example binaries need libevent; tests are Debug-only and assert-based.
    set(LSQUIC_BIN OFF CACHE BOOL "" FORCE)
    set(LSQUIC_TESTS OFF CACHE BOOL "" FORCE)
    # Server-side WebTransport (LSQUIC_WEBTRANSPORT_SERVER_SUPPORT); the
    # client side hand-drives extended CONNECT via lsxpack headers.
    set(LSQUIC_WEBTRANSPORT ON CACHE BOOL "" FORCE)
    add_subdirectory(deps/lsquic)
endif()

find_package(Threads REQUIRED)

add_library(websrt STATIC
    src/core/event_loop.cpp
    src/core/version.cpp)
add_library(websrt::websrt ALIAS websrt)
target_compile_features(websrt PUBLIC cxx_std_20)
set_target_properties(websrt PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(websrt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(websrt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(websrt PRIVATE websrt_warnings Threads::Threads)
target_compile_definitions(websrt PRIVATE
    WEBSRT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    WEBSRT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    WEBSRT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    WEBSRT_VERSION_STRING="${PROJECT_VERSION}")

# WebTransport client: the lsquic engine host (src/wt). Carries the
# lsquic/BoringSSL/ZLIB link line so consumers of websrt need none of it.
# The srt-session layer (forked srt in virtual-transport mode) rides along:
# the same include paths and gating define as the M0 loopback harness.
if(WEBSRT_ENABLE_WT)
    find_package(ZLIB REQUIRED)
    target_sources(websrt PRIVATE
        src/wt/client.cpp
        src/srt_session/subscriber.cpp)
    target_include_directories(websrt PRIVATE
        deps/srt/srtcore
        ${CMAKE_BINARY_DIR}/deps/srt
        deps/lsquic/include
        ${WEBSRT_BORINGSSL_DIR}/include)
    target_compile_definitions(websrt PRIVATE SRT_ENABLE_VIRTUAL_TRANSPORT)
    target_link_libraries(websrt PRIVATE lsquic ZLIB::ZLIB srt_static
        ${WEBSRT_BORINGSSL_BUILD}/libssl.a ${WEBSRT_BORINGSSL_BUILD}/libcrypto.a)
endif()

enable_testing()

add_executable(smoke_test tests/smoke_test.cpp)
target_compile_features(smoke_test PRIVATE cxx_std_20)
set_target_properties(smoke_test PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(smoke_test PRIVATE websrt_warnings websrt)
add_test(NAME smoke_test COMMAND smoke_test)

# EventLoop units: post/fd/timer/engine-host mechanics, no WebTransport
# needed (runs on the WT-off fast path too).
add_executable(event_loop_test tests/event_loop_test.cpp)
target_compile_features(event_loop_test PRIVATE cxx_std_20)
set_target_properties(event_loop_test PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(event_loop_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(event_loop_test PRIVATE websrt_warnings websrt Threads::Threads)
add_test(NAME event_loop_test COMMAND event_loop_test)
set_tests_properties(event_loop_test PROPERTIES TIMEOUT 30)

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

# wt_echo: WebTransport client validation against the live Rust gateway
# (the interop oracle). Requires the Rust WebSRT checkout and its runtime
# cert-hash.js; exits 77 (ctest SKIP_RETURN_CODE) when they are absent so CI
# machines without the Rust world stay green.
if(WEBSRT_ENABLE_WT)
    add_executable(wt_echo tests/wt_echo.cpp)
    target_compile_features(wt_echo PRIVATE cxx_std_20)
    set_target_properties(wt_echo PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(wt_echo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(wt_echo PRIVATE websrt_warnings websrt Threads::Threads)
    target_compile_definitions(wt_echo PRIVATE
        WEBSRT_CERT_HASH_JS_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/../WebSRT/web/public/cert-hash.js")
    add_test(NAME wt_echo COMMAND wt_echo)
    set_tests_properties(wt_echo PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
endif()
