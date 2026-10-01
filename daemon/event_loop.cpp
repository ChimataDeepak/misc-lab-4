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
    ev.data.fd = fd;  // look the handler up by fd at dispatch time
    if (epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev) < 0)
        throw std::system_error(errno, std::generic_category(), "epoll_ctl ADD");
    handlers_[fd] = std::make_shared<FdHandler>(std::move(h));
}

void EventLoop::mod_fd(int fd, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
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
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(t.deadline - now);
        if (left < wait) wait = left;
    }
    if (wait.count() < 0) wait = std::chrono::milliseconds(0);

    epoll_event evs[16];
    int n = epoll_wait(ep_, evs, 16, static_cast<int>(wait.count()));
    for (int i = 0; i < n; i++) {
        auto it = handlers_.find(evs[i].data.fd);
        if (it == handlers_.end()) continue;  // removed by an earlier handler this round
        auto keep_alive = it->second;
        (*keep_alive)(evs[i].events);
    }

    // Collect due timers first, then run them: callbacks may add/cancel timers.
    now = Clock::now();
    std::vector<std::function<void()>> due;
    for (auto it = timers_.begin(); it != timers_.end();) {
        if (it->second.deadline <= now) {
            due.push_back(std::move(it->second.fn));
            it = timers_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& fn : due) fn();
}

bool EventLoop::run_until(const std::function<bool()>& pred, std::chrono::milliseconds limit) {
    auto end = Clock::now() + limit;
    while (!pred()) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now());
        if (left.count() <= 0) break;
        run_once(left);
    }
    return pred();
}

}  // namespace mlab
