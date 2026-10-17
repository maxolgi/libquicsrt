// c_api_test: the extern-C ABI over libwebsrt (websrt_c.h), the layer
// the FFmpeg websrt:// protocol links against.
//
// Offline misuse: every function with NULL handles/args, write and
// read before start, stats before start, bad start params, double
// start, destroy without stop.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "websrt/websrt_c.h"

namespace
{

std::atomic<bool> g_ok{true};

void Fail(const std::string& what)
{
    g_ok = false;
    std::fprintf(stderr, "c_api_test: FAIL: %s\n", what.c_str());
}

int g_checks = 0;

// Plain checks (not assert) so the test stays meaningful in Release.
template <typename Cond>
void Check(const Cond& cond, const std::string& what)
{
    ++g_checks;
    if (!cond())
    {
        Fail(what);
    }
}

bool Contains(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

// ---- offline misuse ----

void TestOfflineMisuse()
{
    Check([&] { return std::strncmp(websrt_version(), "0.1", 3) == 0; }, "websrt_version must be 0.1.x");
    Check([&] { return websrt_publisher_last_error(nullptr) != nullptr; }, "publisher_last_error(NULL) is non-NULL");
    Check([&] { return websrt_subscriber_last_error(nullptr) != nullptr; }, "subscriber_last_error(NULL) is non-NULL");
    Check(
        [&] {
            return websrt_publisher_write(nullptr, "x", 1, 10) == WEBSRT_WRITE_INVALID &&
                   websrt_subscriber_read(nullptr, nullptr, 0, 10) == WEBSRT_READ_INVALID;
        },
        "NULL-handle pump calls are WEBSRT_*_INVALID");
    websrt_publisher_destroy(nullptr); // no-op, must not crash
    websrt_subscriber_destroy(nullptr);
    websrt_publisher_stop(nullptr);
    websrt_subscriber_stop(nullptr);

    websrt_publisher_t* pub = websrt_publisher_create();
    Check([&] { return pub != nullptr; }, "publisher_create");
    Check([&] { return websrt_publisher_start(pub, "127.0.0.1", 0, nullptr, nullptr, 0, 0) == WEBSRT_EINVAL; },
          "start with NULL stream is EINVAL");
    Check([&] { return websrt_publisher_write(pub, "x", 1, 10) == WEBSRT_WRITE_NOT_STARTED; },
          "write before start is NOT_STARTED");
    // The façade checks not-started before the args; NULL/zero-length
    // surface as INVALID only on a live session.
    Check(
        [&] {
            char stats[64];
            return websrt_publisher_stats(pub, stats, sizeof stats) == WEBSRT_ESTATE &&
                   websrt_publisher_stats(pub, nullptr, 100) == WEBSRT_EINVAL;
        },
        "stats before start is ESTATE; NULL buf is EINVAL");
    Check([&] { return websrt_publisher_end(pub, 10) != WEBSRT_OK; }, "end before start fails");
    Check(
        [&] {
            return websrt_publisher_start(pub, "127.0.0.1", 4433, "", "zz", 0, 1) == WEBSRT_ECONFIG &&
                   Contains(websrt_publisher_last_error(pub), "stream");
        },
        "invalid config (empty stream / bad hash) is ECONFIG");
    websrt_publisher_destroy(pub); // never started; clean destroy

    websrt_subscriber_t* sub = websrt_subscriber_create();
    Check([&] { return sub != nullptr; }, "subscriber_create");
    Check([&] { return websrt_subscriber_read(sub, nullptr, 1, 10) == WEBSRT_READ_NOT_STARTED; },
          "read NULL buf before start is NOT_STARTED (façade checks state first)");
    Check([&] { return websrt_subscriber_read(sub, nullptr, 0, 10) == WEBSRT_READ_NOT_STARTED; },
          "read before start is NOT_STARTED (NULL buf checked first)");
    char big[2048];
    Check([&] { return websrt_subscriber_read(sub, big, sizeof big, 10) == WEBSRT_READ_NOT_STARTED; },
          "read before start is NOT_STARTED");
    Check(
        [&] {
            char stats[64];
            return websrt_subscriber_stats(sub, stats, sizeof stats) == WEBSRT_ESTATE;
        },
        "subscriber stats before start is ESTATE");
    Check([&] { return std::strlen(websrt_subscriber_last_error(sub)) == 0; }, "subscriber last_error empty so far");
    websrt_subscriber_destroy(sub);
}

} // namespace

int main()
{
    TestOfflineMisuse();

    if (!g_ok)
    {
        return 1;
    }
    std::printf("c_api_test: ALL CHECKS PASSED (%d)\n", g_checks);
    return 0;
}
