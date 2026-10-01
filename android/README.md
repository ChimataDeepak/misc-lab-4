# Android part: NDK cross-build, bionic run, AIDL HAL + JNI bridge

`./android/build-android.sh` (or `... build` to skip the run step).

| step | what happens | status |
|---|---|---|
| toolchain | `Dockerfile.ndk`: NDK r27c + build-tools r34 (`aidl`) + JDK 17, **linux/amd64** image run under qemu on the arm64 Rancher VM (Google ships these as x86_64 only) | real |
| (a) cross-compile | lib/qmi, rtnl, ifctl, sim, daemon -> `aarch64-linux-android30-clang`, `-Werror`, **static bionic** `modem_sim`, `modem_manager`, `hal_smoke`; plus a normal dynamic PIE (`interpreter /system/bin/linker64`) | real |
| (b) run | static bionic binaries run **natively** in a plain arm64 alpine container (Android is Linux; a static binary needs only the kernel ABI): full QMI bring-up, ioctl UP/MTU on a dummy `wwan0`, RTNETLINK events, NAS indications | real |
| (b) blocked | dynamic bionic binary: `exit 127 not found` (no `/system/bin/linker64`). Emulator/redroid: VM kernel has no binder (`/dev/binder*` absent, none in `/proc/filesystems`, no `/lib/modules`), so redroid cannot boot; the Android emulator needs KVM/HVF + x86 or arm64 system images, not available inside this Docker VM | blocked |
| (c) AIDL | `aidl/IModemControl.aidl` + `IModemIndication.aidl` (`@VintfStability`, oneway indications) -> real `aidl --lang=ndk` -> C++ stubs compiled; `hal/modem_control_service.cpp` implements `BnModemControl` over `ModemHal` | compiled |
| (c) not built | the servicemanager `main()` (behind `MLAB_PLATFORM_BUILD`): `binder_manager.h`/`binder_process.h` are platform-only, not in the NDK | blocked by design |
| (c) JNI | `jni/java/.../ModemControl.java` -> `javac -h` header -> `libmodemjni.so` (`-Wl,--no-undefined`, links `libbinder_ndk`); `nativeAsBinder` returns the AIDL binder via `AIBinder_toJavaBinder` | compiled, not executed (needs ART + bionic) |

Porting finding: `lib/rtnl/rtnl.c` fails `-Wsign-compare` only under bionic, because bionic uses the raw
kernel-uapi `NLMSG_OK` (u32 vs int compare) while glibc's copy casts. Scoped `-Wno-sign-compare` for that file.

`hal/modem_hal.*` is the key idea: binder/JNI calls are **blocking**, QmiClient is **async on epoll**,
so the facade pumps the event loop until that transaction's reply arrives (a mutex serialises binder threads).
