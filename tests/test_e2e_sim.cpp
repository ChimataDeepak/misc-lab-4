// Integration: QmiClient (C++) <-> real modem_sim PROCESS over a Unix socket.
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>

#include "qmi_client.hpp"
#include "sim_core.h"

using namespace mlab;
using namespace std::chrono_literals;

namespace {
class SimProcess {
public:
    explicit SimProcess(const std::string& sock) {
        int p[2];
        if (pipe(p) != 0) return;
        pid_ = fork();
        if (pid_ == 0) {
            dup2(p[1], STDOUT_FILENO);
            close(p[0]);
            execl(MODEM_SIM_PATH, "modem_sim", "--socket", sock.c_str(), "--ind-ms", "50", (char*)nullptr);
            _exit(127);
        }
        close(p[1]);
        // Wait for "READY": the socket is listening only after that line.
        FILE* f = fdopen(p[0], "r");
        char line[64] = {0};
        ready_ = fgets(line, sizeof line, f) && std::string(line) == "READY\n";
        fclose(f);
    }
    ~SimProcess() { stop(); }
    void stop() {
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            int st;
            waitpid(pid_, &st, 0);
            pid_ = -1;
        }
    }
    bool ready() const { return ready_; }

private:
    pid_t pid_ = -1;
    bool ready_ = false;
};
}  // namespace

TEST(EndToEnd, FullBringUpAgainstSimulatorProcess) {
    std::string sock = "/tmp/mlab-e2e-" + std::to_string(getpid()) + ".sock";
    SimProcess sim(sock);
    ASSERT_TRUE(sim.ready());

    EventLoop loop;
    int fd = QmiClient::connect_unix(sock);
    ASSERT_GE(fd, 0) << strerror(-fd);
    QmiClient qmi(loop, fd);

    int indications = 0;
    qmi.on_indication([&](const Reply& r) {
        if (r.msg.service == QMI_SVC_NAS && r.bytes(0x10)) indications++;
    });

    uint8_t dms = 0, nas = 0;
    bool got = false;
    qmi.allocate_cid(QMI_SVC_DMS, [&](Status s, uint8_t c) { EXPECT_EQ(s, Status::Ok); dms = c; got = true; });
    ASSERT_TRUE(loop.run_until([&] { return got; }, 2s));
    ASSERT_NE(dms, 0);

    std::string rev, imei;
    qmi.send_request(QMI_SVC_DMS, dms, QMI_DMS_GET_REVISION, {}, [&](const Reply& r) { rev = r.str(0x01).value_or("ERR"); });
    qmi.send_request(QMI_SVC_DMS, dms, QMI_DMS_GET_IDS, {}, [&](const Reply& r) { imei = r.str(0x11).value_or("ERR"); });
    ASSERT_TRUE(loop.run_until([&] { return !rev.empty() && !imei.empty(); }, 2s));
    EXPECT_EQ(rev, SIM_REVISION);
    EXPECT_EQ(imei, SIM_IMEI);

    got = false;
    qmi.allocate_cid(QMI_SVC_NAS, [&](Status s, uint8_t c) { EXPECT_EQ(s, Status::Ok); nas = c; got = true; });
    ASSERT_TRUE(loop.run_until([&] { return got; }, 2s));
    int8_t dbm = 0;
    qmi.send_request(QMI_SVC_NAS, nas, QMI_NAS_GET_SIGNAL_STRENGTH, {},
                     [&](const Reply& r) { dbm = (int8_t)(*r.bytes(0x01))[0]; });
    ASSERT_TRUE(loop.run_until([&] { return dbm != 0; }, 2s));
    EXPECT_LE(dbm, -60);

    // Async indications arrive with NO request outstanding.
    EXPECT_TRUE(loop.run_until([&] { return indications >= 3; }, 3s));

    // Error path through the real process: wrong client id.
    uint16_t err = 0;
    qmi.send_request(QMI_SVC_DMS, 200, QMI_DMS_GET_REVISION, {}, [&](const Reply& r) { err = r.qmi_error; });
    ASSERT_TRUE(loop.run_until([&] { return err != 0; }, 2s));
    EXPECT_EQ(err, QMI_ERR_INVALID_CLIENT_ID);

    // Modem "crashes": pending requests must fail, not hang.
    Status st = Status::Ok;
    sim.stop();
    qmi.send_request(QMI_SVC_DMS, dms, QMI_DMS_GET_REVISION, {}, [&](const Reply& r) { st = r.status; }, 5s);
    ASSERT_TRUE(loop.run_until([&] { return st != Status::Ok; }, 3s));
    EXPECT_EQ(st, Status::Disconnected);
}
