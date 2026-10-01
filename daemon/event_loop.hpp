// event_loop.hpp -- minimal single-threaded epoll reactor + one-shot timers.
//
// Why a reactor: the daemon must wait on many things at once (modem socket,
// netlink socket, request timeouts) without threads. epoll tells us WHICH fd is
// ready; we call that fd's handler; handlers never block (all fds nonblocking).
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>

namespace mlab {

class EventLoop {
public:
    using Clock = std::chrono::steady_clock;
    using FdHandler = std::function<void(uint32_t events)>;
    using TimerId = uint64_t;

    EventLoop();
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void add_fd(int fd, uint32_t events, FdHandler h);
    void mod_fd(int fd, uint32_t events);
    void del_fd(int fd);

    TimerId add_timer(std::chrono::milliseconds after, std::function<void()> fn);
    void cancel_timer(TimerId id);

    // Waits at most max_wait for events, dispatches them, then fires due timers.
    void run_once(std::chrono::milliseconds max_wait);
    // Runs until pred() is true or `limit` elapses. Returns pred().
    bool run_until(const std::function<bool()>& pred, std::chrono::milliseconds limit);

private:
    struct Timer {
        Clock::time_point deadline;
        std::function<void()> fn;
    };
    int ep_;
    // shared_ptr so a handler can safely del_fd() itself mid-dispatch.
    std::map<int, std::shared_ptr<FdHandler>> handlers_;
    std::map<TimerId, Timer> timers_;
    TimerId next_timer_ = 1;
};

}  // namespace mlab
