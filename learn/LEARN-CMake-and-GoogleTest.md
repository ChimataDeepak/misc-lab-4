# LEARN: CMake, GoogleTest and ctest

## 1. What

- **CMake** describes *targets* (libraries and executables) and how they depend on each other. It then generates the actual build (Makefiles or Ninja).
- **GoogleTest** is a C++ unit-test framework (`TEST`, `TEST_F`, `EXPECT_EQ`).
- **ctest** runs every registered test, whether it is a gtest binary, a shell script or a plain C program, and reports pass or fail.

## 2. Why it exists

You need one reproducible command that builds everything with the same flags and runs every kind of test. CMake is the de facto standard for native C/C++ code on Linux, and it is also used by Android NDK projects.

## 3. How it works

```
CMakeLists.txt
  qmi(C)  rtnl(C)  ifctl(C)  simcore(C) --links--> qmi
                                  mmcore(C++) --links--> qmi rtnl
  modem_sim   = sim/modem_sim.c   + simcore
  modem_manager = daemon/modem_manager.cpp + mmcore + ifctl
  test_* (gtest) --links--> GTest::gtest_main + the lib under test
  e2e_daemon / strace_ioctl = shell scripts given $<TARGET_FILE:...> paths
  test_libqmi_interop (C) --links--> simcore + PkgConfig::QMIGLIB
```

Key mechanisms:

- `target_include_directories(qmi PUBLIC lib/qmi)` (`CMakeLists.txt:23`): **PUBLIC** passes the include path on to every target that links `qmi`.
- **Generator expressions**: `$<TARGET_FILE:modem_sim>` (`CMakeLists.txt:67`) is filled in at build time with the binary's real path.
- `mlab_gtest(name libs...)` (`CMakeLists.txt:54-59`) is a helper function. It adds the executable, links gtest, and calls `add_test`.
- **Options**: `MLAB_SANITIZE` (`CMakeLists.txt:15-19`) adds `-fsanitize=address,undefined`. `MLAB_REQUIRE_LIBQMI` (`:14`, `:89`) makes configure **fail** if libqmi is missing, so the interop test cannot be skipped without anyone noticing.
- `RUN_SERIAL` (`CMakeLists.txt:75`): tests that create the `wwan0`/dummy links must not run at the same time.

### A gtest fixture with a socketpair

```
TEST_F(SocketpairTest, OutOfOrderResponsesMatchByTxnId)
   SetUp(): socketpair(sv)  -> sv[0] = QmiClient,  sv[1] = "the modem" (the test)
   test writes response bytes into sv[1] in whatever order it wants
   loop.run_until(pred) -> asserts on the callbacks
```

`tests/test_qmi_client_socketpair.cpp:23` and `:95`.

## 4. Where in this repo

| item | location |
|---|---|
| languages/standards | `CMakeLists.txt:2-8` |
| warnings as errors | `CMakeLists.txt:11` |
| sanitizer option | `CMakeLists.txt:15-19` |
| libraries | `CMakeLists.txt:22-45` |
| gtest helper | `CMakeLists.txt:54-59` |
| tests registered | `CMakeLists.txt:61-75` |
| optional libqmi (pkg-config) | `CMakeLists.txt:78-92` |
| test list (10 ctest entries) | README.md table |

## 5. How it is verified

```bash
./run-tests.sh
# expected, twice:  100% tests passed, 0 tests failed out of 10
```

Rerun on 2026-10-01: both passes printed that line.

By hand inside the container:

```bash
docker run --rm -it --cap-add NET_ADMIN --cap-add SYS_PTRACE misc-lab-4 bash
cmake -S . -B build -DMLAB_REQUIRE_LIBQMI=ON && cmake --build build -j
cd build && ctest -N               # list the tests
ctest -R socketpair -V             # run one test, verbose
./test_qmi --gtest_filter='QmiDecode.*'
```

## 6. Real vs simulated

Everything here is real.

## 7. Interview Q&A

1. **PUBLIC vs PRIVATE vs INTERFACE?** PRIVATE: only this target uses it. INTERFACE: only targets that depend on this one use it. PUBLIC: both.
2. **What is a generator expression?** A value CMake works out at generation or build time, such as a target's output path or the configuration type.
3. **Why fail configure when an optional dependency is missing?** So that "0 failures" cannot secretly mean "the test never ran".
4. **TEST vs TEST_F?** TEST_F uses a fixture class with `SetUp`/`TearDown` for shared setup, such as the socketpair.
5. **EXPECT vs ASSERT?** EXPECT records a failure and keeps going. ASSERT returns from the test immediately.
6. **How do you avoid flaky I/O tests?** Use socketpairs and scripted bytes instead of real timing, and set RUN_SERIAL on tests that share a global resource such as an interface name.
7. **Why build twice in CI?** The ASan/UBSan build catches memory and undefined-behaviour bugs that the optimized build hides.

## 8. Try it yourself

1. Add `mlab_gtest(test_ifctl_unit ifctl)` with one test for name-length errors.
2. Add a `MLAB_TSAN` option with `-fsanitize=thread`. Note that it cannot be combined with ASan.
3. Use `gtest_discover_tests` instead of `add_test` so that every TEST becomes its own ctest entry. Compare the `ctest -N` output.
