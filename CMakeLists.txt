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

# Consumer export surface. An out-of-tree consumer (the websrt-gateway-cpp
# repo does add_subdirectory(deps/libwebsrt)) links these ALIASes and
# nothing else:
#   websrt::websrt   - libwebsrt plus its internal headers (src/) and the
#                      srt fork's headers/link with the
#                      SRT_ENABLE_VIRTUAL_TRANSPORT gate, so the fork API
#                      (srt_attach_virtual_transport, SRTO_SKIP_INDUCTION,
#                      generated version.h) is callable directly.
#   websrt::srt-deps - the srt surface alone (headers + gate + link) for
#                      consumers that drive raw forked sockets.
#   websrt::wt-deps  - the lsquic + BoringSSL + zlib chain (headers +
#                      link) for consumers hosting their own lsquic engine;
#                      WT-on builds only.
add_library(websrt_srt_deps INTERFACE)
add_library(websrt::srt-deps ALIAS websrt_srt_deps)
target_include_directories(websrt_srt_deps INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/srt/srtcore
    ${CMAKE_CURRENT_BINARY_DIR}/deps/srt)
target_compile_definitions(websrt_srt_deps INTERFACE SRT_ENABLE_VIRTUAL_TRANSPORT)
target_link_libraries(websrt_srt_deps INTERFACE srt_static)

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
target_include_directories(websrt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(websrt
    PUBLIC websrt_srt_deps
    PRIVATE websrt_warnings Threads::Threads)
target_compile_definitions(websrt PRIVATE
    WEBSRT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    WEBSRT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    WEBSRT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    WEBSRT_VERSION_STRING="${PROJECT_VERSION}")

# WebTransport client: the lsquic engine host (src/wt). The
# lsquic/BoringSSL/ZLIB chain rides PUBLIC via websrt_wt_deps (exported as
# websrt::wt-deps), so consumers of websrt get the whole link line and can
# also host their own lsquic engine. The srt-session layer (forked srt in
# virtual-transport mode) and the public Subscriber façade ride along: the
# same include paths and gating define as the M0 loopback harness.
if(WEBSRT_ENABLE_WT)
    find_package(ZLIB REQUIRED)
    # deps/srt/apps/apputil.cpp: the fork's CreateAddr (inet_pton fast path
    # + getaddrinfo), compiled in directly - its apps support library is not
    # built (ENABLE_APPS OFF) and libsrt does not export a resolver. The
    # file is the fork's own, compiled read-only; MPL-2.0. The fork's build
    # defines SRT_VERSION globally (its CMakeLists line ~848); mirror that
    # for the one PrintLibVersion() use from the generated version.h.
    file(STRINGS "${CMAKE_CURRENT_BINARY_DIR}/deps/srt/version.h" srt_version_string_line
         REGEX "^#define SRT_VERSION_STRING ")
    string(REGEX REPLACE "^#define SRT_VERSION_STRING \"(.*)\"$" "\\1"
           srt_version_string "${srt_version_string_line}")
    set_source_files_properties(deps/srt/apps/apputil.cpp PROPERTIES
        COMPILE_DEFINITIONS SRT_VERSION="${srt_version_string}")

    # The exported WT dependency chain (websrt::wt-deps, see the export
    # comment above): everything a consumer needs to run its own lsquic
    # engine against BoringSSL.
    add_library(websrt_wt_deps INTERFACE)
    add_library(websrt::wt-deps ALIAS websrt_wt_deps)
    target_include_directories(websrt_wt_deps INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/deps/lsquic/include
        ${WEBSRT_BORINGSSL_DIR}/include)
    target_link_libraries(websrt_wt_deps INTERFACE
        lsquic ZLIB::ZLIB
        ${WEBSRT_BORINGSSL_BUILD}/libssl.a
        ${WEBSRT_BORINGSSL_BUILD}/libcrypto.a)

    target_sources(websrt PRIVATE
        src/wt/client.cpp
        src/srt_session/subscriber.cpp
        src/srt_session/publisher.cpp
        src/websrt/config.cpp
        src/websrt/resolve.cpp
        deps/srt/apps/apputil.cpp
        src/websrt/subscriber.cpp
        src/websrt/publisher.cpp
        src/websrt/websrt_c.cpp)
    # apps/ stays private: apputil.hpp/netinet_any.h are build inputs, not
    # consumer API. srtcore/generated/binary-dir includes and the VT gate
    # flow from websrt_srt_deps.
    target_include_directories(websrt PRIVATE deps/srt/apps)
    target_link_libraries(websrt PUBLIC websrt_wt_deps)
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
target_include_directories(loopback_srt PRIVATE deps/srt/srtcore ${CMAKE_CURRENT_BINARY_DIR}/deps/srt)
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

    # resolve_test: the fork-reused resolver (CreateAddr) behind
    # ResolveHostToIpv4 - literal fast path, localhost loopback, bounded
    # NXDOMAIN. No gateway needed.
    add_executable(resolve_test tests/resolve_test.cpp)
    target_compile_features(resolve_test PRIVATE cxx_std_20)
    set_target_properties(resolve_test PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(resolve_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(resolve_test PRIVATE websrt_warnings websrt Threads::Threads)
    add_test(NAME resolve_test COMMAND resolve_test)
    set_tests_properties(resolve_test PROPERTIES TIMEOUT 60)

    # subscriber_test: libwebsrt public surface - config validation matrix
    # and offline misuse always run; the live negative path (nonexistent
    # stream -> HTTP 404) needs the same Rust gateway oracle and skips the
    # same way. Only the public headers are included, which doubles as the
    # no-internal-leakage check.
    add_executable(subscriber_test tests/subscriber_test.cpp)
    target_compile_features(subscriber_test PRIVATE cxx_std_20)
    set_target_properties(subscriber_test PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(subscriber_test PRIVATE websrt_warnings websrt Threads::Threads)
    target_compile_definitions(subscriber_test PRIVATE
        WEBSRT_CERT_HASH_JS_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/../WebSRT/web/public/cert-hash.js")
    add_test(NAME subscriber_test COMMAND subscriber_test)
    set_tests_properties(subscriber_test PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

    # publisher_test: libwebsrt Publisher surface - config validation
    # matrix, TsChunker units, and offline misuse always run; the live
    # gate (feed fixtures/test.ts through the gateway, byte-for-byte
    # in-process subscriber dump, first-wins 403 challenger, name freed
    # after End) needs the Rust gateway oracle and skips the same way.
    # Links the internal TsChunker too (src/ include), which doubles as
    # the session-layer coverage.
    add_executable(publisher_test tests/publisher_test.cpp)
    target_compile_features(publisher_test PRIVATE cxx_std_20)
    set_target_properties(publisher_test PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(publisher_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        deps/srt/srtcore
        ${CMAKE_CURRENT_BINARY_DIR}/deps/srt
        deps/lsquic/include
        ${WEBSRT_BORINGSSL_DIR}/include)
    target_link_libraries(publisher_test PRIVATE websrt_warnings websrt Threads::Threads)
    target_compile_definitions(publisher_test PRIVATE
        SRT_ENABLE_VIRTUAL_TRANSPORT
        WEBSRT_CERT_HASH_JS_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/../WebSRT/web/public/cert-hash.js"
        WEBSRT_FIXTURE_TS_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/../WebSRT/fixtures/test.ts")
    add_test(NAME publisher_test COMMAND publisher_test)
    set_tests_properties(publisher_test PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

    # c_api_test: the extern-C ABI (websrt_c.h) the FFmpeg websrt://
    # protocol links against - offline misuse always runs; the live
    # gate (publish + subscribe round trip through the C API against
    # the Rust gateway oracle) skips the same way as its siblings.
    add_executable(c_api_test tests/c_api_test.cpp)
    target_compile_features(c_api_test PRIVATE cxx_std_20)
    set_target_properties(c_api_test PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(c_api_test PRIVATE websrt_warnings websrt Threads::Threads)
    target_compile_definitions(c_api_test PRIVATE
        WEBSRT_CERT_HASH_JS_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/../WebSRT/web/public/cert-hash.js"
        WEBSRT_FIXTURE_TS_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/../WebSRT/fixtures/test.ts")
    add_test(NAME c_api_test COMMAND c_api_test)
    set_tests_properties(c_api_test PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

    # consumer_test: the export-contract proof. It compiles in its own
    # directory scope (tests/consumer/) against ONLY the exported targets -
    # no source-tree include paths, no deps/ paths - exactly like an
    # external add_subdirectory(deps/libwebsrt) consumer.
    add_subdirectory(tests/consumer)

    # websrt-dump: the P1 interop app - subscribe with the public API and
    # write the stream's TS bytes to a file (driven by scripts/interop.sh).
    add_executable(websrt-dump apps/websrt-dump.cpp)
    target_compile_features(websrt-dump PRIVATE cxx_std_20)
    set_target_properties(websrt-dump PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(websrt-dump PRIVATE websrt_warnings websrt)
endif()
