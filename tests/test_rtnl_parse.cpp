// rtnl_parse() on hand-built netlink messages: no kernel, no privileges needed.
#include <gtest/gtest.h>
#include <linux/rtnetlink.h>

#include <cstring>
#include <vector>

#include "rtnl.h"

namespace {
// Appends one RTM_NEWLINK message with IFLA_IFNAME and IFLA_MTU attributes.
void add_link(std::vector<uint8_t>& out, int idx, unsigned flags, const char* name, uint32_t mtu,
              uint16_t type = RTM_NEWLINK) {
    size_t name_len = strlen(name) + 1;
    size_t attrs = RTA_SPACE(name_len) + RTA_SPACE(sizeof mtu);
    size_t len = NLMSG_LENGTH(sizeof(ifinfomsg)) + attrs;
    size_t off = out.size();
    out.resize(off + NLMSG_ALIGN(len), 0);
    auto* nh = reinterpret_cast<nlmsghdr*>(&out[off]);
    nh->nlmsg_len = (uint32_t)len;
    nh->nlmsg_type = type;
    auto* ifi = static_cast<ifinfomsg*>(NLMSG_DATA(nh));
    ifi->ifi_index = idx;
    ifi->ifi_flags = flags;
    auto* rta = reinterpret_cast<rtattr*>(reinterpret_cast<uint8_t*>(ifi) + NLMSG_ALIGN(sizeof(ifinfomsg)));
    rta->rta_type = IFLA_IFNAME;
    rta->rta_len = (unsigned short)RTA_LENGTH(name_len);
    memcpy(RTA_DATA(rta), name, name_len);
    rta = reinterpret_cast<rtattr*>(reinterpret_cast<uint8_t*>(rta) + RTA_SPACE(name_len));
    rta->rta_type = IFLA_MTU;
    rta->rta_len = (unsigned short)RTA_LENGTH(sizeof mtu);
    memcpy(RTA_DATA(rta), &mtu, sizeof mtu);
}

void add_simple(std::vector<uint8_t>& out, uint16_t type, int err = 0) {
    size_t len = NLMSG_LENGTH(sizeof(nlmsgerr));
    size_t off = out.size();
    out.resize(off + NLMSG_ALIGN(len), 0);
    auto* nh = reinterpret_cast<nlmsghdr*>(&out[off]);
    nh->nlmsg_len = (uint32_t)len;
    nh->nlmsg_type = type;
    if (type == NLMSG_ERROR) static_cast<nlmsgerr*>(NLMSG_DATA(nh))->error = err;
}

void collect(const rtnl_link* l, void* u) { static_cast<std::vector<rtnl_link>*>(u)->push_back(*l); }
}  // namespace

TEST(RtnlParse, MultipartDumpThenDone) {
    std::vector<uint8_t> buf;
    add_link(buf, 1, IFF_UP | IFF_LOOPBACK, "lo", 65536);
    add_link(buf, 7, 0, "wwan0", 1500);
    add_simple(buf, NLMSG_DONE);
    std::vector<rtnl_link> links;
    EXPECT_EQ(rtnl_parse(buf.data(), buf.size(), collect, &links), 1);
    ASSERT_EQ(links.size(), 2u);
    EXPECT_STREQ(links[1].name, "wwan0");
    EXPECT_EQ(links[1].ifindex, 7);
    EXPECT_EQ(links[1].mtu, 1500u);
    EXPECT_EQ(links[0].flags & IFF_UP, (unsigned)IFF_UP);
    EXPECT_EQ(links[1].is_new, 1);
}

TEST(RtnlParse, DelLinkAndNoDone) {
    std::vector<uint8_t> buf;
    add_link(buf, 7, 0, "wwan0", 1500, RTM_DELLINK);
    std::vector<rtnl_link> links;
    EXPECT_EQ(rtnl_parse(buf.data(), buf.size(), collect, &links), 0);
    ASSERT_EQ(links.size(), 1u);
    EXPECT_EQ(links[0].is_new, 0);
}

TEST(RtnlParse, ErrorAndTruncation) {
    std::vector<uint8_t> buf;
    add_simple(buf, NLMSG_ERROR, -EPERM);
    std::vector<rtnl_link> links;
    EXPECT_EQ(rtnl_parse(buf.data(), buf.size(), collect, &links), -EPERM);

    std::vector<uint8_t> t;
    add_link(t, 3, 0, "eth0", 1500);
    EXPECT_EQ(rtnl_parse(t.data(), t.size() - 6, collect, &links), -EBADMSG);
}
