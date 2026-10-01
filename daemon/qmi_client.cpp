#include "qmi_client.hpp"

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace mlab {

const char* to_string(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::QmiError: return "qmi-error";
        case Status::Timeout: return "timeout";
        case Status::Disconnected: return "disconnected";
    }
    return "?";
}

std::optional<std::vector<uint8_t>> Reply::bytes(uint8_t tlv_type) const {
    qmi_tlv t;
    if (frame.empty() || qmi_find_tlv(&msg, tlv_type, &t) != QMI_OK) return std::nullopt;
    return std::vector<uint8_t>(t.val, t.val + t.len);
}

std::optional<std::string> Reply::str(uint8_t tlv_type) const {
    auto b = bytes(tlv_type);
    if (!b) return std::nullopt;
    return std::string(b->begin(), b->end());
}

int QmiClient::connect_unix(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    if (path.size() >= sizeof sa.sun_path) {
        close(fd);
        return -ENAMETOOLONG;
    }
    std::memcpy(sa.sun_path, path.c_str(), path.size() + 1);
    // Blocking connect is fine (local, instant); we switch to nonblocking after.
    if (connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    return fd;
}

QmiClient::QmiClient(EventLoop& loop, int fd) : loop_(loop), fd_(fd) {
    fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
    loop_.add_fd(fd_, EPOLLIN, [this](uint32_t ev) { on_events(ev); });
}

QmiClient::~QmiClient() {
    for (auto& [k, p] : pending_) loop_.cancel_timer(p.timer);
    if (fd_ >= 0) {
        loop_.del_fd(fd_);
        close(fd_);
    }
}

uint16_t QmiClient::send_request(uint8_t service, uint8_t client_id, uint16_t msg_id,
                                 const std::vector<Tlv>& tlvs, Callback cb,
                                 std::chrono::milliseconds timeout) {
    // Pick the next txn id, skipping 0 (reserved) and any id still in flight.
    // Bounded search: if every id is in flight (255 for CTL) we must not spin forever.
    uint16_t txn = 0;
    const unsigned space = service == QMI_SVC_CTL ? 256u : 65536u;
    for (unsigned tries = 0; tries < space; tries++) {
        uint16_t cand = service == QMI_SVC_CTL ? ++ctl_txn_ : ++svc_txn_;
        if (cand != 0 && !pending_.count(key(service, client_id, cand))) {
            txn = cand;
            break;
        }
    }
    if (txn == 0) {  // id space exhausted: refuse instead of reusing a live id
        Reply r;
        r.status = Status::QmiError;
        cb(r);
        return 0;
    }

    if (fd_ < 0) {
        Reply r;
        r.status = Status::Disconnected;
        cb(r);
        return txn;
    }

    uint8_t buf[QMI_MAX_FRAME];
    qmi_builder b;
    qmi_build_begin(&b, buf, sizeof buf, service, client_id, QMI_MSG_REQUEST, txn, msg_id);
    for (const auto& t : tlvs)
        qmi_build_tlv(&b, t.type, t.value.data(), static_cast<uint16_t>(t.value.size()));
    ssize_t n = qmi_build_finish(&b);
    if (n < 0) {
        Reply r;
        r.status = Status::QmiError;
        cb(r);
        return txn;
    }

    uint32_t k = key(service, client_id, txn);
    // The timer captures the KEY, not an iterator/pointer: the map may rehash.
    auto timer = loop_.add_timer(timeout, [this, k]() {
        auto it = pending_.find(k);
        if (it == pending_.end()) return;
        Callback c = std::move(it->second.cb);
        pending_.erase(it);
        Reply r;
        r.status = Status::Timeout;
        c(r);
    });
    pending_.emplace(k, Pending{std::move(cb), timer});

    tx_.insert(tx_.end(), buf, buf + n);
    flush();
    return txn;
}

void QmiClient::allocate_cid(uint8_t service, std::function<void(Status, uint8_t)> cb) {
    send_request(QMI_SVC_CTL, 0, QMI_CTL_ALLOCATE_CID, {Tlv{0x01, {service}}},
                 [cb = std::move(cb), service](const Reply& r) {
                     if (r.status != Status::Ok) return cb(r.status, 0);
                     auto info = r.bytes(0x01);
                     if (!info || info->size() != 2 || (*info)[0] != service)
                         return cb(Status::QmiError, 0);
                     cb(Status::Ok, (*info)[1]);
                 });
}

void QmiClient::flush() {
    while (!tx_.empty() && fd_ >= 0) {
        ssize_t n = send(fd_, tx_.data(), tx_.size(), MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            fail_all(Status::Disconnected);
            return;
        }
        tx_.erase(tx_.begin(), tx_.begin() + n);  // partial write: keep the rest
    }
    bool want = !tx_.empty();
    if (fd_ >= 0 && want != want_out_) {
        want_out_ = want;
        loop_.mod_fd(fd_, EPOLLIN | (want ? EPOLLOUT : 0u));
    }
}

void QmiClient::on_events(uint32_t ev) {
    if (ev & EPOLLOUT) flush();
    if (ev & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
        uint8_t buf[4096];
        for (;;) {
            ssize_t n = recv(fd_, buf, sizeof buf, 0);
            if (n > 0) {
                rx_.append(buf, static_cast<size_t>(n));
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            // n == 0 (EOF) or a hard error. Deliver what we have, then fail the rest.
            while (auto f = rx_.next()) handle_frame(std::move(*f));
            fail_all(Status::Disconnected);
            return;
        }
        while (auto f = rx_.next()) handle_frame(std::move(*f));
    }
}

void QmiClient::handle_frame(std::vector<uint8_t> frame) {
    Reply r;
    r.frame = std::move(frame);
    if (qmi_parse(r.frame.data(), r.frame.size(), &r.msg) != QMI_OK) return;  // drop malformed
    frames_rx_++;

    if (r.msg.type == QMI_MSG_INDICATION) {
        if (ind_cb_) ind_cb_(r);
        return;
    }
    if (r.msg.type != QMI_MSG_RESPONSE) return;

    auto it = pending_.find(key(r.msg.service, r.msg.client_id, r.msg.txn_id));
    if (it == pending_.end()) return;  // late reply after timeout, or unknown txn: ignore
    loop_.cancel_timer(it->second.timer);
    Callback cb = std::move(it->second.cb);
    pending_.erase(it);  // erase BEFORE calling: cb may send new requests

    if (qmi_get_result(&r.msg, &r.qmi_result, &r.qmi_error) != QMI_OK || r.qmi_result != 0)
        r.status = Status::QmiError;
    cb(r);
}

void QmiClient::fail_all(Status s) {
    if (fd_ >= 0) {
        loop_.del_fd(fd_);
        close(fd_);
        fd_ = -1;
    }
    auto pend = std::move(pending_);
    pending_.clear();
    for (auto& [k, p] : pend) {
        loop_.cancel_timer(p.timer);
        Reply r;
        r.status = s;
        p.cb(r);
    }
}

}  // namespace mlab
