#include "event_loop.hpp"

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace mlab {

EventLoop::EventLoop() : ep_(epoll_create1(EPOLL_CLOEXEC)) {
    if (ep_ < 0) throw std::system_error(errno, std::generic_category(), "epoll_create1");
}

EventLoop::~EventLoop() { close(ep_); }

void EventLoop::add_fd(int fd, uint32_t events, FdHandler h) {
    epoll_event ev{};
    ev.events = events;
    uint32_t gen = next_gen_++;
    ev.data.u64 = (static_cast<uint64_t>(gen) << 32) | static_cast<uint32_t>(fd);
    if (epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev) < 0)
        throw std::system_error(errno, std::generic_category(), "epoll_ctl ADD");
    handlers_[fd] = Reg{gen, std::make_shared<FdHandler>(std::move(h))};
}

void EventLoop::mod_fd(int fd, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    auto it = handlers_.find(fd);
    if (it == handlers_.end()) return;
    ev.data.u64 = (static_cast<uint64_t>(it->second.gen) << 32) | static_cast<uint32_t>(fd);
    epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &ev);
}

void EventLoop::del_fd(int fd) {
    epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr);
    handlers_.erase(fd);
}

EventLoop::TimerId EventLoop::add_timer(std::chrono::milliseconds after, std::function<void()> fn) {
    TimerId id = next_timer_++;
    timers_[id] = Timer{Clock::now() + after, std::move(fn)};
    return id;
}

void EventLoop::cancel_timer(TimerId id) { timers_.erase(id); }

void EventLoop::run_once(std::chrono::milliseconds max_wait) {
    // Sleep no longer than the nearest timer deadline.
    auto now = Clock::now();
    auto wait = max_wait;
    for (const auto& [id, t] : timers_) {
        // ceil, not duration_cast (truncation): a timer 0.4ms away must give a
        // 1ms wait, not 0ms, or the loop busy-spins until the deadline.
        auto left = std::chrono::ceil<std::chrono::milliseconds>(t.deadline - now);
        if (left < wait) wait = left;
    }
    if (wait.count() < 0) wait = std::chrono::milliseconds(0);

    epoll_event evs[16];
    int n = epoll_wait(ep_, evs, 16, static_cast<int>(wait.count()));
    for (int i = 0; i < n; i++) {
        int fd = static_cast<int>(evs[i].data.u64 & 0xffffffffu);
        uint32_t gen = static_cast<uint32_t>(evs[i].data.u64 >> 32);
        auto it = handlers_.find(fd);
        if (it == handlers_.end()) continue;    // removed by an earlier handler this round
        if (it->second.gen != gen) continue;    // fd number reused: stale event
        auto keep_alive = it->second.h;
        (*keep_alive)(evs[i].events);
    }

    // Collect due timer IDS (not callbacks), then re-look each one up before
    // running it: an earlier callback may have cancelled it (or destroyed the
    // object its lambda captures), and cancel_timer must still win.
    now = Clock::now();
    std::vector<TimerId> due;
    for (const auto& [id, t] : timers_)
        if (t.deadline <= now) due.push_back(id);
    for (TimerId id : due) {
        auto it = timers_.find(id);
        if (it == timers_.end()) continue;  // cancelled by an earlier callback
        auto fn = std::move(it->second.fn);
        timers_.erase(it);
        fn();
    }
}

bool EventLoop::run_until(const std::function<bool()>& pred, std::chrono::milliseconds limit) {
    auto end = Clock::now() + limit;
    while (!pred()) {
        auto left = std::chrono::ceil<std::chrono::milliseconds>(end - Clock::now());
        if (left.count() <= 0) break;
        run_once(left);
    }
    return pred();
}

}  // namespace mlab
