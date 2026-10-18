// c_api_test: the extern-C ABI over libquicsrt (quicsrt_c.h), the layer
// the FFmpeg quicsrt:// protocol links against.
//
// Offline misuse: every function with NULL handles/args, write and
// read before start, stats before start, bad start params, double
// start, destroy without stop.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "quicsrt/quicsrt_c.h"

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
    Check([&] { return std::strncmp(quicsrt_version(), "0.1", 3) == 0; }, "quicsrt_version must be 0.1.x");
    Check([&] { return quicsrt_publisher_last_error(nullptr) != nullptr; }, "publisher_last_error(NULL) is non-NULL");
    Check([&] { return quicsrt_subscriber_last_error(nullptr) != nullptr; }, "subscriber_last_error(NULL) is non-NULL");
    Check(
        [&] {
            return quicsrt_publisher_write(nullptr, "x", 1, 10) == QUICSRT_WRITE_INVALID &&
                   quicsrt_subscriber_read(nullptr, nullptr, 0, 10) == QUICSRT_READ_INVALID;
        },
        "NULL-handle pump calls are QUICSRT_*_INVALID");
    quicsrt_publisher_destroy(nullptr); // no-op, must not crash
    quicsrt_subscriber_destroy(nullptr);
    quicsrt_publisher_stop(nullptr);
    quicsrt_subscriber_stop(nullptr);

    quicsrt_publisher_t* pub = quicsrt_publisher_create();
    Check([&] { return pub != nullptr; }, "publisher_create");
    Check([&] { return quicsrt_publisher_start(pub, "127.0.0.1", 0, nullptr, nullptr, 0, 0) == QUICSRT_EINVAL; },
          "start with NULL stream is EINVAL");
    Check([&] { return quicsrt_publisher_write(pub, "x", 1, 10) == QUICSRT_WRITE_NOT_STARTED; },
          "write before start is NOT_STARTED");
    // The façade checks not-started before the args; NULL/zero-length
    // surface as INVALID only on a live session.
    Check(
        [&] {
            char stats[64];
            return quicsrt_publisher_stats(pub, stats, sizeof stats) == QUICSRT_ESTATE &&
                   quicsrt_publisher_stats(pub, nullptr, 100) == QUICSRT_EINVAL;
        },
        "stats before start is ESTATE; NULL buf is EINVAL");
    Check([&] { return quicsrt_publisher_end(pub, 10) != QUICSRT_OK; }, "end before start fails");
    Check(
        [&] {
            return quicsrt_publisher_start(pub, "127.0.0.1", 4433, "", "zz", 0, 1) == QUICSRT_ECONFIG &&
                   Contains(quicsrt_publisher_last_error(pub), "stream");
        },
        "invalid config (empty stream / bad hash) is ECONFIG");
    quicsrt_publisher_destroy(pub); // never started; clean destroy

    quicsrt_subscriber_t* sub = quicsrt_subscriber_create();
    Check([&] { return sub != nullptr; }, "subscriber_create");
    Check([&] { return quicsrt_subscriber_read(sub, nullptr, 1, 10) == QUICSRT_READ_NOT_STARTED; },
          "read NULL buf before start is NOT_STARTED (façade checks state first)");
    Check([&] { return quicsrt_subscriber_read(sub, nullptr, 0, 10) == QUICSRT_READ_NOT_STARTED; },
          "read before start is NOT_STARTED (NULL buf checked first)");
    char big[2048];
    Check([&] { return quicsrt_subscriber_read(sub, big, sizeof big, 10) == QUICSRT_READ_NOT_STARTED; },
          "read before start is NOT_STARTED");
    Check(
        [&] {
            char stats[64];
            return quicsrt_subscriber_stats(sub, stats, sizeof stats) == QUICSRT_ESTATE;
        },
        "subscriber stats before start is ESTATE");
    Check([&] { return std::strlen(quicsrt_subscriber_last_error(sub)) == 0; }, "subscriber last_error empty so far");
    quicsrt_subscriber_destroy(sub);
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
