# LEARN: Android NDK, AIDL/binder, JNI and the RIL

## 1. What

| term | meaning |
|---|---|
| **NDK** | Google's toolchain for building C/C++ for Android: clang plus the **bionic** libc |
| **AIDL** | Android's interface language. `aidl --lang=ndk` generates C++ proxies (`Bp*`) and stubs (`Bn*`) that talk over **binder** IPC |
| **JNI** | the bridge between Java/Kotlin and native code (`Java_pkg_Class_method` symbols) |
| **RIL** | the Radio Interface Layer. The telephony framework calls the **IRadio** AIDL HAL, which a vendor RIL daemon implements by talking QMI to the modem |

## 2. Why it exists

On a phone, `TelephonyManager` (Java) has to reach a modem that only speaks QMI. The path is Java, then binder (AIDL HAL), then the vendor native daemon, then QMI over `/dev/...` or QRTR. Netd configures the data netdev with netlink and ioctl. This lab's `modem_manager` plays the vendor-daemon role.

## 3. How it works

```
 Java ModemControl  --JNI-->  libmodemjni.so (modem_jni.cpp)
        |                          |
        | asBinder()               v
        |                     ModemHal (sync facade: lock, pump EventLoop until reply)
        v                          |
 IModemControl (AIDL, @VintfStability)   QmiClient (async epoll) --QMI--> modem_sim
   BnModemControl <- ModemControlService (modem_control_service.cpp)
 IModemIndication (oneway) <- signalChanged(dbm)   like IRadioNetworkIndication
```

### Blocking API on an async core

A binder or JNI call is blocking ("get the IMEI"), but QmiClient works with callbacks. `ModemHal::call` sends the request, then runs `loop.run_until(done, 3s)` (`android/hal/modem_hal.cpp:38-48`). A mutex serialises callers, because binder threads are many and the loop has only one thread.

### Static vs dynamic bionic binaries

```
static:  all of bionic linked in; needs only the kernel ABI -> runs on any arm64 Linux kernel
dynamic: PT_INTERP = /system/bin/linker64 + libc.so from /system -> needs Android userspace
```

### A real porting finding

bionic uses the raw kernel-uapi `NLMSG_OK`, which compares a u32 with an int. glibc's copy of the header casts. So `-Wsign-compare` fires only on Android, and the build waives it for `lib/rtnl/rtnl.c` only (`android/build-android.sh:37`).

## 4. Where in this repo

| item | location |
|---|---|
| build script (two containers) | `android/build-android.sh` |
| NDK toolchain image (amd64 under qemu) | `android/Dockerfile.ndk` |
| static cross-builds | `android/build-android.sh:42-44` |
| dynamic build | `android/build-android.sh:46` |
| AIDL compile | `android/build-android.sh:49` |
| JNI header generation | `android/build-android.sh:57` |
| AIDL interfaces | `android/aidl/com/mlab/modem/IModemControl.aidl`, `IModemIndication.aidl` |
| service impl | `android/hal/modem_control_service.cpp:27` |
| platform-only `main()` (not built) | `android/hal/modem_control_service.cpp:68-80` (`#ifdef MLAB_PLATFORM_BUILD`) |
| sync facade | `android/hal/modem_hal.hpp`, `android/hal/modem_hal.cpp` |
| JNI | `android/jni/modem_jni.cpp:19` (open), `:43` (asBinder) |
| Java side | `android/jni/java/com/mlab/modem/ModemControl.java` |
| runnable smoke test | `android/hal/hal_smoke.cpp` |

## 5. How it is verified

```bash
MLAB_ANDROID=1 ./run-tests.sh          # or ./android/build-android.sh  (./android/build-android.sh build = no run)
```

The run step uses a plain `alpine:3.17` arm64 container:

- static `hal_smoke` must print `HAL_SMOKE PASS`
- static `modem_manager` must finish with `errors=0`
- `modem_manager.dynamic` is expected to fail with exit 127

Status: passed at `1837752`. **Not rerun** while writing these notes.

## 6. Real vs simulated vs blocked

| piece | status |
|---|---|
| NDK r27c cross-compile with `-Werror` | real, ran |
| static bionic binaries executed | real, ran on plain arm64 Linux, **not on Android** |
| dynamic bionic binary | built; fails to run (no `/system/bin/linker64`), as expected |
| AIDL stubs + `BnModemControl` service | compiled only; never executed (no binder driver in the VM kernel) |
| `libmodemjni.so` | compiled and linked (`--no-undefined`); never loaded (no ART) |
| servicemanager registration | not built: `binder_manager.h`/`binder_process.h` are platform-only, not in the NDK |
| redroid | blocked: no binder in the VM kernel |
| Android emulator | blocked: needs KVM/HVF and system images |
| real IRadio HAL / rild | not implemented (needs an AOSP tree) |

## 7. Interview Q&A

1. **Why are indications `oneway`?** The HAL must never block on the framework. A slow app would otherwise stall the modem path.
2. **What does `@VintfStability` mean?** The interface crosses the system/vendor boundary. It is frozen and versioned so the two sides can be updated independently.
3. **How do you call a blocking HAL method on an async modem client?** Use a sync facade that pumps the event loop under a mutex, or a dedicated loop thread with futures.
4. **Static vs dynamic bionic?** Static runs on any Linux kernel with the same ABI. Dynamic needs the Android linker and system libraries.
5. **How is a JNI symbol name formed?** `Java_<package with _>_<Class>_<method>`. `javac -h` generates the matching header.
6. **What does `AIBinder_toJavaBinder` do?** It converts an NDK binder into an `android.os.IBinder` jobject, for example to return it from `Service.onBind`.
7. **RIL vs this lab?** The vendor RIL does what `modem_manager` does: QMI transactions, mapping indications to `unsol` callbacks, and working with netd for the data interface.
8. **NDK vs platform (AOSP) builds?** The NDK exposes a stable public API only. Service registration and many HAL headers need a platform or vendor build.

## 8. Try it yourself

1. If you have a device or a KVM-capable emulator: `adb push android/out/bin/{modem_sim,hal_smoke} /data/local/tmp`, then run both. This is the first time it would run on actual Android.
2. Write a Kotlin app that loads `libmodemjni.so` and calls `getRevision()` against `modem_sim` started via `adb shell`.
3. Add `getSignalInfo()` returning a parcelable `{int dbm, int radioTech}` to the AIDL, regenerate it, and implement it.
4. Compare `IModemControl` with AOSP's `hardware/interfaces/radio/aidl/android/hardware/radio/network/IRadioNetwork.aidl`. Which methods map to QMI NAS?
