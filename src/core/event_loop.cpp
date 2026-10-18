#include "core/event_loop.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace quicsrt
{

namespace
{

constexpr size_t kMaxEventsPerWait = 16;

// epoll_ctl wrapper that always executes (never inside assert(), which
// vanishes in Release) and treats failure as fatal: short of ENOMEM it can
// only happen through programmer error (bad fd, double registration), and
// the loop has no error channel.
void EpollCtl(int epfd, int op, int fd, std::uint32_t events)
{
    struct epoll_event ev{};
    ev.events  = events;
    ev.data.fd = fd;
    if (0 != ::epoll_ctl(epfd, op, fd, &ev))
    {
        std::fprintf(stderr, "quicsrt: epoll_ctl(op=%d, fd=%d) failed: %s\n", op, fd, std::strerror(errno));
        std::abort();
    }
}

} // namespace

EventLoop::EventLoop()
{
    epfd_    = ::epoll_create1(EPOLL_CLOEXEC);
    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(epfd_ >= 0 && wake_fd_ >= 0);
    EpollCtl(epfd_, EPOLL_CTL_ADD, wake_fd_, EPOLLIN);
}

EventLoop::~EventLoop()
{
    Stop();
    if (thread_.joinable())
    {
        thread_.join();
    }
    if (epfd_ >= 0)
    {
        ::close(epfd_);
    }
    if (wake_fd_ >= 0)
    {
        ::close(wake_fd_);
    }
}

void EventLoop::Start()
{
    assert(!thread_.joinable());
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { Run(); });
}

void EventLoop::Stop()
{
    {
        // Flip under post_mutex_ so Post's accept test is serialized with
        // the stop: a Post either enqueues before the flip (and the loop's
        // shutdown drain runs it) or observes the flip and rejects.
        const std::lock_guard<std::mutex> lock(post_mutex_);
        if (!running_.exchange(false, std::memory_order_acq_rel))
        {
            return;
        }
    }
    const std::uint64_t one = 1;
    (void)::write(wake_fd_, &one, sizeof one);
    // Called from a callback, the loop thread exits on its own and the
    // destructor performs the join.
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id())
    {
        thread_.join();
    }
}

bool EventLoop::Post(std::function<void()> fn)
{
    {
        const std::lock_guard<std::mutex> lock(post_mutex_);
        if (!running_.load(std::memory_order_acquire))
        {
            return false;
        }
        posted_.push_back(std::move(fn));
    }
    const std::uint64_t one = 1;
    (void)::write(wake_fd_, &one, sizeof one);
    return true;
}

bool EventLoop::OnLoopThread() const
{
    return thread_.joinable() && thread_.get_id() == std::this_thread::get_id();
}

bool EventLoop::IsRunning() const
{
    return running_.load(std::memory_order_acquire);
}

void EventLoop::AddFd(int fd, std::uint32_t events, FdCallback cb)
{
    AssertLoopThread();
    assert(fd >= 0 && fd != wake_fd_);
    assert(cb != nullptr);
    assert(fds_.find(fd) == fds_.end());
    EpollCtl(epfd_, EPOLL_CTL_ADD, fd, events);
    fds_.emplace(fd, std::move(cb));
}

void EventLoop::ModFd(int fd, std::uint32_t events)
{
    AssertLoopThread();
    assert(fds_.find(fd) != fds_.end());
    EpollCtl(epfd_, EPOLL_CTL_MOD, fd, events);
}

void EventLoop::DelFd(int fd)
{
    AssertLoopThread();
    assert(fds_.find(fd) != fds_.end());
    EpollCtl(epfd_, EPOLL_CTL_DEL, fd, 0);
    fds_.erase(fd);
}

void EventLoop::SetEngineHost(EngineHost* host)
{
    AssertLoopThread();
    engine_host_ = host;
}

EventLoop::TimerId EventLoop::SetTimer(std::chrono::milliseconds delay, bool repeating, TimerCallback cb)
{
    AssertLoopThread();
    assert(cb != nullptr);
    const auto    clamped = std::max(delay, std::chrono::milliseconds{});
    const TimerId id      = next_timer_id_++;
    TimerEntry    entry;
    entry.id        = id;
    entry.deadline  = std::chrono::steady_clock::now() + clamped;
    entry.delay     = clamped;
    entry.repeating = repeating;
    entry.cb        = std::move(cb);
    timers_.push_back(std::move(entry));
    return id;
}

void EventLoop::CancelTimer(TimerId id)
{
    AssertLoopThread();
    const auto it = std::ranges::find_if(timers_, [id](const TimerEntry& entry) { return entry.id == id; });
    if (it != timers_.end())
    {
        timers_.erase(it);
    }
}

void EventLoop::Run()
{
    std::vector<struct epoll_event> events(kMaxEventsPerWait);
    for (;;)
    {
        const int timeout_ms = SleepTimeoutMs();
        const int n          = ::epoll_wait(epfd_, events.data(), static_cast<int>(events.size()), timeout_ms);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            std::fprintf(stderr, "quicsrt: epoll_wait failed: %s\n", std::strerror(errno));
            break;
        }
        for (int i = 0; i < n; ++i)
        {
            HandleReady(events[static_cast<size_t>(i)].data.fd, events[static_cast<size_t>(i)].events);
        }
        if (!running_.load(std::memory_order_acquire))
        {
            break;
        }
        RunTimers();
        if (!running_.load(std::memory_order_acquire))
        {
            break;
        }
        PumpEngine();
        if (!running_.load(std::memory_order_acquire))
        {
            break;
        }
    }
    // Shutdown drain: every job Post accepted was enqueued before running_
    // flipped false (the flip and Post's accept test share post_mutex_), so
    // a final RunPosted runs them all before the thread exits — accepted
    // means the job will run, rejected means it never will.
    RunPosted();
}

int EventLoop::SleepTimeoutMs()
{
    int timeout_ms = -1;

    if (engine_host_ != nullptr)
    {
        std::chrono::microseconds diff{};
        if (engine_host_->NextTick(diff))
        {
            if (diff <= std::chrono::microseconds{})
            {
                return 0;
            }
            timeout_ms = static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(diff).count());
        }
    }

    const auto now = std::chrono::steady_clock::now();
    for (const TimerEntry& entry : timers_)
    {
        if (entry.deadline <= now)
        {
            return 0;
        }
        const int candidate =
            static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(entry.deadline - now).count());
        if (timeout_ms < 0 || candidate < timeout_ms)
        {
            timeout_ms = candidate;
        }
    }
    return timeout_ms;
}

void EventLoop::HandleReady(int fd, std::uint32_t events)
{
    if (fd == wake_fd_)
    {
        std::uint64_t counter = 0;
        (void)::read(wake_fd_, &counter, sizeof counter);
        RunPosted();
        return;
    }
    const auto it = fds_.find(fd);
    if (it == fds_.end())
    {
        return;
    }
    // Copy: the callback may DelFd this fd and re-register the same number.
    const FdCallback cb = it->second;
    cb(fd, events);
}

void EventLoop::RunPosted()
{
    std::vector<std::function<void()>> jobs;
    {
        const std::lock_guard<std::mutex> lock(post_mutex_);
        jobs.swap(posted_);
    }
    for (const auto& job : jobs)
    {
        job();
    }
}

void EventLoop::RunTimers()
{
    const auto now = std::chrono::steady_clock::now();

    std::vector<TimerId> due;
    for (const TimerEntry& entry : timers_)
    {
        if (entry.deadline <= now)
        {
            due.push_back(entry.id);
        }
    }

    for (const TimerId id : due)
    {
        auto it = std::ranges::find_if(timers_, [id](const TimerEntry& entry) { return entry.id == id; });
        if (it == timers_.end())
        {
            continue; // cancelled by an earlier callback in this batch
        }
        // Copy the callback: it may set timers and grow (relocate) the vector.
        const TimerCallback cb = it->cb;
        if (it->repeating)
        {
            it->deadline = now + it->delay;
        }
        else
        {
            timers_.erase(it);
        }
        cb();
    }
}

void EventLoop::PumpEngine()
{
    if (engine_host_ != nullptr)
    {
        engine_host_->Pump();
    }
}

void EventLoop::AssertLoopThread() const // NOLINT(readability-convert-member-functions-to-static)
{
    assert(!thread_.joinable() || OnLoopThread());
}

} // namespace quicsrt
