# LEARN: the QMI protocol (QMUX + SDU + TLV)

## 1. What

QMI (Qualcomm MSM Interface) is the binary protocol a host uses to control a Qualcomm modem. Messages come in three kinds:

- **requests** (host to modem)
- **responses** (modem to host, matched to a request)
- **indications** (modem to host, unsolicited)

Messages are grouped into **services**: CTL = 0, DMS = 2 (device management), NAS = 3 (network access), WDS = 1 (data calls), UIM (SIM) and so on.

## 2. Why it exists

It is the control plane of most Linux and Android cellular devices. ModemManager/libqmi, qmicli and vendor RILs all speak it. The protocol has a few quirks, and getting them wrong causes bugs that are hard to see.

## 3. How it works: byte layout

```
QMUX header (6 bytes)
 off 0  u8   0x01 marker (if_type)
 off 1  u16  length LE = total frame bytes - 1  (everything except the marker)
 off 3  u8   ctl flags: 0x00 from host, 0x80 from service (modem)
 off 4  u8   service
 off 5  u8   client id
SDU header
 CTL (svc 0):  u8 flags | u8  txn      | u16 msg id | u16 tlv_len   (6 bytes)
 others:       u8 flags | u16 txn (LE) | u16 msg id | u16 tlv_len   (7 bytes)
 flags:        CTL: 0 req, 1 rsp, 2 ind      services: 0 req, 2 rsp, 4 ind
TLVs (repeated): u8 type | u16 len LE | value[len]
Every response has TLV 0x02 = { u16 result (0 ok / 1 fail), u16 error }
```

### Worked example: CTL Allocate Client ID for DMS, txn 7

This is the exact byte array in `tests/test_qmi.cpp:21-24`:

```
01 0F 00 00 00 00 | 00 07 22 00 04 00 | 01 01 00 02
^  ^^^^^ ^  ^  ^    ^  ^  ^^^^^ ^^^^^   ^  ^^^^^ ^
|  len=15 |  |  cid  |  txn msg=0x22 tlv  T  L=1   V=DMS(2)
marker  host CTL    req      len=4
```

### The three gotchas

1. The txn id is 1 byte for CTL and 2 bytes for every other service (`lib/qmi/qmi.c:64`).
2. The response and indication flag values differ between CTL and services (`lib/qmi/qmi.c:11-34`).
3. The QMUX length excludes only the marker, and both length fields are patched at the end (`lib/qmi/qmi.c:96-104`).

### Session flow used by the daemon

```
host                                   modem
CTL AllocCID(svc=DMS)        ------->
                             <-------  rsp {result ok, {DMS, cid=1}}
DMS GetRevision (cid 1)      ------->
                             <-------  rsp TLV 0x01 "MLAB-SIM-MODEM 1.0.0 ..."
DMS GetIDs                   ------->
                             <-------  rsp TLV 0x10 ESN 0x11 IMEI 0x12 MEID
CTL AllocCID(svc=NAS)        ------->  ... cid
NAS GetSignalStrength        ------->
                             <-------  rsp TLV 0x01 {int8 dBm, u8 radio=0x08 LTE}
                             <-------  IND NAS EventReport TLV 0x10 {dBm, radio}  (periodic)
```

### Client ids and transactions

- A client id lets several programs share one modem. Each one asks CTL for its own id.
- A request on a cid that was never allocated gets error `QMI_ERR_INVALID_CLIENT_ID` (0x0007) (`sim/sim_core.c:68-69`).
- The client matches each response by the key `(service, cid, txn)`, packed into a uint32 (`daemon/qmi_client.hpp:83`).

## 4. Where in this repo

| item | location |
|---|---|
| layout comment | `lib/qmi/qmi.h:1-26` |
| constants (services, msg ids, errors) | `lib/qmi/qmi.h:38-67` |
| encoder | `lib/qmi/qmi.c:48-104` |
| `qmi_frame_length` | `lib/qmi/qmi.c:106` |
| parser and validation | `lib/qmi/qmi.c:121-168` |
| result TLV | `lib/qmi/qmi.c:88` (build), `:186` (get) |
| simulated modem handlers | `sim/sim_core.c:37` (CTL), `:60` (DMS/NAS), `:92` (indication) |
| client txn matching | `daemon/qmi_client.cpp:68-135`, `:187-208` |
| in-kernel encoder (same bytes) | `kernel/fake_mhi_modem/fake_mhi_modem.c:95-182` |

## 5. How it is verified

```bash
./run-tests.sh
```

- `test_qmi`: exact bytes and malformed input (`tests/test_qmi.cpp:19-92`).
- `test_sim_core`: the simulator's handlers.
- `test_libqmi_interop`: an independent implementation checks the bytes (see LEARN-libqmi).
- Optional `MLAB_KERNEL=1`: the same frames through `/dev/fake_qmi0`. It passed at `1837752` and was not rerun while writing these notes.

## 6. Real vs simulated

- **Real:** the wire format, cross-checked against libqmi 1.32.2.
- **Simulated:** the modem's answers, and only five messages are implemented: CTL AllocateCID, DMS GetRevision, DMS GetIDs, NAS GetSignalStrength, and the NAS EventReport indication.
- **Not implemented:** WDS data calls, UIM, real registration, QRTR transport.
- The in-kernel fake uses the wrong code for "invalid client id": 0x22 instead of 0x07 (see learn/README.md).

## 7. Interview Q&A

1. **Why do client ids exist?** So several control points can share one modem, and responses and indications are routed to the client that asked.
2. **CTL vs service SDU differences?** CTL has a 1-byte txn and flag values 1 and 2. Services have a 2-byte txn and flag values 2 and 4.
3. **How do you match responses?** By the key `(service, cid, txn)`. The modem echoes the txn back.
4. **How do you handle a response that arrives after the timeout?** Erase the pending entry when the timeout fires, then drop the late reply, which no longer matches anything.
5. **Are strings NUL-terminated?** No. The TLV length is the string length.
6. **Indication vs response?** An indication has its own flag value and no pending entry. It goes to a separate handler.
7. **QMUX vs QRTR?** QMUX is the framed, char-device transport. QRTR (`AF_QIPCRTR`) is a socket-based router used on newer SoCs. QMI messages travel inside either one.
8. **Which parsing checks do you need?** Marker, both length fields agree, every TLV fits inside the TLV area, flag values are known, and an upper bound on frame size.

## 8. Try it yourself

1. Implement NAS **Get Serving System** (0x0024) in `sim_core.c` and the daemon, and add a byte-exact test.
2. Add WDS (svc 0x01) **Start Network Interface** (0x0020) returning a packet data handle. Then bring the interface up only after it succeeds.
3. Feed `qmi_parse` random bytes for a minute (a tiny fuzzer, or libFuzzer) under ASan.
4. Change the kernel fake's error code to 0x0007, fix its test, and rerun `MLAB_KERNEL=1 ./run-tests.sh`.
