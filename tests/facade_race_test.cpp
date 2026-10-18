// facade_race_test: concurrent Stop() safety of the two public façades.
//
// The headers document Stop as safe to call from another thread, and the
// destructor calls Stop too - so a watchdog thread's Stop can race the
// controlling thread's Stop or a Start failure path's Teardown. The
// guards must let exactly ONE caller run the teardown sequence; a second
// run would call srt_cleanup() twice for one srt_startup(),
// over-decrementing the process-global srt refcount. An over-decremented
// count reaches zero while OTHER sessions are still live, and srt's
// cleanup-at-zero (stopGarbageCollector + cleanupAllSockets) then
// destroys their sockets mid-session - the FFmpeg several-handles-in-one
// process scenario.
//
// (a) many rounds of a fresh façade: Start runs on its own thread and,
//     once the session is in flight (ever_started_, teardown pending),
//     watchdog threads hammer Stop() while Start fails against a dead
//     port - its error path runs the full teardown sequence racing the
//     hammering and the controlling thread's post-failure Stop. The
//     destructor's share of the race is exactly that Stop call (racing
//     member calls against destruction itself is UB by C++ rules).
// (b) a sentinel façade of the same family stays in flight across all
//     rounds: a double cleanup anywhere in the phase yanks its sockets,
//     so its dead-port Start must fail by TIMEOUT (kStartTimedOut at its
//     connect_timeout), never early.
// (c) trailing canary: fresh Subscriber and Publisher sessions, driven
//     concurrently in this same process, must still fail typed and tear
//     down cleanly.
// (d) publisher Write vs Stop on the offline states: the blocked-send
//     case needs a connected session, but every Write state reachable
//     without a gateway - never-started, start-failed (Stop hammering
//     through the failure teardown), already-stopped - must return its
//     typed result promptly: never consuming the Write budget, never a
//     positive accept.
//
// Reproduction note: the double-run window of the old check-then-act
// guards is a bare load/store pair - nanoseconds wide, found by code
// review, not reachable by black-box hammering (a 600-round escalation
// never tripped it). This test is the standing trap: hammering catches
// the crash modes, the sentinel and the canary catch the refcount
// corruption should a regression ever widen the window.
//
// Offline: the dead port (nothing listens on UDP 1) needs no gateway, so
// this always runs.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "quicsrt/publisher.hpp"
#include "quicsrt/subscriber.hpp"

namespace
{

// Watchdogs per round; several concurrent Stop() callers make the old
// check-then-act guards collide within a round's few hundred ms.
constexpr int kWatchdogs    = 3;
constexpr int kRaceRounds   = 25;
constexpr int kCanaryRounds = 3;
// Head start before the watchdogs spawn: Start must be past
// ever_started_ and waiting in-flight, or the first Stop would consume
// the entry guard as a pre-Start no-op and the round would not race.
constexpr auto kStartHeadStart = std::chrono::milliseconds(50);
// The sentinel must outlive all of its family's rounds (each well under
// ~300 ms) so any double cleanup lands while it is live.
constexpr int kSentinelTimeoutMs = 6000;
// Write budget for (d): large enough that a state check consuming it
// would be an obvious hang. The prompt ceiling is generous - the offline
// state checks are flag reads, so anything near it is a regression.
constexpr int  kWriteBudgetMs = 10000;
constexpr auto kWritePromptMs = std::chrono::milliseconds(2000);

std::atomic<bool> g_ok{true};
std::atomic<int>  g_checks{0};

void Fail(const std::string& what)
{
    g_ok = false;
    std::fprintf(stderr, "facade_race_test: FAIL: %s\n", what.c_str());
}

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

// Spawns the watchdogs: they hammer Stop() until halt - the documented
// cross-thread caller the guards must serialize against.
template <typename Facade>
std::vector<std::thread> SpawnWatchdogs(Facade* facade, std::atomic<bool>& halt)
{
    std::vector<std::thread> watchdogs;
    watchdogs.reserve(kWatchdogs);
    for (int i = 0; i < kWatchdogs; ++i)
    {
        watchdogs.emplace_back(
            [facade, &halt]
            {
                while (!halt.load())
                {
                    facade->Stop();
                    std::this_thread::yield();
                }
            });
    }
    return watchdogs;
}

void HaltAndJoin(std::atomic<bool>& halt, std::vector<std::thread>* watchdogs)
{
    halt.store(true);
    for (auto& watchdog : *watchdogs)
    {
        watchdog.join();
    }
}

// One race round: Start runs on its own thread into the dead port; once
// the session is in flight, the watchdogs hammer Stop() while Start's
// failure path runs RequestClose/WaitClosed/Teardown - the full teardown
// sequence with the srt library-reference drop. Returns Start's status.
template <typename Facade, typename Config>
int RaceStopAgainstStartFailure(const Config& config)
{
    auto              facade = std::make_unique<Facade>();
    std::atomic<bool> halt{false};
    std::atomic<int>  status{-1000};

    std::thread start_thread([&] { status.store(facade->Start(config)); });
    std::this_thread::sleep_for(kStartHeadStart);
    auto watchdogs = SpawnWatchdogs(facade.get(), halt);
    start_thread.join();

    // Keep hammering briefly past Start's return: the teardown that just
    // ran must turn every later Stop into a prompt no-op.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    HaltAndJoin(halt, &watchdogs);

    facade->Stop(); // idempotent no-op after the race
    facade.reset(); // destructor Stop: also a no-op
    return status.load();
}

// ---- (a) hammering rounds + (b) live sentinel, per façade ----

quicsrt::SubscriberConfig DeadPortSubscriberConfig(int connect_timeout_ms)
{
    quicsrt::SubscriberConfig cfg;
    cfg.host               = "127.0.0.1";
    cfg.port               = 1; // nothing listens on UDP 1
    cfg.stream_name        = "facade_race";
    cfg.cert_sha256_hex    = ""; // no pinning needed against a dead port
    cfg.connect_timeout_ms = connect_timeout_ms;
    return cfg;
}

quicsrt::PublisherConfig DeadPortPublisherConfig(int connect_timeout_ms)
{
    quicsrt::PublisherConfig cfg;
    cfg.host               = "127.0.0.1";
    cfg.port               = 1;
    cfg.stream_name        = "facade_race";
    cfg.cert_sha256_hex    = "";
    cfg.connect_timeout_ms = connect_timeout_ms;
    return cfg;
}

template <typename Facade, typename Config, typename SentinelConfig>
void TestFacadeRace(const char*           family,
                    int                   expected_early_status,
                    int                   expected_timeout_status,
                    const Config&         race_config,
                    const SentinelConfig& sentinel_config)
{
    // Sentinel: in flight across every round of the phase. Its dead-port
    // Start can only end early if srt's cleanup-at-zero destroyed its
    // sockets (a double cleanup in some round) - the global-refcount
    // corruption this test exists to catch.
    std::thread sentinel_thread(
        [&]
        {
            Facade     sentinel;
            const auto t0      = std::chrono::steady_clock::now();
            const int  status  = sentinel.Start(sentinel_config);
            const auto elapsed = std::chrono::steady_clock::now() - t0;
            Check([&] { return status == expected_timeout_status; },
                  std::string(family) + " sentinel must fail by timeout, not early (got " + std::to_string(status) +
                      ")");
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
            Check([&] { return ms.count() >= kSentinelTimeoutMs - 500; },
                  std::string(family) + " sentinel must run its full connect timeout (took " +
                      std::to_string(ms.count()) + " ms)");
        });

    for (int i = 0; i < kRaceRounds; ++i)
    {
        const int status = RaceStopAgainstStartFailure<Facade>(race_config);
        Check([&] { return status == expected_timeout_status || status == expected_early_status; },
              std::string(family) + " raced Start must fail typed (got " + std::to_string(status) + ")");
    }

    sentinel_thread.join();
    std::printf("%s race: %d rounds + sentinel clean\n", family, kRaceRounds);
}

// ---- (c) canary: fresh sessions in this same process ----

// Even a self-healed refcount must leave no residue: fresh concurrent
// Subscriber and Publisher sessions still start (typed, bounded failure)
// and tear down cleanly.
void TestCanaryCleanSessions()
{
    const auto t0 = std::chrono::steady_clock::now();

    std::thread subscriber_thread(
        []
        {
            for (int i = 0; i < kCanaryRounds; ++i)
            {
                quicsrt::Subscriber sub;
                const int          status = sub.Start(DeadPortSubscriberConfig(300));
                Check([&] { return status == quicsrt::kStartTimedOut || status == quicsrt::kStartConnectFailed; },
                      "canary Subscriber Start must still fail typed (got " + std::to_string(status) + ")");
                char buf[188];
                Check([&] { return sub.Read(buf, sizeof buf, 100) == quicsrt::kReadClosed; },
                      "canary Subscriber Read must be terminal");
                sub.Stop();
            }
        });
    std::thread publisher_thread(
        []
        {
            for (int i = 0; i < kCanaryRounds; ++i)
            {
                quicsrt::Publisher pub;
                const int         status = pub.Start(DeadPortPublisherConfig(300));
                Check([&] { return status == quicsrt::kPublishTimedOut || status == quicsrt::kPublishConnectFailed; },
                      "canary Publisher Start must still fail typed (got " + std::to_string(status) + ")");
                Check([&] { return pub.Write("x", 1, 100) == quicsrt::kWriteClosed; },
                      "canary Publisher Write must be terminal");
                pub.Stop();
            }
        });

    subscriber_thread.join();
    publisher_thread.join();

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
    Check([&] { return ms.count() < 30000; }, "canary must stay bounded (took " + std::to_string(ms.count()) + " ms)");
    std::printf(
        "canary: %d concurrent rounds per façade in %lld ms\n", kCanaryRounds, static_cast<long long>(ms.count()));
}

// ---- (d) publisher Write vs Stop on the offline states ----

// Write's blocking domain proper - srt_sendmsg2 parked on a full send
// buffer - needs a connected session (not covered offline). Offline a
// Write can only sit in the façade's state checks, and each must return
// its typed result promptly while Stop is concurrent or already effected:
// a budget-consuming wait or a positive accept there would mean the state
// checks consult something that outlives Stop's teardown.
void TestPublisherWriteStates()
{
    char buf[1128];

    // (d1) never-started: watchdog Stop hammering around Write must leave
    // the kWriteNotStarted result and its promptness intact.
    {
        quicsrt::Publisher pub;
        std::atomic<bool> halt{false};
        auto              watchdogs = SpawnWatchdogs(&pub, halt);
        const auto        t0        = std::chrono::steady_clock::now();
        Check([&] { return pub.Write(buf, sizeof buf, kWriteBudgetMs) == quicsrt::kWriteNotStarted; },
              "never-started Write must stay kWriteNotStarted under concurrent Stop");
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        Check([&] { return ms < kWritePromptMs; },
              "never-started Write must not consume its budget (took " + std::to_string(ms.count()) + " ms)");
        HaltAndJoin(halt, &watchdogs);
    }

    // (d2) start-failed: one race round (Start into the dead port while
    // the watchdogs hammer its failure-path teardown), then - still under
    // the hammering - Write must be terminal kWriteClosed, prompt.
    {
        auto              facade = std::make_unique<quicsrt::Publisher>();
        std::atomic<bool> halt{false};
        std::atomic<int>  status{-1000};
        std::thread start_thread([&facade, &status] { status.store(facade->Start(DeadPortPublisherConfig(500))); });
        std::this_thread::sleep_for(kStartHeadStart);
        auto watchdogs = SpawnWatchdogs(facade.get(), halt);
        start_thread.join();
        Check([&]
              { return status.load() == quicsrt::kPublishTimedOut || status.load() == quicsrt::kPublishConnectFailed; },
              "write-state round Start must fail typed (got " + std::to_string(status.load()) + ")");
        const auto t0 = std::chrono::steady_clock::now();
        Check([&] { return facade->Write(buf, sizeof buf, kWriteBudgetMs) == quicsrt::kWriteClosed; },
              "start-failed Write must be kWriteClosed while Stop hammers");
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        Check([&] { return ms < kWritePromptMs; },
              "start-failed Write must not consume its budget (took " + std::to_string(ms.count()) + " ms)");
        HaltAndJoin(halt, &watchdogs);
        facade->Stop();
        facade.reset();
    }

    // (d3) already-stopped: Stop ran to completion first; a Write with a
    // full budget must still return immediately, terminal.
    {
        quicsrt::Publisher pub;
        const int         status = pub.Start(DeadPortPublisherConfig(300));
        Check([&] { return status == quicsrt::kPublishTimedOut || status == quicsrt::kPublishConnectFailed; },
              "already-stopped round Start must fail typed (got " + std::to_string(status) + ")");
        pub.Stop();
        const auto t0 = std::chrono::steady_clock::now();
        Check([&] { return pub.Write(buf, sizeof buf, kWriteBudgetMs) == quicsrt::kWriteClosed; },
              "post-Stop Write must be kWriteClosed");
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        Check([&] { return ms < kWritePromptMs; },
              "post-Stop Write must not consume its budget (took " + std::to_string(ms.count()) + " ms)");
    }

    std::printf("publisher write states: never-started, start-failed, stopped covered\n");
}

} // namespace

int main()
{
    TestFacadeRace<quicsrt::Subscriber>("subscriber",
                                       quicsrt::kStartConnectFailed,
                                       quicsrt::kStartTimedOut,
                                       DeadPortSubscriberConfig(500),
                                       DeadPortSubscriberConfig(kSentinelTimeoutMs));
    TestFacadeRace<quicsrt::Publisher>("publisher",
                                      quicsrt::kPublishConnectFailed,
                                      quicsrt::kPublishTimedOut,
                                      DeadPortPublisherConfig(500),
                                      DeadPortPublisherConfig(kSentinelTimeoutMs));
    TestCanaryCleanSessions();
    TestPublisherWriteStates();

    if (!g_ok.load())
    {
        return 1;
    }
    std::printf("facade_race_test: ALL CHECKS PASSED (%d)\n", g_checks.load());
    return 0;
}
