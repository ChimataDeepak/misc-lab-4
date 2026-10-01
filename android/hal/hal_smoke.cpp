// hal_smoke: drives ModemHal (the same object the JNI bridge and the AIDL service
// wrap) against a running modem_sim. This is what we can actually EXECUTE: the
// binder and JVM halves need an Android runtime this machine does not have.
#include "modem_hal.hpp"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    const char* sock = argc > 1 ? argv[1] : "/tmp/m.sock";
    mlab::ModemHal hal;
    if (int rc = hal.open(sock)) { std::fprintf(stderr, "open %s: %s\n", sock, std::strerror(-rc)); return 1; }
    auto rev = hal.revision();
    auto imei = hal.imei();
    auto sig = hal.signal_dbm();
    std::printf("revision=%s imei=%s signal=%d dBm\n", rev ? rev->c_str() : "?", imei ? imei->c_str() : "?",
                sig ? *sig : 0);
    int inds = 0;
    hal.poll_indications(1500, [&](int dbm) { inds++; std::printf("indication: signal %d dBm\n", dbm); });
    bool ok = rev && imei && sig && inds > 0;
    std::printf("%s\n", ok ? "HAL_SMOKE PASS" : "HAL_SMOKE FAIL");
    return ok ? 0 : 1;
}
