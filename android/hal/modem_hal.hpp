#pragma once
// ModemHal: a SYNCHRONOUS facade over the lab's async QmiClient.
// HAL/JNI entry points are blocking calls ("give me the IMEI"), while QmiClient is
// callback-driven on an epoll EventLoop. This class bridges the two by running the
// loop until the specific reply arrives (or a timeout), exactly the shape of a
// vendor RIL turning a binder call into a modem transaction.
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace mlab {
class EventLoop;
class QmiClient;

class ModemHal {
public:
    ModemHal();
    ~ModemHal();
    int open(const std::string& socket_path);  // 0 or -errno
    std::optional<std::string> revision();
    std::optional<std::string> imei();
    std::optional<int> signal_dbm();
    // Pump the loop for up to ms, delivering NAS indications to cb.
    void poll_indications(int ms, const std::function<void(int dbm)>& cb);

private:
    bool ensure_cid(uint8_t svc, uint8_t& cid);
    std::unique_ptr<EventLoop> loop_;
    std::unique_ptr<QmiClient> qmi_;
    uint8_t dms_ = 0, nas_ = 0;
    std::function<void(int)> ind_cb_;
    std::mutex mu_;  // binder/JNI calls can arrive on many threads; the loop is single-threaded
};
}  // namespace mlab
