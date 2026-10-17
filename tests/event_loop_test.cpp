// EventLoop units: cross-thread Post (accept/reject contract and a
// post-vs-Stop race), fd registration and wakeup, one-shot and repeating
// timers with cancellation, EngineHost tick driving, and Stop from inside a
// callback. Plain checks (not assert) so Release stays meaningful; every
// wait is bounded so a broken loop fails instead of hanging.

#include "core/event_loop.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>

namespace
{

using websrt::EngineHost;
using websrt::EventLoop;

constexpr std::chrono::milliseconds kWaitBudget{2000};

std::atomic<bool> g_ok{true};

void Fail(const char* what)
{
    g_ok = false;
    std::fprintf(stderr, "event_loop_test: FAIL: %s\n", what);
}

bool Check(bool cond, const char* what)
{
    if (!cond)
    {
        Fail(what);
    }
    return cond;
}

bool WaitFor(const std::function<bool()>& pred)
{
    const auto deadline = std::chrono::steady_clock::now() + kWaitBudget;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return pred();
}

struct FakeEngineHost final : public EngineHost
{
    std::atomic<bool>    scheduled{false};
    std::atomic<int64_t> diff_us{0};
    std::atomic<int>     pumps{0};

    bool NextTick(std::chrono::microseconds& diff) override
    {
        diff = std::chrono::microseconds{diff_us.load()};
        return scheduled.load();
    }

    void Pump() override { pumps.fetch_add(1); }
};

bool TestPostRunsOnLoopThread()
{
    EventLoop loop;
    loop.Start();

    std::atomic<bool>            ran{false};
    std::atomic<std::thread::id> loop_thread{};
    Check(loop.Post(
              [&]
              {
                  loop_thread = std::this_thread::get_id();
                  ran         = true;
              }),
          "post while running is accepted");
    if (!Check(WaitFor([&] { return ran.load(); }), "posted functor ran"))
    {
        return false;
    }
    if (!Check(loop_thread.load() != std::this_thread::get_id(), "posted functor ran on the loop thread"))
    {
        return false;
    }
    if (!Check(!loop.OnLoopThread(), "OnLoopThread is false off the loop thread"))
    {
        return false;
    }

    loop.Stop();
    return true;
}

bool TestFdRegistrationAndWake()
{
    EventLoop loop;

    const int efd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (!Check(efd >= 0, "eventfd created"))
    {
        return false;
    }

    std::atomic<bool> fired{false};
    loop.AddFd(efd,
               EPOLLIN,
               [&fired](int fd, std::uint32_t events)
               {
                   std::uint64_t counter = 0;
                   (void)::read(fd, &counter, sizeof counter);
                   if (events & EPOLLIN)
                   {
                       fired = true;
                   }
               });

    loop.Start();

    const std::uint64_t one = 1;
    if (!Check(::write(efd, &one, sizeof one) == static_cast<ssize_t>(sizeof one), "eventfd write"))
    {
        return false;
    }
    if (!Check(WaitFor([&] { return fired.load(); }), "fd callback fired on wakeup"))
    {
        return false;
    }

    Check(loop.Post([&] { loop.DelFd(efd); }), "del-fd post accepted");
    loop.Stop();
    ::close(efd);
    return true;
}

bool TestTimers()
{
    EventLoop loop;

    std::atomic<int>   one_shot_fires{0};
    std::atomic<int>   repeating_fires{0};
    std::atomic<bool>  cancelled_one_shot_fired{false};
    EventLoop::TimerId self_cancelling = websrt::EventLoop::kInvalidTimerId;
    EventLoop::TimerId never_fires     = websrt::EventLoop::kInvalidTimerId;

    loop.SetTimer(std::chrono::milliseconds{20}, false, [&] { one_shot_fires.fetch_add(1); });
    self_cancelling = loop.SetTimer(std::chrono::milliseconds{15},
                                    true,
                                    [&]
                                    {
                                        if (repeating_fires.fetch_add(1) + 1 == 3)
                                        {
                                            loop.CancelTimer(self_cancelling);
                                        }
                                    });
    // Cancelled before it can ever become due: must not fire.
    never_fires = loop.SetTimer(std::chrono::milliseconds{400}, false, [&] { cancelled_one_shot_fired = true; });
    loop.SetTimer(std::chrono::milliseconds{20}, false, [&] { loop.CancelTimer(never_fires); });

    loop.Start();

    if (!Check(WaitFor([&] { return one_shot_fires.load() == 1; }), "one-shot timer fired once"))
    {
        return false;
    }
    if (!Check(WaitFor([&] { return repeating_fires.load() >= 3; }), "repeating timer reached three fires"))
    {
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    if (!Check(one_shot_fires.load() == 1, "one-shot timer did not refire"))
    {
        return false;
    }
    if (!Check(repeating_fires.load() == 3, "self-cancelled repeating timer stopped at three fires"))
    {
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{350});
    if (!Check(!cancelled_one_shot_fired.load(), "cancelled timer never fired"))
    {
        return false;
    }

    loop.Stop();
    return true;
}

bool TestEngineHostDriving()
{
    EventLoop      loop;
    FakeEngineHost host;

    loop.SetEngineHost(&host);
    loop.Start();

    // No tick scheduled: the loop sleeps; Pump only runs when woken.
    Check(loop.Post([] {}), "wake-only post accepted");
    if (!Check(WaitFor([&] { return host.pumps.load() >= 1; }), "Pump ran on a wake"))
    {
        return false;
    }

    // Overdue tick: the loop spins without sleeping and Pump runs hot. The
    // state change rides a Post, the only cross-thread way an engine host's
    // scheduling can change (the loop samples NextTick just before sleeping).
    Check(loop.Post([&] { host.scheduled.store(true); }), "schedule post accepted");
    if (!Check(WaitFor([&] { return host.pumps.load() >= 20; }), "overdue tick keeps Pumping"))
    {
        return false;
    }

    // Tick far in the future: no iterations, no Pump calls. The grace sleep
    // lets the loop pick the new diff up before sampling.
    Check(loop.Post([&] { host.diff_us.store(500000); }), "diff post accepted");
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    const int before = host.pumps.load();
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    if (!Check(host.pumps.load() == before, "future tick puts the loop to sleep"))
    {
        return false;
    }

    Check(loop.Post([&] { host.scheduled.store(false); }), "unschedule post accepted");
    loop.Stop();
    return true;
}

bool TestStopFromCallback()
{
    EventLoop loop;
    loop.Start();

    std::atomic<bool> after_stop_ran{false};
    Check(loop.Post([&] { loop.Stop(); }), "stop post accepted");
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    Check(!loop.Post([&] { after_stop_ran = true; }), "post after Stop is rejected");
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    const bool dropped = Check(!after_stop_ran.load(), "functors posted after Stop are dropped");

    // The loop thread exited by itself; the destructor must join it without
    // hanging (reaching the end of this function proves it).
    return dropped;
}

bool TestPostReturnContract()
{
    // Never started: the loop is not running, the post is rejected.
    {
        EventLoop loop;
        if (!Check(!loop.Post([] {}), "post to a never-started loop is rejected"))
        {
            return false;
        }
    }

    // Running: accepted, and the job executes on the loop thread.
    {
        EventLoop         loop;
        std::atomic<bool> ran{false};
        loop.Start();
        if (!Check(loop.Post([&] { ran = true; }), "post while running is accepted"))
        {
            return false;
        }
        if (!Check(WaitFor([&] { return ran.load(); }), "accepted post executed"))
        {
            return false;
        }
        loop.Stop();
        if (!Check(!loop.Post([] {}), "post after Stop is rejected"))
        {
            return false;
        }
    }
    return true;
}

bool TestPostStopRace()
{
    // One thread Stops the loop while another posts in a tight loop. Every
    // Post must return a definite verdict and the verdicts must account for
    // every execution: an accepted post runs before the loop thread exits
    // (shutdown drain), a rejected one never runs — so executed must equal
    // accepted, with neither lost nor duplicated jobs. Post never blocks
    // indefinitely: it holds post_mutex_ only for a push_back and does one
    // nonblocking eventfd write, so a concurrent Stop cannot wedge it.
    constexpr int kRounds        = 200;
    constexpr int kPostsPerRound = 25;
    for (int round = 0; round < kRounds; ++round)
    {
        EventLoop loop;
        loop.Start();

        std::atomic<int> executed{0};
        int              accepted = 0;

        std::thread poster(
            [&]
            {
                for (int i = 0; i < kPostsPerRound; ++i)
                {
                    if (loop.Post([&] { executed.fetch_add(1); }))
                    {
                        ++accepted;
                    }
                }
            });
        std::thread stopper([&] { loop.Stop(); });

        poster.join();
        stopper.join(); // Stop joined the loop thread: executed is final

        if (!Check(executed.load() == accepted, "race: executed posts equal accepted posts"))
        {
            return false;
        }
    }
    return true;
}

} // namespace

int main()
{
    struct Section
    {
        const char* name;
        bool (*fn)();
    };

    const Section sections[] = {
        {.name = "post", .fn = TestPostRunsOnLoopThread},
        {.name = "post-contract", .fn = TestPostReturnContract},
        {.name = "post-stop-race", .fn = TestPostStopRace},
        {.name = "fd", .fn = TestFdRegistrationAndWake},
        {.name = "timers", .fn = TestTimers},
        {.name = "engine-host", .fn = TestEngineHostDriving},
        {.name = "stop-from-callback", .fn = TestStopFromCallback},
    };

    bool ok = true;
    for (const Section& section : sections)
    {
        ok = section.fn() && ok;
    }
    if (!ok)
    {
        return 1;
    }
    std::printf("event_loop_test: all sections passed\n");
    return 0;
}
