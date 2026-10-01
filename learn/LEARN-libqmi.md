# LEARN: libqmi interoperability

## 1. What

libqmi (`libqmi-glib`) is the open-source QMI library from the freedesktop project. ModemManager and `qmicli` are built on it. It has its own encoder, parser, and a pretty-printer generated from the real message definitions.

## 2. Why it is used here

A codec that only passes **its own** tests can be wrong in a self-consistent way: the encoder and the decoder share the same mistake. Checking against an **independent implementation** catches that. This is the reason `MLAB_REQUIRE_LIBQMI=ON` makes the build fail when libqmi is missing.

## 3. How it works

```
our builder (lib/qmi)  --raw bytes-->  qmi_message_new_from_raw()  (libqmi parser)
                                       -> get_service / get_client_id / get_transaction_id
                                       -> qmi_message_tlv_read_*   (TLV readers)
                                       -> get_printable_full()     ("Get Revision" (0x0023) ...)

libqmi qmi_message_new() + tlv_write_*  --raw bytes-->  our qmi_parse()
```

Things that came up while writing it:

- libqmi keeps its `QMI_MESSAGE_*` enums private, so the test uses raw hex message ids.
- libqmi masks the IMEI as `###` in printable output because it is personal data.
- GLib has intentional process-lifetime allocations, so they are suppressed for LeakSanitizer (`tests/lsan.supp`).
- libqmi's headers trip `-Werror=deprecated-declarations`, which is relaxed for that one target only (`CMakeLists.txt:85`).

## 4. Where in this repo

| item | location |
|---|---|
| test program | `tests/test_libqmi_interop.c` |
| our bytes into libqmi | `:24` (`to_libqmi`) |
| CTL AllocCID request checks | `:51-66` |
| DMS revision response checks | `:69-100` |
| IDs + NAS indication | `:103-137` |
| libqmi bytes into our parser | `:140-165` |
| CMake wiring | `CMakeLists.txt:78-92` |
| image providing libqmi 1.32.2 | `Dockerfile` (`libqmi-glib-dev`) |

## 5. How it is verified

```bash
./run-tests.sh      # ctest entry: test_libqmi_interop, in both the normal and ASan builds
```

Rerun 2026-10-01: passed.

## 6. Real vs simulated

It runs real upstream libqmi against our bytes. While writing these notes I also checked libqmi's `qmi-errors.h` in the image: `INVALID_CLIENT_ID = 7`. That confirmed the user-space value and exposed the wrong 0x22 in the kernel fake, which libqmi never sees.

## 7. Interview Q&A

1. **Why test against another implementation?** To catch symmetric bugs that a self round-trip test cannot see.
2. **How is libqmi's API organised?** There is a generic `QmiMessage` (raw TLV read/write) and generated per-service request/response types (`qmi_client_dms_get_revision`) on top of `QmiDevice`/`QmiClient` GObjects.
3. **How does ModemManager open a modem?** Through `QmiDevice` on `/dev/cdc-wdm0` or `/dev/wwan0qmi0`. Optionally it goes through `qmi-proxy`, so several processes can share a device.
4. **What is qmi-proxy for?** It multiplexes one char device between several libqmi users. This is the same problem client ids solve at the protocol level.
5. **Why suppress GLib leaks?** GType registrations are never freed by design. They are not bugs in our code.
6. **qmicli example?** `qmicli -d /dev/wwan0qmi0 --dms-get-revision`. It sends the same frame that `test_qmi` checks byte by byte.

## 7b. Useful commands (would need real hardware; not run here)

```bash
qmicli -d /dev/cdc-wdm0 --dms-get-ids
qmicli -d /dev/cdc-wdm0 --nas-get-signal-strength
qmicli -d /dev/cdc-wdm0 --client-no-release-cid --nas-noop   # keep the cid
```

## 8. Try it yourself

1. Add a case that sends a NAS **Get Serving System** request built by us and checks libqmi's printable output.
2. Use libqmi's generated `QmiMessageDmsGetRevisionOutput` parsing on a response from our simulator, instead of raw TLV readers.
3. Write a tiny GLib program that uses `QmiDevice` over a pipe or pty to talk to `modem_sim`. Hint: libqmi expects a char device, so a socat pty bridge is needed.
