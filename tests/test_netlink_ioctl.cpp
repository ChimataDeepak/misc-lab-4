// Integration: ioctl() changes a real (dummy) interface and RTNETLINK sees it.
// Needs CAP_NET_ADMIN (docker run --cap-add NET_ADMIN), which run-tests.sh grants.
#include <gtest/gtest.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <vector>

#include "ifctl.h"
#include "rtnl.h"

namespace {
const char* kIf = "mlabwwan0";

struct Events {
    std::vector<rtnl_link> v;
    static void cb(const rtnl_link* l, void* u) {
        if (std::string(l->name) == kIf) static_cast<Events*>(u)->v.push_back(*l);
    }
};

// Reads netlink events until pred(last event) holds or 2s pass.
template <class P>
bool wait_for(int fd, Events& ev, P pred) {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < end) {
        for (auto& l : ev.v)
            if (pred(l)) return true;
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 200) > 0) rtnl_recv(fd, Events::cb, &ev);
    }
    return false;
}

class NetlinkIoctl : public ::testing::Test {
protected:
    void SetUp() override {
        std::system((std::string("ip link del ") + kIf + " 2>/dev/null").c_str());
        if (std::system((std::string("ip link add ") + kIf + " type dummy").c_str()) != 0) {
            // ctest counts a skip as a pass, so under run-tests.sh (which sets
            // REQUIRE_NET_ADMIN=1) a missing capability must be a hard FAILURE.
            const char* req = std::getenv("REQUIRE_NET_ADMIN");
            if (req && std::string(req) == "1")
                FAIL() << "REQUIRE_NET_ADMIN=1 but cannot create dummy link";
            GTEST_SKIP() << "cannot create dummy link (need --cap-add NET_ADMIN)";
        }
        fd_ = rtnl_open(RTMGRP_LINK, 1);
        ASSERT_GE(fd_, 0);
    }
    void TearDown() override {
        if (fd_ >= 0) close(fd_);
        std::system((std::string("ip link del ") + kIf + " 2>/dev/null").c_str());
    }
    int fd_ = -1;
};
}  // namespace

TEST_F(NetlinkIoctl, DumpFindsTheInterface) {
    Events ev;
    ASSERT_EQ(rtnl_dump_links(Events::cb, &ev), 0);
    ASSERT_EQ(ev.v.size(), 1u);
    EXPECT_EQ(ev.v[0].ifindex, (int)if_nametoindex(kIf));
    EXPECT_EQ(ev.v[0].flags & IFF_UP, 0u);
}

TEST_F(NetlinkIoctl, IoctlUpDownIsSeenByNetlink) {
    unsigned f = 0;
    ASSERT_EQ(ifctl_get_flags(kIf, &f), 0);
    EXPECT_EQ(f & IFF_UP, 0u);

    ASSERT_EQ(ifctl_set_up(kIf, 1), 0);
    ASSERT_EQ(ifctl_get_flags(kIf, &f), 0);
    EXPECT_NE(f & IFF_UP, 0u);
    Events ev;
    EXPECT_TRUE(wait_for(fd_, ev, [](const rtnl_link& l) { return l.is_new && (l.flags & IFF_UP); }))
        << "netlink never reported IFF_UP";

    ev.v.clear();
    ASSERT_EQ(ifctl_set_up(kIf, 0), 0);
    EXPECT_TRUE(wait_for(fd_, ev, [](const rtnl_link& l) { return l.is_new && !(l.flags & IFF_UP); }))
        << "netlink never reported DOWN";
}

TEST_F(NetlinkIoctl, IoctlMtuIsSeenByNetlink) {
    ASSERT_EQ(ifctl_set_mtu(kIf, 1430), 0);
    int mtu = 0;
    ASSERT_EQ(ifctl_get_mtu(kIf, &mtu), 0);
    EXPECT_EQ(mtu, 1430);
    Events ev;
    EXPECT_TRUE(wait_for(fd_, ev, [](const rtnl_link& l) { return l.mtu == 1430; }));
}

TEST_F(NetlinkIoctl, DeleteIsReportedAsDelLink) {
    Events ev;
    ASSERT_EQ(std::system((std::string("ip link del ") + kIf).c_str()), 0);
    EXPECT_TRUE(wait_for(fd_, ev, [](const rtnl_link& l) { return !l.is_new; }));
}

TEST(Ifctl, ErrorsAreNegativeErrno) {
    unsigned f;
    EXPECT_EQ(ifctl_get_flags("nope0", &f), -ENODEV);
    EXPECT_EQ(ifctl_get_flags("this-name-is-way-too-long-for-ifnamsiz", &f), -EINVAL);
}
