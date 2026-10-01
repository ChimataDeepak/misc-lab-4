#include "modem_hal.hpp"

#include "event_loop.hpp"
#include "qmi_client.hpp"

#include <chrono>

using namespace std::chrono_literals;

namespace mlab {

ModemHal::ModemHal() : loop_(std::make_unique<EventLoop>()) {}
ModemHal::~ModemHal() = default;

int ModemHal::open(const std::string& path) {
    std::lock_guard<std::mutex> g(mu_);
    int fd = QmiClient::connect_unix(path);
    if (fd < 0) return fd;
    qmi_ = std::make_unique<QmiClient>(*loop_, fd);
    qmi_->on_indication([this](const Reply& r) {
        if (r.msg.service != QMI_SVC_NAS || r.msg.msg_id != QMI_NAS_EVENT_REPORT || !ind_cb_) return;
        auto sig = r.bytes(0x10);
        if (sig && sig->size() == 2) ind_cb_(static_cast<int8_t>((*sig)[0]));
    });
    return 0;
}

bool ModemHal::ensure_cid(uint8_t svc, uint8_t& cid) {
    if (cid) return true;
    bool done = false;
    qmi_->allocate_cid(svc, [&](Status s, uint8_t c) { done = true; if (s == Status::Ok) cid = c; });
    loop_->run_until([&] { return done; }, 3000ms);
    return cid != 0;
}

// Send one request and block (pumping the loop) until its reply. `take` reads the
// TLVs it needs while the Reply is still alive (Reply's msg points into its frame).
static bool call(EventLoop& loop, QmiClient& q, uint8_t svc, uint8_t cid, uint16_t msg,
                 const std::function<void(const Reply&)>& take) {
    bool done = false, ok = false;
    q.send_request(svc, cid, msg, {}, [&](const Reply& r) {
        done = true;
        ok = r.status == Status::Ok;
        if (ok) take(r);
    });
    loop.run_until([&] { return done; }, 3000ms);
    return ok;
}

std::optional<std::string> ModemHal::revision() {
    std::lock_guard<std::mutex> g(mu_);
    if (!qmi_ || !ensure_cid(QMI_SVC_DMS, dms_)) return std::nullopt;
    std::optional<std::string> v;
    call(*loop_, *qmi_, QMI_SVC_DMS, dms_, QMI_DMS_GET_REVISION, [&](const Reply& r) { v = r.str(0x01); });
    return v;
}

std::optional<std::string> ModemHal::imei() {
    std::lock_guard<std::mutex> g(mu_);
    if (!qmi_ || !ensure_cid(QMI_SVC_DMS, dms_)) return std::nullopt;
    std::optional<std::string> v;
    call(*loop_, *qmi_, QMI_SVC_DMS, dms_, QMI_DMS_GET_IDS, [&](const Reply& r) { v = r.str(0x11); });
    return v;
}

std::optional<int> ModemHal::signal_dbm() {
    std::lock_guard<std::mutex> g(mu_);
    if (!qmi_ || !ensure_cid(QMI_SVC_NAS, nas_)) return std::nullopt;
    std::optional<int> v;
    call(*loop_, *qmi_, QMI_SVC_NAS, nas_, QMI_NAS_GET_SIGNAL_STRENGTH, [&](const Reply& r) {
        auto b = r.bytes(0x01);
        if (b && b->size() == 2) v = static_cast<int8_t>((*b)[0]);
    });
    return v;
}

void ModemHal::poll_indications(int ms, const std::function<void(int)>& cb) {
    std::lock_guard<std::mutex> g(mu_);
    if (!qmi_) return;
    ind_cb_ = cb;
    auto end = EventLoop::Clock::now() + std::chrono::milliseconds(ms);
    while (EventLoop::Clock::now() < end) loop_->run_once(50ms);
    ind_cb_ = nullptr;
}

}  // namespace mlab
