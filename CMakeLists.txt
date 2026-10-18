cmake_minimum_required(VERSION 3.24)

project(quicsrt
    VERSION 0.1.0
    DESCRIPTION "SRT over WebTransport: client library, gateway, and FFmpeg protocol"
    LANGUAGES CXX)

# Strict warnings for OUR targets only; submodules build with their own settings.
add_library(quicsrt_warnings INTERFACE)
target_compile_options(quicsrt_warnings INTERFACE -Wall -Wextra -Wpedantic)

# srt fork: static, no built-in crypto (WebTransport's TLS provides it),
# C++ standard sync primitives instead of pthreads, virtual transport seam
# compiled in (datagrams via owner-supplied callback, not OS UDP sockets).
set(ENABLE_ENCRYPTION OFF CACHE BOOL "" FORCE)
set(ENABLE_STDCXX_SYNC ON CACHE BOOL "" FORCE)
set(ENABLE_VIRTUAL_TRANSPORT ON CACHE BOOL "" FORCE)
set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(ENABLE_APPS OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/srt)

# Shared flavor: libquicsrt.so exporting ONLY the extern-C ABI
# (include/quicsrt/quicsrt_c.h) via a linker version script - the C++ API,
# the srt fork, lsquic, and BoringSSL are baked in as local symbols.
# Sharing no symbols with anything else in the process is what lets one
# FFmpeg binary carry quicsrt:// (this DSO) and srt:// (a system libsrt)
# side by side; see README "Coexisting with system srt". The static
# flavor stays the default: in-tree consumers (quicsrt-gateway-cpp, the
# internal tests) call the C++ API and the fork's sockets directly and
# need those symbols visible.
option(QUICSRT_BUILD_SHARED "Build libquicsrt.so exporting only the C ABI" OFF)
if(QUICSRT_BUILD_SHARED)
    # Objects linked into the DSO must be PIC; the fork enables it only
    # for its own shared flavor, so set it on the object library here.
    set_target_properties(srt_virtual PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# Consumer export surface. An out-of-tree consumer (the quicsrt-gateway-cpp
# repo does add_subdirectory(deps/libquicsrt)) links these ALIASes and
# nothing else:
#   quicsrt::quicsrt   - libquicsrt plus its internal headers (src/) and the
#                      srt fork's headers/link with the
#                      SRT_ENABLE_VIRTUAL_TRANSPORT gate, so the fork API
#                      (srt_attach_virtual_transport, SRTO_SKIP_INDUCTION,
#                      generated version.h) is callable directly.
#   quicsrt::srt-deps - the srt surface alone (headers + gate + link) for
#                      consumers that drive raw forked sockets.
#   quicsrt::wt-deps  - the lsquic + BoringSSL + zlib chain (headers +
#                      link) for consumers hosting their own lsquic engine;
#                      WT-on builds only.
add_library(quicsrt_srt_deps INTERFACE)
add_library(quicsrt::srt-deps ALIAS quicsrt_srt_deps)
target_include_directories(quicsrt_srt_deps INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/srt/srtcore
    ${CMAKE_CURRENT_BINARY_DIR}/deps/srt)
target_compile_definitions(quicsrt_srt_deps INTERFACE SRT_ENABLE_VIRTUAL_TRANSPORT)
target_link_libraries(quicsrt_srt_deps INTERFACE srt_static)

# WebTransport support: lsquic (submodule) over BoringSSL (fetched by
# ./build.sh setup, never committed). OFF keeps the srt-only fast path
# (M0/CI): no BoringSSL, no lsquic.
option(QUICSRT_ENABLE_WT "Build WebTransport support (lsquic + BoringSSL)" ON)

# Receiver-only flavor: just the WT-free ReceiverCore over the srt fork.
# No EventLoop (epoll/eventfd - Emscripten has neither), no capsule
# framing, no tests or apps. This is the Emscripten consumer surface:
# QuicSRT-wasm configures deps/libquicsrt with emcmake and links
# quicsrt::receiver-core + quicsrt::srt-deps.
option(QUICSRT_RECEIVER_ONLY "Build only the WT-free receiver core (no event loop)" OFF)

if(QUICSRT_ENABLE_WT)
    set(QUICSRT_BORINGSSL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/boringssl")
    set(QUICSRT_BORINGSSL_BUILD "${QUICSRT_BORINGSSL_DIR}/build")
    if(NOT (EXISTS "${QUICSRT_BORINGSSL_DIR}/include/openssl/ssl.h"
            AND EXISTS "${QUICSRT_BORINGSSL_BUILD}/libssl.a"
            AND EXISTS "${QUICSRT_BORINGSSL_BUILD}/libcrypto.a"))
        message(FATAL_ERROR
            "QUICSRT_ENABLE_WT requires BoringSSL in deps/boringssl "
            "(run ./build.sh setup, or configure with -DQUICSRT_ENABLE_WT=OFF)")
    endif()

    # lsquic compiles with its own settings: its CMake adjusts only
    # directory-scoped flags/includes inside the add_subdirectory, so it is
    # not an INTERFACE parasite on quicsrt_warnings. The granular BORINGSSL_*
    # variables are the add_subdirectory integration lsquic's CMake documents;
    # LIBSSL_DIR would instead expect an in-source BoringSSL build.
    set(LSQUIC_LIBSSL "BORINGSSL" CACHE STRING "" FORCE)
    set(BORINGSSL_INCLUDE "${QUICSRT_BORINGSSL_DIR}/include" CACHE PATH "" FORCE)
    set(BORINGSSL_LIB_ssl "${QUICSRT_BORINGSSL_BUILD}/libssl.a" CACHE FILEPATH "" FORCE)
    set(BORINGSSL_LIB_crypto "${QUICSRT_BORINGSSL_BUILD}/libcrypto.a" CACHE FILEPATH "" FORCE)
    # Example binaries need libevent; tests are Debug-only and assert-based.
    set(LSQUIC_BIN OFF CACHE BOOL "" FORCE)
    set(LSQUIC_TESTS OFF CACHE BOOL "" FORCE)
    # Server-side WebTransport (LSQUIC_WEBTRANSPORT_SERVER_SUPPORT); the
    # client side hand-drives extended CONNECT via lsxpack headers.
    set(LSQUIC_WEBTRANSPORT ON CACHE BOOL "" FORCE)
    add_subdirectory(deps/lsquic)
    if(QUICSRT_BUILD_SHARED)
        set_target_properties(lsquic PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
endif()

find_package(Threads REQUIRED)

if(QUICSRT_RECEIVER_ONLY)
    # Everything past this point (the full quicsrt target, WebTransport
    # chain, tests, install rules) is native-consumer territory and skipped
    # wholesale: the receiver-only flavor exports quicsrt::receiver-core and
    # returns; its consumer (QuicSRT-wasm under Emscripten) carries its
    # own build gates.
    add_library(quicsrt_receiver_core STATIC
        src/srt_session/receiver_core.cpp
        # SendMessage's negotiated-payload readback (see negotiate.h);
        # the send path genuinely needs the second file.
        src/srt_session/negotiate.cpp)
    add_library(quicsrt::receiver-core ALIAS quicsrt_receiver_core)
    target_compile_features(quicsrt_receiver_core PUBLIC cxx_std_20)
    set_target_properties(quicsrt_receiver_core PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(quicsrt_receiver_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(quicsrt_receiver_core
        PUBLIC quicsrt_srt_deps
        PRIVATE quicsrt_warnings Threads::Threads)
    return()
endif()
if(QUICSRT_BUILD_SHARED)
    add_library(quicsrt SHARED
        src/core/event_loop.cpp
        src/core/version.cpp
        src/srt_session/receiver_core.cpp
        src/wt/capsule.cpp
        src/wt/connect_response.cpp)
    set_target_properties(quicsrt PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})
    # The export contract of the DSO: the C ABI only. Applied at link
    # time, so the fork's srt.h "default" SRT_API visibility is moot.
    target_link_options(quicsrt PRIVATE
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/quicsrt_shared.map")
else()
    add_library(quicsrt STATIC
        src/core/event_loop.cpp
        src/core/version.cpp
        src/srt_session/receiver_core.cpp
        src/wt/capsule.cpp
        src/wt/connect_response.cpp)
endif()
add_library(quicsrt::quicsrt ALIAS quicsrt)
target_compile_features(quicsrt PUBLIC cxx_std_20)
set_target_properties(quicsrt PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(quicsrt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(quicsrt
    PUBLIC quicsrt_srt_deps
    PRIVATE quicsrt_warnings Threads::Threads)
target_compile_definitions(quicsrt PRIVATE
    QUICSRT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    QUICSRT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    QUICSRT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    QUICSRT_VERSION_STRING="${PROJECT_VERSION}")

# WebTransport client: the lsquic engine host (src/wt). The
# lsquic/BoringSSL/ZLIB chain rides PUBLIC via quicsrt_wt_deps (exported as
# quicsrt::wt-deps), so consumers of quicsrt get the whole link line and can
# also host their own lsquic engine. The srt-session layer (forked srt in
# virtual-transport mode) and the public Subscriber façade ride along: the
# same include paths and gating define as the M0 loopback harness.
if(QUICSRT_ENABLE_WT)
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

    # The exported WT dependency chain (quicsrt::wt-deps, see the export
    # comment above): everything a consumer needs to run its own lsquic
    # engine against BoringSSL.
    add_library(quicsrt_wt_deps INTERFACE)
    add_library(quicsrt::wt-deps ALIAS quicsrt_wt_deps)
    target_include_directories(quicsrt_wt_deps INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/deps/lsquic/include
        ${QUICSRT_BORINGSSL_DIR}/include)
    # lsquic is compiled with the server-WT feature macro: every TU that
    # declares its structs must see the same define or the layouts diverge.
    target_compile_definitions(quicsrt_wt_deps INTERFACE LSQUIC_WEBTRANSPORT_SERVER_SUPPORT=1)
    target_link_libraries(quicsrt_wt_deps INTERFACE
        lsquic ZLIB::ZLIB
        ${QUICSRT_BORINGSSL_BUILD}/libssl.a
        ${QUICSRT_BORINGSSL_BUILD}/libcrypto.a)

    target_sources(quicsrt PRIVATE
        src/wt/client.cpp
        src/srt_session/subscriber.cpp
        src/srt_session/publisher.cpp
        src/srt_session/negotiate.cpp
        src/quicsrt/config.cpp
        src/quicsrt/resolve.cpp
        deps/srt/apps/apputil.cpp
        src/quicsrt/subscriber.cpp
        src/quicsrt/publisher.cpp
        src/quicsrt/quicsrt_c.cpp)
    # apps/ stays private: apputil.hpp/netinet_any.h are build inputs, not
    # consumer API. srtcore/generated/binary-dir includes and the VT gate
    # flow from quicsrt_srt_deps.
    target_include_directories(quicsrt PRIVATE deps/srt/apps)
    target_link_libraries(quicsrt PUBLIC quicsrt_wt_deps)
endif()

enable_testing()

# The internal tests below link the quicsrt target and call the C++ API /
# internals, which the shared flavor localizes - only the C-ABI canary
# (c_api_test) and the fork-level loopback build there.
if(NOT QUICSRT_BUILD_SHARED)
add_executable(smoke_test tests/smoke_test.cpp)
target_compile_features(smoke_test PRIVATE cxx_std_20)
set_target_properties(smoke_test PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(smoke_test PRIVATE quicsrt_warnings quicsrt)
add_test(NAME smoke_test COMMAND smoke_test)

# EventLoop units: post/fd/timer/engine-host mechanics, no WebTransport
# needed (runs on the WT-off fast path too).
add_executable(event_loop_test tests/event_loop_test.cpp)
target_compile_features(event_loop_test PRIVATE cxx_std_20)
set_target_properties(event_loop_test PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(event_loop_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(event_loop_test PRIVATE quicsrt_warnings quicsrt Threads::Threads)
add_test(NAME event_loop_test COMMAND event_loop_test)
set_tests_properties(event_loop_test PROPERTIES TIMEOUT 30)

# WtCapsuleParser units: RFC 9599 capsule framing and QUIC varints, pure
# parsing (no WebTransport needed, runs on the WT-off fast path too).
add_executable(capsule_test tests/capsule_test.cpp)
target_compile_features(capsule_test PRIVATE cxx_std_20)
set_target_properties(capsule_test PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(capsule_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(capsule_test PRIVATE quicsrt_warnings quicsrt Threads::Threads)
add_test(NAME capsule_test COMMAND capsule_test)
set_tests_properties(capsule_test PROPERTIES TIMEOUT 30)

# ConnectResponseParser units: the bounded extended-CONNECT response-
# head accumulation (64 KiB ceiling, status-line parsing), pure parsing
# (no WebTransport needed, runs on the WT-off fast path too).
add_executable(connect_response_test tests/connect_response_test.cpp)
target_compile_features(connect_response_test PRIVATE cxx_std_20)
set_target_properties(connect_response_test PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(connect_response_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(connect_response_test PRIVATE quicsrt_warnings quicsrt Threads::Threads)
add_test(NAME connect_response_test COMMAND connect_response_test)
set_tests_properties(connect_response_test PROPERTIES TIMEOUT 30)
endif()

# M0 loopback: two forked srt sockets over an in-memory wire pair (gated
# virtual transport seam, zero OS sockets). The srt CMake uses
# directory-scoped includes, so the headers and the generated version.h are
# referenced explicitly.
add_executable(loopback_srt tests/loopback_srt.cpp)
target_compile_features(loopback_srt PRIVATE cxx_std_20)
set_target_properties(loopback_srt PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(loopback_srt PRIVATE deps/srt/srtcore ${CMAKE_CURRENT_BINARY_DIR}/deps/srt)
target_compile_definitions(loopback_srt PRIVATE SRT_ENABLE_VIRTUAL_TRANSPORT)
target_link_libraries(loopback_srt PRIVATE quicsrt_warnings Threads::Threads srt_static)
add_test(NAME loopback_srt COMMAND loopback_srt)

# ReceiverCore session coverage: the WT-free receiver half driven end to
# end against a raw forked-srt initiator over an in-memory wire pair (the
# M0 pattern with the core as the listener half) - here proving the
# SRTO_INITIAL_RTT seed selection (explicit params.initial_rtt_ms override
# vs the transport-measured RTT) via the accepted socket's pre-data msRTT
# readback, plus the pure selection units shared with the publisher half
# (whose socket application needs a live WebTransport session). Links
# quicsrt: receiver_core.cpp plus the srt fork surface ride its PUBLIC
# deps.
add_executable(receiver_core_test tests/receiver_core_test.cpp)
target_compile_features(receiver_core_test PRIVATE cxx_std_20)
set_target_properties(receiver_core_test PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(receiver_core_test PRIVATE quicsrt_warnings quicsrt)
add_test(NAME receiver_core_test COMMAND receiver_core_test)
set_tests_properties(receiver_core_test PROPERTIES TIMEOUT 60)

if(QUICSRT_ENABLE_WT)
    # c_api_test: the extern-C ABI (quicsrt_c.h) the FFmpeg quicsrt://
    # protocol links against - offline misuse coverage. Builds in the
    # shared flavor too: it is the DSO's ABI canary.
    add_executable(c_api_test tests/c_api_test.cpp)
    target_compile_features(c_api_test PRIVATE cxx_std_20)
    set_target_properties(c_api_test PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(c_api_test PRIVATE quicsrt_warnings quicsrt Threads::Threads)
    add_test(NAME c_api_test COMMAND c_api_test)
    set_tests_properties(c_api_test PROPERTIES TIMEOUT 30)

    # Everything below uses the C++ API or internals, which the shared
    # flavor localizes - static flavor only.
    if(NOT QUICSRT_BUILD_SHARED)
    # resolve_test: the fork-reused resolver (CreateAddr) behind
    # ResolveHostToIpv4 - literal fast path, localhost loopback, bounded
    # NXDOMAIN. No gateway needed.
    add_executable(resolve_test tests/resolve_test.cpp)
    target_compile_features(resolve_test PRIVATE cxx_std_20)
    set_target_properties(resolve_test PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(resolve_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(resolve_test PRIVATE quicsrt_warnings quicsrt Threads::Threads)
    add_test(NAME resolve_test COMMAND resolve_test)
    set_tests_properties(resolve_test PROPERTIES TIMEOUT 60)

    # subscriber_test: libquicsrt public surface - config validation matrix
    # and offline misuse. Only the public headers are included, which
    # doubles as the no-internal-leakage check.
    add_executable(subscriber_test tests/subscriber_test.cpp)
    target_compile_features(subscriber_test PRIVATE cxx_std_20)
    set_target_properties(subscriber_test PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(subscriber_test PRIVATE quicsrt_warnings quicsrt Threads::Threads)
    add_test(NAME subscriber_test COMMAND subscriber_test)
    set_tests_properties(subscriber_test PROPERTIES TIMEOUT 60)

    # publisher_test: libquicsrt Publisher surface - config validation
    # matrix, TsChunker units, and offline misuse. Links the internal
    # TsChunker too (src/ include), which doubles as the session-layer
    # coverage.
    add_executable(publisher_test tests/publisher_test.cpp)
    target_compile_features(publisher_test PRIVATE cxx_std_20)
    set_target_properties(publisher_test PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(publisher_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        deps/srt/srtcore
        ${CMAKE_CURRENT_BINARY_DIR}/deps/srt
        deps/lsquic/include
        ${QUICSRT_BORINGSSL_DIR}/include)
    target_link_libraries(publisher_test PRIVATE quicsrt_warnings quicsrt Threads::Threads)
    target_compile_definitions(publisher_test PRIVATE SRT_ENABLE_VIRTUAL_TRANSPORT)
    add_test(NAME publisher_test COMMAND publisher_test)
    set_tests_properties(publisher_test PROPERTIES TIMEOUT 60)

    # facade_race_test: concurrent Stop() safety of the public façades -
    # watchdog threads hammer Stop() while Start fails against a dead
    # port (error-path teardown) and around the controlling thread's
    # Stop/destructor; a trailing canary proves fresh sessions in the
    # same process still start and tear down cleanly (a second
    # srt_cleanup would corrupt the global srt refcount). Dead port
    # only, no gateway: always runs.
    add_executable(facade_race_test tests/facade_race_test.cpp)
    target_compile_features(facade_race_test PRIVATE cxx_std_20)
    set_target_properties(facade_race_test PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(facade_race_test PRIVATE quicsrt_warnings quicsrt Threads::Threads)
    add_test(NAME facade_race_test COMMAND facade_race_test)
    set_tests_properties(facade_race_test PROPERTIES TIMEOUT 120)

    endif()

    # consumer_test: the export-contract proof. It compiles in its own
    # directory scope (tests/consumer/) against ONLY the exported targets -
    # no source-tree include paths, no deps/ paths - exactly like an
    # external add_subdirectory(deps/libquicsrt) consumer.
    add_subdirectory(tests/consumer)

    # quicsrt-dump: the subscriber example app - subscribe with the public
    # API and write the stream's TS bytes to a file.
    add_executable(quicsrt-dump apps/quicsrt-dump.cpp)
    target_compile_features(quicsrt-dump PRIVATE cxx_std_20)
    set_target_properties(quicsrt-dump PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(quicsrt-dump PRIVATE quicsrt_warnings quicsrt)
endif()

# Install + generated pkg-config for out-of-tree consumers. FFmpeg's
# configure runs pkg-config WITHOUT --static, so Libs: must carry the whole
# static chain (-lsrt, -llsquic, -lssl, -lcrypto, ...) - the line build.sh
# hand-rolls in its ffmpeg pkg-config shim, generated here from the
# configured tree instead of rewritten per run. The template is relocatable
# (prefix=${pcfiledir}/../..): the tree is configured with the default
# /usr/local prefix, but `cmake --install ... --prefix <dir>` must stage a
# correct quicsrt.pc without a reconfigure.
include(GNUInstallDirs)
# LIBRARY covers the DSO of the shared flavor, ARCHIVE the static lib.
install(TARGETS quicsrt
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/quicsrt/quicsrt_c.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/quicsrt)
set(QUICSRT_PC_EXTRA "")
if(QUICSRT_BUILD_SHARED)
    # The whole chain is baked into the DSO: consumers need -lquicsrt
    # only. (The srt submodule's own install rules still stage its
    # headers/libsrt.a/srt.pc into the prefix; build.sh prunes them so
    # the staged prefix exports exactly the quicsrt C ABI and pkg-config
    # `srt` keeps resolving to the system for --enable-libsrt.)
    # The rpath lets binaries linked against this dev-stage DSO load it
    # without LD_LIBRARY_PATH; pkg-config expands ${libdir} from the pc
    # file location, so it stays relocatable. Distro-packaged libquicsrt
    # ships its own pc without an rpath and the normal search path
    # applies.
    set(QUICSRT_PC_LIBS "")
    set(QUICSRT_PC_EXTRA "-Wl,-rpath,\${libdir}")
elseif(QUICSRT_ENABLE_WT)
    install(TARGETS srt_static lsquic ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES "${QUICSRT_BORINGSSL_BUILD}/libssl.a" "${QUICSRT_BORINGSSL_BUILD}/libcrypto.a"
            DESTINATION ${CMAKE_INSTALL_LIBDIR})
    set(QUICSRT_PC_LIBS "-lsrt -llsquic -lssl -lcrypto -lz -lpthread -lstdc++ -lm")
else()
    set(QUICSRT_PC_LIBS "-lsrt -lpthread -lstdc++ -lm")
endif()
configure_file(cmake/quicsrt.pc.in "${CMAKE_CURRENT_BINARY_DIR}/quicsrt.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/quicsrt.pc"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
