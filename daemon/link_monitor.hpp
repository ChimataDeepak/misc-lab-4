// link_monitor.hpp -- RTNETLINK link events plugged into the EventLoop.
#pragma once

#include "event_loop.hpp"
#include "rtnl.h"

#include <functional>
#include <map>

namespace mlab {

class LinkMonitor {
public:
    // cb(link, previous_flags, had_previous)
    using Callback = std::function<void(const rtnl_link&, unsigned prev_flags, bool known)>;

    LinkMonitor(EventLoop& loop, Callback cb);
    ~LinkMonitor();
    LinkMonitor(const LinkMonitor&) = delete;
    LinkMonitor& operator=(const LinkMonitor&) = delete;

    int fd() const { return fd_; }

private:
    void on_readable();
    void deliver(const rtnl_link& l);

    EventLoop& loop_;
    Callback cb_;
    int fd_;
    std::map<int, unsigned> last_flags_;  // ifindex -> flags, to report transitions
};

}  // namespace mlab
