// JNI bridge: Java ModemControl <-> C++ ModemHal. Built into libmodemjni.so.
// Signatures are checked at build time against the header javac -h generates.
#include <android/binder_ibinder_jni.h>
#include <jni.h>

#include "com_mlab_modem_ModemControl.h"
#include "modem_hal.hpp"

extern "C" AIBinder* mlab_new_modem_binder(mlab::ModemHal* hal);

static mlab::ModemHal* H(jlong h) { return reinterpret_cast<mlab::ModemHal*>(h); }

static jstring to_j(JNIEnv* env, const std::optional<std::string>& v) {
    return v ? env->NewStringUTF(v->c_str()) : nullptr;  // null -> Java sees null
}

extern "C" {

JNIEXPORT jlong JNICALL Java_com_mlab_modem_ModemControl_nativeOpen(JNIEnv* env, jclass, jstring path) {
    const char* p = env->GetStringUTFChars(path, nullptr);
    auto* hal = new mlab::ModemHal();
    int rc = hal->open(p);
    env->ReleaseStringUTFChars(path, p);
    if (rc != 0) { delete hal; return 0; }
    return reinterpret_cast<jlong>(hal);
}
JNIEXPORT jstring JNICALL Java_com_mlab_modem_ModemControl_nativeGetRevision(JNIEnv* env, jclass, jlong h) {
    return to_j(env, H(h)->revision());
}
JNIEXPORT jstring JNICALL Java_com_mlab_modem_ModemControl_nativeGetImei(JNIEnv* env, jclass, jlong h) {
    return to_j(env, H(h)->imei());
}
JNIEXPORT jint JNICALL Java_com_mlab_modem_ModemControl_nativeGetSignalDbm(JNIEnv* env, jclass cls, jlong h) {
    auto v = H(h)->signal_dbm();
    if (!v) {
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), "NAS GetSignalStrength failed");
        return 0;
    }
    (void)cls;
    return *v;
}
// The same HAL, exposed as an AIDL IModemControl binder (android.os.IBinder in Java).
JNIEXPORT jobject JNICALL Java_com_mlab_modem_ModemControl_nativeAsBinder(JNIEnv* env, jclass, jlong h) {
    AIBinder* b = mlab_new_modem_binder(H(h));
    jobject j = AIBinder_toJavaBinder(env, b);
    AIBinder_decStrong(b);
    return j;
}
JNIEXPORT void JNICALL Java_com_mlab_modem_ModemControl_nativeClose(JNIEnv*, jclass, jlong h) { delete H(h); }

}  // extern "C"
