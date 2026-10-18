// consumer_test: proves the consumer export contract. Built in its own
// CMake directory scope (tests/consumer/) against ONLY quicsrt::quicsrt and
// quicsrt::wt-deps - the surface an out-of-tree consumer (the
// quicsrt-gateway-cpp repo, add_subdirectory(deps/libquicsrt)) sees. No
// source-tree include paths, no deps/ paths: headers, defines, and link
// must all arrive through the interface targets.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "core/event_loop.h" // internal header via the src/ interface dir
#include "lsquic.h"          // via quicsrt::wt-deps
#include "srt.h"             // via quicsrt::quicsrt -> quicsrt::srt-deps
#include "quicsrt/quicsrt_c.h" // public API via the include/ interface dir

namespace
{
int g_failures = 0;

void check(bool ok, const char* what)
{
    if (!ok)
    {
        std::fprintf(stderr, "consumer_test: FAIL: %s\n", what);
        ++g_failures;
    }
}
} // namespace

int main()
{
    // (a) srt fork headers under the virtual-transport gate: the enum and
    // the attach entry point only exist when the define is exported.
    static_assert(SRTO_SKIP_INDUCTION == 64,
                  "SRTO_SKIP_INDUCTION missing: SRT_ENABLE_VIRTUAL_TRANSPORT "
                  "did not reach the consumer");
    (void)&srt_attach_virtual_transport;
    check(srt_startup() == 0, "srt_startup/srt_cleanup link+run");
    srt_cleanup();

    // (b) the lsquic/BoringSSL/zlib chain: a consumer hosting its own
    // engine (server or client) starts here.
    check(lsquic_global_init(LSQUIC_GLOBAL_CLIENT | LSQUIC_GLOBAL_SERVER) == 0,
          "lsquic_global_init/lsquic_global_cleanup link+run");
    lsquic_global_cleanup();

    // (c) internal headers via the src/ interface include dir: drive a
    // real loop from the consumer side.
    {
        quicsrt::EventLoop loop;
        std::atomic<bool> posted{false};
        loop.Start();
        check(loop.Post([&posted] { posted = true; }), "EventLoop Post accepted");
        for (int i = 0; i < 500 && !posted; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(posted, "EventLoop Post ran on the loop thread");
        loop.Stop();
    }

    // The library's own version entry point (C ABI).
    const char* v = quicsrt_version();
    check(v != nullptr && v[0] != '\0', "quicsrt_version()");

    if (g_failures != 0)
    {
        std::fprintf(stderr, "consumer_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("consumer_test: OK (export contract holds, quicsrt_version %s)\n", v);
    return 0;
}
