// Single-threaded epoll event loop: the one thread every lsquic engine call
// and callback lives on (AGENTS.md "Gotchas"). It multiplexes a wakeup
// eventfd (cross-thread Post), registered file descriptors, one-shot and
// repeating timers, and the advisory tick of an EngineHost.

#ifndef WEBSRT_CORE_EVENT_LOOP_H
#define WEBSRT_CORE_EVENT_LOOP_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace websrt
{

// The lsquic-engine seam of the loop, implemented by the party that owns an
// engine hosted on it (see src/wt/client.cpp). All methods run on the loop
// thread.
//
// The loop samples NextTick only right before it sleeps. A hosted engine is
// passive (it only runs when Pumped), so its scheduling state changes only
// from the loop thread itself - fd input, callbacks, or posted functors -
// never behind the loop's back. Mutations from other threads must ride a
// Post, which wakes the loop.
class EngineHost
{
public:
    virtual ~EngineHost() = default;

    // True while the engine has an advisory tick scheduled; `diff` is the time
    // from now until then, negative when overdue. Maps
    // lsquic_engine_earliest_adv_tick().
    virtual bool NextTick(std::chrono::microseconds& diff) = 0;

    // Gives the engine CPU time: process connections whose advisory tick has
    // arrived and flush packets a previous send could not place. Maps
    // lsquic_engine_process_conns / lsquic_engine_has_unsent_packets +
    // lsquic_engine_send_unsent_packets. Returning from Pump with work still
    // due is expressed by NextTick staying overdue, which keeps the loop
    // spinning without sleeping.
    virtual void Pump() = 0;
};

class EventLoop
{
public:
    using FdCallback    = std::function<void(int fd, std::uint32_t events)>;
    using TimerCallback = std::function<void()>;
    using TimerId       = std::uint64_t;

    static constexpr TimerId kInvalidTimerId = 0;

    // Threading contract:
    // - Start, Stop, and Post are thread-safe.
    // - AddFd, ModFd, DelFd, SetEngineHost, SetTimer, and CancelTimer must run
    //   on the loop thread (from a callback, a posted functor, or while the
    //   loop is not running). From any other thread, go through Post.
    // - Fd, timer, and EngineHost callbacks execute on the loop thread and may
    //   call back into this API, including changing registrations.
    // - Stop may be called from a callback: the loop thread then exits by
    //   itself and the join happens in the destructor.

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&)            = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // Spawns the loop thread; the loop must not already be running.
    void Start();

    // Stops the loop and joins its thread. No-op when not running.
    void Stop();

    // Runs `fn` on the loop thread as soon as the loop is idle. Functors
    // posted while the loop is not running are dropped.
    void Post(std::function<void()> fn);

    [[nodiscard]] bool OnLoopThread() const;

    // Whether the loop thread is running (thread-safe).
    [[nodiscard]] bool IsRunning() const;

    // Registers interest in `events` (level-triggered epoll bits) on `fd`;
    // `cb` fires with the active event bits. The caller owns the fd: DelFd
    // only deregisters, it never closes.
    void AddFd(int fd, std::uint32_t events, FdCallback cb);
    void ModFd(int fd, std::uint32_t events);
    void DelFd(int fd);

    // The engine host driven by this loop; at most one. May be nullptr.
    void SetEngineHost(EngineHost* host);

    // Arms a timer: one-shot fires once after `delay`, repeating fires every
    // `delay` until cancelled. Callbacks run on the loop thread and may set or
    // cancel timers, including their own id.
    TimerId SetTimer(std::chrono::milliseconds delay, bool repeating, TimerCallback cb);
    void    CancelTimer(TimerId id);

private:
    struct TimerEntry
    {
        TimerId                               id = kInvalidTimerId;
        std::chrono::steady_clock::time_point deadline;
        std::chrono::milliseconds             delay{};
        bool                                  repeating = false;
        TimerCallback                         cb;
    };

    void Run();
    // Milliseconds to sleep at most: 0 when engine work or a timer is due,
    // -1 when there is nothing to wait for.
    [[nodiscard]] int SleepTimeoutMs();
    void              HandleReady(int fd, std::uint32_t events);
    void              RunPosted();
    void              RunTimers();
    void              PumpEngine();
    void              AssertLoopThread() const;

    int epfd_    = -1;
    int wake_fd_ = -1;

    std::thread       thread_;
    std::atomic<bool> running_{false};

    std::mutex                         post_mutex_;
    std::vector<std::function<void()>> posted_;

    std::map<int, FdCallback> fds_;
    std::vector<TimerEntry>   timers_;
    EngineHost*               engine_host_   = nullptr;
    TimerId                   next_timer_id_ = 1;
};

} // namespace websrt

#endif // WEBSRT_CORE_EVENT_LOOP_H
