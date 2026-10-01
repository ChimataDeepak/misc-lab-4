#include "link_monitor.hpp"

#include <linux/rtnetlink.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>

namespace mlab {

LinkMonitor::LinkMonitor(EventLoop& loop, Callback cb) : loop_(loop), cb_(std::move(cb)) {
    // Subscribe FIRST, then dump: anything that changes between the two shows up
    // as an event, so we cannot miss a transition (classic snapshot+stream race).
    fd_ = rtnl_open(RTMGRP_LINK, 1);
    if (fd_ < 0) throw std::system_error(-fd_, std::generic_category(), "rtnl_open");
    rtnl_dump_links(
        [](const rtnl_link* l, void* self) { static_cast<LinkMonitor*>(self)->deliver(*l); }, this);
    loop_.add_fd(fd_, EPOLLIN, [this](uint32_t) { on_readable(); });
}

LinkMonitor::~LinkMonitor() {
    loop_.del_fd(fd_);
    close(fd_);
}

void LinkMonitor::deliver(const rtnl_link& l) {
    auto it = last_flags_.find(l.ifindex);
    bool known = it != last_flags_.end();
    unsigned prev = known ? it->second : 0;
    if (l.is_new) last_flags_[l.ifindex] = l.flags;
    else last_flags_.erase(l.ifindex);
    cb_(l, prev, known);
}

void LinkMonitor::on_readable() {
    for (;;) {
        int rc = rtnl_recv(fd_, [](const rtnl_link* l, void* self) {
            static_cast<LinkMonitor*>(self)->deliver(*l);
        }, this);
        if (rc == -EAGAIN || rc == -EWOULDBLOCK) return;
        if (rc == -ENOBUFS) {
            // Socket buffer overflowed: the kernel DROPPED events, so last_flags_
            // may be stale. Recover by re-dumping the full link table; deliver()
            // diffs each link against last_flags_ and reports any missed change.
            rtnl_dump_links(
                [](const rtnl_link* l, void* self) { static_cast<LinkMonitor*>(self)->deliver(*l); },
                this);
            continue;
        }
        if (rc < 0) return;
    }
}

}  // namespace mlab
