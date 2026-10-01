#!/usr/bin/env bash
# Cross-compile the lab for Android arm64 (bionic) with the NDK, compile the
# AIDL HAL + JNI bridge, then RUN what can run on this machine.
#
#   ./android/build-android.sh          # build + run
#   ./android/build-android.sh build    # build only
#
# Why two containers:
#   * build: NDK clang and aidl are x86_64-only Linux binaries -> linux/amd64 image,
#     emulated by qemu on the arm64 Rancher VM (slow but real).
#   * run:   the outputs are aarch64 bionic binaries; the VM kernel IS aarch64 Linux,
#     and Android is Linux, so a STATICALLY linked bionic binary runs natively in a
#     plain arm64 container. Dynamic ones need /system/bin/linker64 + bionic .so's
#     (i.e. a real Android userspace), which is the part we cannot provide here.
set -euo pipefail
cd "$(dirname "$0")/.."
export DOCKER_HOST="${DOCKER_HOST:-unix://$HOME/.rd/docker.sock}"
IMG=mlab-android-ndk

docker build --platform linux/amd64 -q -t "$IMG" -f android/Dockerfile.ndk android >/dev/null

docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src "$IMG" bash -euo pipefail -c '
API=30          # static executables: any API works; 30 = Android 11
API_BINDER=34   # libbinder_ndk service APIs (AServiceManager_addService) need >= 29
CC="aarch64-linux-android$API-clang";  CXX="aarch64-linux-android$API-clang++"
W="-Wall -Wextra -Werror -Wshadow -O2 -g"
INC="-Ilib/qmi -Ilib/rtnl -Ilib/ifctl -Isim -Idaemon -Iandroid/hal"
O=android/out; rm -rf $O; mkdir -p $O/obj $O/bin $O/lib $O/gen $O/classes
echo "== NDK: $(cat $NDK/source.properties | tr "\n" " ")"

echo "== [a] C libraries + sim + daemon (bionic, static)"
# Real portability hit: bionic uses the raw kernel uapi NLMSG_OK, which compares
# nlmsg_len (u32) with an int len; the glibc copy of the header casts it. Same code,
# different libc headers -> -Wsign-compare fires only on Android. Scoped waiver:
for f in lib/qmi/qmi.c lib/ifctl/ifctl.c sim/sim_core.c; do
  $CC -std=c11 $W $INC -fPIC -c $f -o $O/obj/$(basename $f .c).o; done
$CC -std=c11 $W -Wno-sign-compare $INC -fPIC -c lib/rtnl/rtnl.c -o $O/obj/rtnl.o
for f in daemon/event_loop.cpp daemon/frame_buffer.cpp daemon/qmi_client.cpp daemon/link_monitor.cpp android/hal/modem_hal.cpp; do
  $CXX -std=c++17 $W $INC -fPIC -c $f -o $O/obj/$(basename $f .cpp).o; done
C_OBJS="$O/obj/qmi.o $O/obj/rtnl.o $O/obj/ifctl.o"
MM_OBJS="$O/obj/event_loop.o $O/obj/frame_buffer.o $O/obj/qmi_client.o $O/obj/link_monitor.o"
$CC  -std=c11   $W $INC -static sim/modem_sim.c $O/obj/sim_core.o $O/obj/qmi.o -o $O/bin/modem_sim
$CXX -std=c++17 $W $INC -static daemon/modem_manager.cpp $MM_OBJS $C_OBJS -o $O/bin/modem_manager
$CXX -std=c++17 $W $INC -static android/hal/hal_smoke.cpp $O/obj/modem_hal.o $MM_OBJS $C_OBJS -o $O/bin/hal_smoke
# and one ordinary dynamic (PIE, linker64) build, to show what a device build looks like
$CXX -std=c++17 $W $INC -static-libstdc++ daemon/modem_manager.cpp $MM_OBJS $C_OBJS -o $O/bin/modem_manager.dynamic

echo "== [c1] AIDL -> NDK-backend C++ (real aidl compiler from build-tools r34)"
aidl --lang=ndk --structured --stability=vintf -I android/aidl -h $O/gen/include -o $O/gen \
     android/aidl/com/mlab/modem/IModemIndication.aidl android/aidl/com/mlab/modem/IModemControl.aidl
CXXB="aarch64-linux-android$API_BINDER-clang++"
for f in $O/gen/com/mlab/modem/*.cpp android/hal/modem_control_service.cpp; do
  $CXXB -std=c++17 $W -Wno-unused-parameter $INC -I$O/gen/include -fPIC -c $f -o $O/obj/aidl_$(basename $f .cpp).o; done
echo "   (servicemanager registration main() NOT built: needs platform-only binder_manager.h)"

echo "== [c2] JNI: javac -h generates the C header; libmodemjni.so must match it"
javac -h $O/gen/jni -d $O/classes android/jni/java/com/mlab/modem/ModemControl.java
$CXXB -std=c++17 $W $INC -I$O/gen/jni -I$O/gen/include -shared -fPIC -static-libstdc++ -Wl,--no-undefined \
  android/jni/modem_jni.cpp $O/obj/aidl_*.o $O/obj/modem_hal.o $MM_OBJS $C_OBJS -lbinder_ndk -o $O/lib/libmodemjni.so

echo "== results"
for b in $O/bin/* $O/lib/*; do file -b "$b" | sed "s|^|$(basename $b): |" | cut -c1-170; done
echo "-- interpreter of dynamic build: $(readelf -l $O/bin/modem_manager.dynamic | grep -o "/system/bin/linker64")"
echo "-- NEEDED of libmodemjni.so: $(readelf -d $O/lib/libmodemjni.so | grep NEEDED | grep -o "\[.*\]" | tr "\n" " ")"
echo "-- JNI exports:        $(llvm-nm -D --defined-only $O/lib/libmodemjni.so | grep -c Java_com_mlab) Java_* symbols"
echo "-- bionic proof (static modem_sim contains bionic libc symbols): $(llvm-nm $O/bin/modem_sim | grep -c " __libc_init\$\| __system_property_get\$")"
'
[ "${1:-}" = build ] && exit 0

echo; echo "== [b] RUN on arm64 Linux kernel ($(docker info --format "{{.KernelVersion}}")), plain alpine, NO Android userspace"
docker run --rm --cap-add NET_ADMIN -v "$PWD/android/out/bin":/b:ro alpine:3.17 sh -c '
set -u
uname -m
/b/modem_sim --socket /tmp/m.sock --ind-ms 300 & SIM=$!; sleep 0.3
echo "-- static bionic hal_smoke (the object behind JNI + AIDL):"; /b/hal_smoke /tmp/m.sock; HS=$?
IF=""; ip link add wwan0 type dummy 2>/dev/null && IF="--iface wwan0 --bring-up"
echo "-- static bionic modem_manager ${IF:-(no dummy iface: busybox ip lacks dummy type)}:"
/b/modem_manager --socket /tmp/m.sock $IF --run-ms 1200; MM=$?
kill $SIM; wait $SIM 2>/dev/null
echo "-- dynamic bionic build (expect failure: no /system/bin/linker64 here):"
/b/modem_manager.dynamic --socket /tmp/m.sock >/tmp/d.log 2>&1; echo "   exit=$? $(head -c 200 /tmp/d.log)"
echo "-- binder: $(ls /dev/binder /dev/binderfs 2>&1 | head -1); filesystems: $(grep -c binder /proc/filesystems) binder entries"
echo "RESULT hal_smoke=$HS modem_manager=$MM"
[ $HS = 0 ] && [ $MM = 0 ]'
