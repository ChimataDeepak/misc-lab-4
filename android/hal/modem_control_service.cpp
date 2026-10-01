// The HAL service object: implements the aidl-generated BnModemControl (NDK
// backend, libbinder_ndk) by delegating to ModemHal.
//
// Two ways to publish it, and the NDK only allows ONE of them:
//  * vendor/platform build (AOSP tree, -DMLAB_PLATFORM_BUILD): main() registers with
//    servicemanager as "<descriptor>/default" and runs a binder thread pool -- how a
//    real radio HAL is started by init. AServiceManager_* / ABinderProcess_* live in
//    platform-only headers (binder_manager.h, binder_process.h) that the NDK does NOT
//    ship, so this branch cannot be compiled here.
//  * NDK/app build (what we compile): mlab_new_modem_binder() hands the binder to
//    Java via JNI (AIBinder_toJavaBinder), the way an app's bound Service returns a
//    native IBinder from onBind().
#include <aidl/com/mlab/modem/BnModemControl.h>
#include <aidl/com/mlab/modem/IModemIndication.h>
#ifdef MLAB_PLATFORM_BUILD
#include <android/binder_manager.h>
#include <android/binder_process.h>
#endif

#include "modem_hal.hpp"

#include <thread>

using aidl::com::mlab::modem::BnModemControl;
using aidl::com::mlab::modem::IModemIndication;

class ModemControlService : public BnModemControl {
public:
    explicit ModemControlService(mlab::ModemHal& hal) : hal_(hal) {}

    ndk::ScopedAStatus getRevision(std::string* out) override {
        auto v = hal_.revision();
        if (!v) return ndk::ScopedAStatus::fromServiceSpecificError(1);
        *out = *v;
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus getImei(std::string* out) override {
        auto v = hal_.imei();
        if (!v) return ndk::ScopedAStatus::fromServiceSpecificError(1);
        *out = *v;
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus getSignalDbm(int32_t* out) override {
        auto v = hal_.signal_dbm();
        if (!v) return ndk::ScopedAStatus::fromServiceSpecificError(1);
        *out = *v;
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus setIndicationCallback(const std::shared_ptr<IModemIndication>& cb) override {
        cb_ = cb;
        return ndk::ScopedAStatus::ok();
    }
    std::shared_ptr<IModemIndication> cb() const { return cb_; }

private:
    mlab::ModemHal& hal_;
    std::shared_ptr<IModemIndication> cb_;
};

// NDK path: returns a +1 strong ref; caller (JNI) converts it with AIBinder_toJavaBinder.
extern "C" AIBinder* mlab_new_modem_binder(mlab::ModemHal* hal) {
    auto svc = ndk::SharedRefBase::make<ModemControlService>(*hal);
    AIBinder* b = svc->asBinder().get();
    AIBinder_incStrong(b);
    return b;
}

#ifdef MLAB_PLATFORM_BUILD
int main(int argc, char** argv) {
    mlab::ModemHal hal;
    if (hal.open(argc > 1 ? argv[1] : "/dev/socket/modem_sim") != 0) return 1;
    auto svc = ndk::SharedRefBase::make<ModemControlService>(hal);
    std::string name = std::string(ModemControlService::descriptor) + "/default";
    if (AServiceManager_addService(svc->asBinder().get(), name.c_str()) != STATUS_OK) return 1;
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();
    // Indication pump: forward modem NAS reports to whoever registered a callback.
    for (;;) hal.poll_indications(1000, [&](int dbm) { if (auto c = svc->cb()) c->signalChanged(dbm); });
}
#endif
