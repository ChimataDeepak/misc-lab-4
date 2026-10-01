cmake_minimum_required(VERSION 3.20)
project(misc_lab_4_modem_control LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Warnings are errors: in C/C++ most real bugs show up as warnings first.
add_compile_options(-Wall -Wextra -Werror -Wshadow -Wformat=2 -g)

# -DMLAB_SANITIZE=ON  -> AddressSanitizer + UndefinedBehaviorSanitizer build
option(MLAB_REQUIRE_LIBQMI "Fail configure if libqmi-glib is missing" OFF)
option(MLAB_SANITIZE "Build with ASan + UBSan" OFF)
if(MLAB_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---- C libraries --------------------------------------------------------
add_library(qmi STATIC lib/qmi/qmi.c)
target_include_directories(qmi PUBLIC lib/qmi)

add_library(rtnl STATIC lib/rtnl/rtnl.c)
target_include_directories(rtnl PUBLIC lib/rtnl)

add_library(ifctl STATIC lib/ifctl/ifctl.c)
target_include_directories(ifctl PUBLIC lib/ifctl)

add_library(simcore STATIC sim/sim_core.c)
target_include_directories(simcore PUBLIC sim)
target_link_libraries(simcore PUBLIC qmi)

# ---- programs -----------------------------------------------------------
add_executable(modem_sim sim/modem_sim.c)
target_link_libraries(modem_sim PRIVATE simcore)

add_executable(ifctl_cli tools/ifctl_cli.c)
target_link_libraries(ifctl_cli PRIVATE ifctl)

add_library(mmcore STATIC
  daemon/event_loop.cpp daemon/frame_buffer.cpp daemon/qmi_client.cpp daemon/link_monitor.cpp)
target_include_directories(mmcore PUBLIC daemon)
target_link_libraries(mmcore PUBLIC qmi rtnl)

add_executable(modem_manager daemon/modem_manager.cpp)
target_link_libraries(modem_manager PRIVATE mmcore ifctl)

# ---- tests --------------------------------------------------------------
enable_testing()
find_package(GTest REQUIRED)

function(mlab_gtest name)
  add_executable(${name} tests/${name}.cpp)
  target_link_libraries(${name} PRIVATE GTest::gtest GTest::gtest_main ${ARGN})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()

mlab_gtest(test_qmi qmi)
mlab_gtest(test_sim_core simcore)
mlab_gtest(test_frame_buffer mmcore)
mlab_gtest(test_qmi_client_socketpair mmcore)
mlab_gtest(test_rtnl_parse rtnl)
mlab_gtest(test_e2e_sim mmcore simcore)
target_compile_definitions(test_e2e_sim PRIVATE MODEM_SIM_PATH="$<TARGET_FILE:modem_sim>")
add_dependencies(test_e2e_sim modem_sim)
mlab_gtest(test_netlink_ioctl rtnl ifctl)

add_test(NAME e2e_daemon COMMAND ${CMAKE_SOURCE_DIR}/tests/e2e_daemon.sh
         $<TARGET_FILE:modem_sim> $<TARGET_FILE:modem_manager>)
add_test(NAME strace_ioctl COMMAND ${CMAKE_SOURCE_DIR}/tests/strace_ioctl.sh $<TARGET_FILE:ifctl_cli>)
# These two create real interfaces; don't let ctest -j run them at the same time.
set_tests_properties(e2e_daemon strace_ioctl test_netlink_ioctl PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)

# libqmi interoperability (upstream parser). Optional so the lab still builds without it.
find_package(PkgConfig)
if(PkgConfig_FOUND)
  pkg_check_modules(QMIGLIB IMPORTED_TARGET qmi-glib)
endif()
if(QMIGLIB_FOUND)
  add_executable(test_libqmi_interop tests/test_libqmi_interop.c)
  # libqmi headers trip some of our stricter flags; that's their code, not ours.
  target_compile_options(test_libqmi_interop PRIVATE -Wno-error=deprecated-declarations)
  target_link_libraries(test_libqmi_interop PRIVATE simcore PkgConfig::QMIGLIB)
  add_test(NAME test_libqmi_interop COMMAND test_libqmi_interop)
elseif(MLAB_REQUIRE_LIBQMI)
  message(FATAL_ERROR "qmi-glib not found but MLAB_REQUIRE_LIBQMI=ON")
else()
  message(WARNING "qmi-glib not found: libqmi interop test disabled")
endif()
