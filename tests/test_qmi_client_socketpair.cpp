// QmiClient against a hand-driven "modem" on the other end of a socketpair().
// socketpair gives two connected stream sockets in one process, so the test
// controls EXACTLY how bytes arrive: split mid-frame, out of order, never, ...
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <set>

#include "frame_buffer.hpp"
#include "qmi_client.hpp"

using namespace mlab;
using namespace std::chrono_literals;

namespace {

class SocketpairTest : public ::testing::Test {
protected:
    void SetUp() override {
        int sv[2];
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv), 0);
        client_ = std::make_unique<QmiClient>(loop_, sv[0]);
        modem_ = sv[1];
    }
    void TearDown() override {
        client_.reset();
        if (modem_ >= 0) close(modem_);
    }

    // Modem side: read requests the client wrote (waits up to 1s for n frames).
    std::vector<std::vector<uint8_t>> read_requests(size_t n) {
        std::vector<std::vector<uint8_t>> out;
        FrameBuffer fb;
        while (out.size() < n) {
            pollfd p{modem_, POLLIN, 0};
            if (poll(&p, 1, 1000) <= 0) break;
            uint8_t buf[1024];
            ssize_t r = read(modem_, buf, sizeof buf);
            if (r <= 0) break;
            fb.append(buf, (size_t)r);
            while (auto f = fb.next()) out.push_back(*f);
        }
        return out;
    }

    static qmi_msg parse(const std::vector<uint8_t>& f) {
        qmi_msg m;
        EXPECT_EQ(qmi_parse(f.data(), f.size(), &m), QMI_OK);
        return m;
    }

    std::vector<uint8_t> response_to(const std::vector<uint8_t>& req, const char* text, uint16_t err = 0) {
        qmi_msg m = parse(req);
        uint8_t buf[256];
        qmi_builder b;
        qmi_build_begin(&b, buf, sizeof buf, m.service, m.client_id, QMI_MSG_RESPONSE, m.txn_id, m.msg_id);
        qmi_build_result(&b, err ? 1 : 0, err);
        if (text) qmi_build_tlv(&b, 0x01, text, (uint16_t)strlen(text));
        ssize_t n = qmi_build_finish(&b);
        return {buf, buf + n};
    }

    void modem_write(const uint8_t* p, size_t n) { ASSERT_EQ(write(modem_, p, n), (ssize_t)n); }
    void modem_write(const std::vector<uint8_t>& v) { modem_write(v.data(), v.size()); }

    EventLoop loop_;
    std::unique_ptr<QmiClient> client_;
    int modem_ = -1;
};

}  // namespace

TEST_F(SocketpairTest, ResponseSplitAcrossReadsIsReassembled) {
    std::string got;
    client_->send_request(QMI_SVC_DMS, 1, QMI_DMS_GET_REVISION, {},
                          [&](const Reply& r) { got = r.status == Status::Ok ? *r.str(0x01) : "ERR"; });
    auto reqs = read_requests(1);
    ASSERT_EQ(reqs.size(), 1u);
    auto rsp = response_to(reqs[0], "REV-SPLIT");

    modem_write(rsp.data(), 2);  // not even the full QMUX length field yet
    loop_.run_once(50ms);
    EXPECT_EQ(got, "");
    modem_write(rsp.data() + 2, 9);
    loop_.run_once(50ms);
    EXPECT_EQ(got, "");
    modem_write(rsp.data() + 11, rsp.size() - 11);
    EXPECT_TRUE(loop_.run_until([&] { return !got.empty(); }, 1s));
    EXPECT_EQ(got, "REV-SPLIT");
    EXPECT_EQ(client_->pending(), 0u);
}

TEST_F(SocketpairTest, OutOfOrderResponsesMatchByTxnId) {
    std::vector<std::string> order;
    client_->send_request(QMI_SVC_DMS, 1, QMI_DMS_GET_REVISION, {},
                          [&](const Reply& r) { order.push_back("first:" + *r.str(0x01)); });
    client_->send_request(QMI_SVC_DMS, 1, QMI_DMS_GET_IDS, {},
                          [&](const Reply& r) { order.push_back("second:" + *r.str(0x01)); });
    auto reqs = read_requests(2);
    ASSERT_EQ(reqs.size(), 2u);
    EXPECT_NE(parse(reqs[0]).txn_id, parse(reqs[1]).txn_id);

    // Answer the SECOND request first, both in one write.
    auto both = response_to(reqs[1], "B");
    auto a = response_to(reqs[0], "A");
    both.insert(both.end(), a.begin(), a.end());
    modem_write(both);
    EXPECT_TRUE(loop_.run_until([&] { return order.size() == 2; }, 1s));
    EXPECT_EQ(order, (std::vector<std::string>{"second:B", "first:A"}));
}

TEST_F(SocketpairTest, TimeoutFiresAndLateReplyIsIgnored) {
    Status st = Status::Ok;
    int calls = 0;
    client_->send_request(QMI_SVC_DMS, 1, QMI_DMS_GET_REVISION, {},
                          [&](const Reply& r) { st = r.status; calls++; }, 100ms);
    auto reqs = read_requests(1);
    auto start = EventLoop::Clock::now();
    EXPECT_TRUE(loop_.run_until([&] { return calls == 1; }, 2s));
    EXPECT_EQ(st, Status::Timeout);
    EXPECT_GE(EventLoop::Clock::now() - start, 80ms);
    EXPECT_EQ(client_->pending(), 0u);

    modem_write(response_to(reqs[0], "too late"));
    loop_.run_until([] { return false; }, 100ms);
    EXPECT_EQ(calls, 1);  // callback never fires twice
}

TEST_F(SocketpairTest, QmiErrorResultIsSurfaced) {
    uint16_t err = 0;
    Status st = Status::Ok;
    client_->send_request(QMI_SVC_DMS, 9, QMI_DMS_GET_REVISION, {},
                          [&](const Reply& r) { st = r.status; err = r.qmi_error; });
    auto reqs = read_requests(1);
    modem_write(response_to(reqs[0], nullptr, QMI_ERR_INVALID_CLIENT_ID));
    EXPECT_TRUE(loop_.run_until([&] { return st != Status::Ok; }, 1s));
    EXPECT_EQ(st, Status::QmiError);
    EXPECT_EQ(err, QMI_ERR_INVALID_CLIENT_ID);
}

TEST_F(SocketpairTest, IndicationGoesToIndicationHandler) {
    int8_t dbm = 0;
    client_->on_indication([&](const Reply& r) { dbm = (int8_t)(*r.bytes(0x10))[0]; });
    uint8_t buf[64];
    qmi_builder b;
    qmi_build_begin(&b, buf, sizeof buf, QMI_SVC_NAS, 2, QMI_MSG_INDICATION, 0, QMI_NAS_EVENT_REPORT);
    uint8_t sig[2] = {(uint8_t)-88, QMI_NAS_RADIO_LTE};
    qmi_build_tlv(&b, 0x10, sig, 2);
    modem_write(buf, (size_t)qmi_build_finish(&b));
    EXPECT_TRUE(loop_.run_until([&] { return dbm != 0; }, 1s));
    EXPECT_EQ(dbm, -88);
}

TEST_F(SocketpairTest, PeerCloseFailsAllPending) {
    int disconnected = 0;
    for (int i = 0; i < 3; i++)
        client_->send_request(QMI_SVC_DMS, 1, QMI_DMS_GET_REVISION, {}, [&](const Reply& r) {
            if (r.status == Status::Disconnected) disconnected++;
        });
    read_requests(3);
    close(modem_);
    modem_ = -1;
    EXPECT_TRUE(loop_.run_until([&] { return disconnected == 3; }, 1s));
    EXPECT_FALSE(client_->connected());
}

TEST_F(SocketpairTest, CtlTxnIdIsEightBitSkipsZeroAndNeverReusesLiveIds) {
    std::set<uint16_t> uniq;
    for (int i = 0; i < 255; i++) {
        uint16_t t = client_->send_request(QMI_SVC_CTL, 0, QMI_CTL_ALLOCATE_CID, {Tlv{0x01, {2}}},
                                           [](const Reply&) {}, 10s);
        EXPECT_NE(t, 0);
        EXPECT_LE(t, 255);
        uniq.insert(t);
    }
    EXPECT_EQ(uniq.size(), 255u);  // all 255 usable 8-bit ids, none reused
    // The 256th concurrent CTL request has no free id: refused, not duplicated.
    Status st = Status::Ok;
    EXPECT_EQ(client_->send_request(QMI_SVC_CTL, 0, QMI_CTL_ALLOCATE_CID, {Tlv{0x01, {2}}},
                                    [&](const Reply& r) { st = r.status; }),
              0);
    EXPECT_EQ(st, Status::QmiError);
}

// Review fix: a due timer cancelled by an earlier due timer's callback must NOT run.
TEST(EventLoopTimers, CancelFromEarlierDueCallbackWins) {
    mlab::EventLoop loop;
    bool second_ran = false;
    mlab::EventLoop::TimerId second = 0;
    loop.add_timer(std::chrono::milliseconds(0), [&] { loop.cancel_timer(second); });
    second = loop.add_timer(std::chrono::milliseconds(0), [&] { second_ran = true; });
    loop.run_once(std::chrono::milliseconds(5));
    loop.run_once(std::chrono::milliseconds(5));
    EXPECT_FALSE(second_ran);
}

// Review fix: oversized TLV is rejected with QmiError, not silently truncated.
TEST(QmiClientLimits, OversizedTlvRejected) {
    mlab::EventLoop loop;
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv), 0);
    mlab::QmiClient c(loop, sv[0]);
    mlab::Status st = mlab::Status::Ok;
    std::vector<mlab::Tlv> tlvs{{0x01, std::vector<uint8_t>(70000, 0xAB)}};
    EXPECT_EQ(c.send_request(QMI_SVC_DMS, 1, 0x0023, tlvs,
                             [&](const mlab::Reply& r) { st = r.status; },
                             std::chrono::milliseconds(100)),
              0);
    EXPECT_EQ(st, mlab::Status::QmiError);
    close(sv[1]);
}
