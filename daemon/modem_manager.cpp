// modem_manager -- the C++17 "connection manager" daemon of the lab.
//
// Rough real-world analogue: ModemManager (Linux) or the Android RIL daemon.
// It talks QMI to the (simulated) modem, watches the data interface with
// RTNETLINK, and configures that interface with ioctl().
//
// Bring-up sequence (all async on one epoll loop, no threads):
//   CTL AllocateCID(DMS) -> DMS GetRevision -> DMS GetIDs
//   -> CTL AllocateCID(NAS) -> NAS GetSignalStrength -> "modem ready"
//   -> [--bring-up] ioctl SIOCSIFMTU + SIOCSIFFLAGS(IFF_UP) on the data iface
//   ... NAS indications + netlink link events are logged as they arrive ...
//   -> after --run-ms: [--bring-up] ioctl IFF_UP off, drain netlink, exit.
//
// usage: modem_manager --socket PATH [--iface wwan0] [--bring-up] [--mtu N] [--run-ms N]
#include "ifctl.h"
#include "link_monitor.hpp"
#include "qmi_client.hpp"

#include <net/if.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

using namespace mlab;
using namespace std::chrono_literals;

namespace {

struct Options {
    std::string socket;
    std::string iface = "wwan0";
    bool bring_up = false;
    int mtu = 1430;      // typical for LTE/5G after tunnelling overhead
    long run_ms = 0;     // 0 = forever
};

void log(const std::string& s) { std::cout << "[modem_manager] " << s << std::endl; }

std::string flags_str(unsigned f) { return (f & IFF_UP) ? "UP" : "DOWN"; }

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--socket") o.socket = next();
        else if (a == "--iface") o.iface = next();
        else if (a == "--bring-up") o.bring_up = true;
        else if (a == "--mtu") o.mtu = std::atoi(next().c_str());
        else if (a == "--run-ms") o.run_ms = std::atol(next().c_str());
        else {
            std::cerr << "usage: modem_manager --socket PATH [--iface IF] [--bring-up] [--mtu N] [--run-ms N]\n";
            return 2;
        }
    }
    if (o.socket.empty()) { std::cerr << "--socket required\n"; return 2; }

    EventLoop loop;
    int errors = 0;
    bool ready = false, done = false;

    // --- RTNETLINK: log only the data interface's transitions -------------
    LinkMonitor links(loop, [&](const rtnl_link& l, unsigned prev, bool known) {
        if (o.iface != l.name) return;
        if (!l.is_new) { log("netlink: link " + std::string(l.name) + " removed"); return; }
        if (!known) {
            log("netlink: found link " + std::string(l.name) + " idx=" + std::to_string(l.ifindex) +
                " state=" + flags_str(l.flags) + " mtu=" + std::to_string(l.mtu));
        } else if ((prev ^ l.flags) & IFF_UP) {
            log("netlink: link " + std::string(l.name) + " " + flags_str(prev) + " -> " +
                flags_str(l.flags) + " mtu=" + std::to_string(l.mtu));
        } else {
            log("netlink: link " + std::string(l.name) + " changed mtu=" + std::to_string(l.mtu));
        }
    });

    // --- QMI over the Unix socket ------------------------------------------
    int fd = QmiClient::connect_unix(o.socket);
    if (fd < 0) { log("connect " + o.socket + ": " + std::strerror(-fd)); return 1; }
    QmiClient qmi(loop, fd);
    log("connected to modem at " + o.socket);

    qmi.on_indication([&](const Reply& r) {
        if (r.msg.service == QMI_SVC_NAS && r.msg.msg_id == QMI_NAS_EVENT_REPORT) {
            auto sig = r.bytes(0x10);
            if (sig && sig->size() == 2)
                log("indication: NAS signal " + std::to_string(static_cast<int8_t>((*sig)[0])) + " dBm");
        }
    });

    auto check = [&](const char* what, const Reply& r) {
        if (r.status == Status::Ok) return true;
        errors++;
        log(std::string(what) + " failed: " + to_string(r.status) + " qmi_error=" + std::to_string(r.qmi_error));
        return false;
    };

    auto bring_up_iface = [&]() {
        if (!o.bring_up) return;
        int rc = ifctl_set_mtu(o.iface.c_str(), o.mtu);
        if (rc == 0) rc = ifctl_set_up(o.iface.c_str(), 1);
        if (rc) { errors++; log("ioctl on " + o.iface + " failed: " + std::strerror(-rc)); return; }
        unsigned f = 0;
        int mtu = 0;
        ifctl_get_flags(o.iface.c_str(), &f);
        ifctl_get_mtu(o.iface.c_str(), &mtu);
        log("ioctl: " + o.iface + " is now " + flags_str(f) + " mtu=" + std::to_string(mtu));
    };

    // The bring-up chain. Each step is started from the previous step's callback.
    qmi.allocate_cid(QMI_SVC_DMS, [&](Status s, uint8_t dms) {
        if (s != Status::Ok) { errors++; log(std::string("DMS cid alloc failed: ") + to_string(s)); return; }
        log("CTL: DMS client id = " + std::to_string(dms));
        qmi.send_request(QMI_SVC_DMS, dms, QMI_DMS_GET_REVISION, {}, [&, dms](const Reply& r) {
            if (!check("DMS GetRevision", r)) return;
            log("DMS revision: " + r.str(0x01).value_or("?"));
            qmi.send_request(QMI_SVC_DMS, dms, QMI_DMS_GET_IDS, {}, [&](const Reply& r2) {
                if (!check("DMS GetIDs", r2)) return;
                log("DMS IMEI: " + r2.str(0x11).value_or("?") + " ESN: " + r2.str(0x10).value_or("?") +
                    " MEID: " + r2.str(0x12).value_or("?"));
                qmi.allocate_cid(QMI_SVC_NAS, [&](Status s2, uint8_t nas) {
                    if (s2 != Status::Ok) { errors++; log("NAS cid alloc failed"); return; }
                    log("CTL: NAS client id = " + std::to_string(nas));
                    qmi.send_request(QMI_SVC_NAS, nas, QMI_NAS_GET_SIGNAL_STRENGTH, {}, [&](const Reply& r3) {
                        if (!check("NAS GetSignalStrength", r3)) return;
                        auto sig = r3.bytes(0x01);
                        if (sig && sig->size() == 2)
                            log("NAS signal strength: " + std::to_string(static_cast<int8_t>((*sig)[0])) +
                                " dBm (radio if " + std::to_string((*sig)[1]) + ")");
                        ready = true;
                        log("modem ready");
                        bring_up_iface();
                    });
                });
            });
        });
    });

    if (o.run_ms > 0) loop.add_timer(std::chrono::milliseconds(o.run_ms), [&] { done = true; });
    while (!done && qmi.connected()) loop.run_once(1000ms);

    if (o.bring_up && ready) {
        int rc = ifctl_set_up(o.iface.c_str(), 0);
        log(rc ? "ioctl down failed" : "ioctl: " + o.iface + " set DOWN");
        loop.run_until([] { return false; }, 200ms);  // let netlink report the DOWN
    }
    if (!qmi.connected()) { log("modem disconnected"); errors++; }
    log("exiting: ready=" + std::string(ready ? "yes" : "no") + " errors=" + std::to_string(errors) +
        " frames_rx=" + std::to_string(qmi.frames_rx()));
    return (ready && errors == 0) ? 0 : 1;
}
