// qmi_client.hpp -- asynchronous QMI client over a stream fd.
//
// Responsibilities:
//   * allocate transaction ids (8-bit for CTL, 16-bit for other services)
//   * keep a table of in-flight requests keyed by (service, client id, txn)
//   * match each incoming response to its request and fire the callback
//   * fail requests that don't get an answer within their timeout
//   * route indications (unsolicited messages) to a separate handler
//   * nonblocking writes with an outgoing queue (EPOLLOUT only when backlogged)
#pragma once

#include "event_loop.hpp"
#include "frame_buffer.hpp"
#include "qmi.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mlab {

enum class Status { Ok, QmiError, Timeout, Disconnected };
const char* to_string(Status s);

struct Tlv {
    uint8_t type;
    std::vector<uint8_t> value;
};

// One received message. `msg` points INTO `frame`, so Reply is move-only.
struct Reply {
    Status status = Status::Ok;
    std::vector<uint8_t> frame;
    qmi_msg msg{};
    uint16_t qmi_result = 0, qmi_error = 0;

    Reply() = default;
    Reply(Reply&&) = default;
    Reply& operator=(Reply&&) = default;
    Reply(const Reply&) = delete;
    Reply& operator=(const Reply&) = delete;

    std::optional<std::string> str(uint8_t tlv_type) const;
    std::optional<std::vector<uint8_t>> bytes(uint8_t tlv_type) const;
};

class QmiClient {
public:
    using Callback = std::function<void(const Reply&)>;

    // Takes ownership of an already-connected stream fd (made nonblocking here).
    QmiClient(EventLoop& loop, int fd);
    ~QmiClient();
    QmiClient(const QmiClient&) = delete;
    QmiClient& operator=(const QmiClient&) = delete;

    static int connect_unix(const std::string& path);  // returns fd or -errno

    // Returns the transaction id used (0 = no free id; cb already got QmiError).
    uint16_t send_request(uint8_t service, uint8_t client_id, uint16_t msg_id,
                          const std::vector<Tlv>& tlvs, Callback cb,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

    // CTL Allocate Client ID convenience wrapper: cb(status, cid).
    void allocate_cid(uint8_t service, std::function<void(Status, uint8_t)> cb);

    void on_indication(Callback cb) { ind_cb_ = std::move(cb); }

    size_t pending() const { return pending_.size(); }
    bool connected() const { return fd_ >= 0; }
    uint64_t frames_rx() const { return frames_rx_; }
    size_t resync_bytes() const { return rx_.resync_bytes(); }

private:
    struct Pending {
        Callback cb;
        EventLoop::TimerId timer;
    };
    static uint32_t key(uint8_t svc, uint8_t cid, uint16_t txn) {
        return (uint32_t(svc) << 24) | (uint32_t(cid) << 16) | txn;
    }
    void on_events(uint32_t ev);
    void handle_frame(std::vector<uint8_t> frame);
    void flush();
    void fail_all(Status s);

    EventLoop& loop_;
    int fd_;
    FrameBuffer rx_;
    std::vector<uint8_t> tx_;
    bool want_out_ = false;
    uint8_t ctl_txn_ = 0;   // CTL transaction ids are only 8 bits wide
    uint16_t svc_txn_ = 0;  // other services: 16 bits
    std::unordered_map<uint32_t, Pending> pending_;
    Callback ind_cb_;
    uint64_t frames_rx_ = 0;
};

}  // namespace mlab
