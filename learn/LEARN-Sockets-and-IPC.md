# LEARN: Unix-domain sockets and IPC framing

## 1. What

An `AF_UNIX` socket connects two processes on the same machine through a filesystem path (here `/tmp/m.sock`). `SOCK_STREAM` gives you a reliable, ordered **stream of bytes**, just like TCP.

## 2. Why it exists here

A real QMI modem appears as a character device: `/dev/cdc-wdm0` over USB, or `/dev/wwan0qmi0` over MHI/PCIe. You `write()` frames to it and `read()` frames from it. The lab uses a Unix socket as that "device" so it can run anywhere, with no hardware. Android also uses Unix sockets for IPC: rild's sockets and netd. Only binder is different.

## 3. How it works

### Server and client calls

```
modem_sim (server)                               modem_manager (client)
socket(AF_UNIX, SOCK_STREAM|NONBLOCK)            socket(AF_UNIX, SOCK_STREAM)
unlink(path); bind(path); listen(8)              connect(path)
epoll: listen fd readable -> accept4(NONBLOCK)   fcntl(O_NONBLOCK); add to epoll
recv/send on the client fd                       send(MSG_NOSIGNAL)/recv
```

### Why framing is needed

A stream has **no message boundaries**:

```
modem writes:  [frame A 20B][frame B 30B]
client recv(): [A 12B] [A 8B + B 30B]    <- any split is legal
```

So the reader accumulates bytes and only takes a frame out once it is complete:

```
FrameBuffer::append(bytes)
FrameBuffer::next():
   fl = qmi_frame_length(p, avail)   // needs >=3 bytes: 0x01 + LE16 len
   fl < 0  -> garbage: skip ONE byte, look for the next 0x01   (resync)
   fl == 0 or fl > avail -> partial: wait for more bytes
   else    -> return a frame of fl bytes
```

### Partial writes

`send()` can accept fewer bytes than you asked it to. The rest stays in `tx_`, and EPOLLOUT is switched on only while that backlog exists (`daemon/qmi_client.cpp:148-164`).

### `MSG_NOSIGNAL`

Writing to a socket whose peer has closed raises **SIGPIPE**, and by default that kills the process. `MSG_NOSIGNAL` (`daemon/qmi_client.cpp:150`) turns it into an `EPIPE` error, which the code handles by failing every pending request.

### `socketpair()` for deterministic tests

`socketpair(AF_UNIX, SOCK_STREAM)` creates two connected ends inside one process. The test plays the modem and decides exactly which bytes arrive and when (`tests/test_qmi_client_socketpair.cpp:23`).

## 4. Where in this repo

| item | location |
|---|---|
| server socket, bind, listen | `sim/modem_sim.c:172-181` |
| accept4 loop | `sim/modem_sim.c:221` |
| server-side framing and resync | `sim/modem_sim.c:98-138` (resync at `:117`) |
| client connect | `daemon/qmi_client.cpp:36-53` |
| nonblocking client | `daemon/qmi_client.cpp:56` |
| FrameBuffer | `daemon/frame_buffer.cpp:7-32` (resync at `:21`) |
| partial-write backlog | `daemon/qmi_client.cpp:148-164` |
| EOF -> fail all pending | `daemon/qmi_client.cpp:178-182`, `:210` |

## 5. How it is verified

```bash
./run-tests.sh
```

The relevant tests:

- `test_frame_buffer`: ByteByByte, TwoAndAHalfFramesInOneRead, ResyncsAfterGarbage (`tests/test_frame_buffer.cpp:21,35,49`).
- `test_qmi_client_socketpair`: ResponseSplitAcrossReadsIsReassembled (`:75`) and PeerCloseFailsAllPending (`:156`).
- `test_e2e_sim`: a real `modem_sim` process.

## 6. Real vs simulated

The sockets are real. The **device** is simulated: on hardware, the fd would come from `open("/dev/wwan0qmi0")`, and the framing and transaction code would stay the same. One difference: the real char device delivers whole frames per `read()`. The lab's FrameBuffer is stricter than it needs to be for that case, which does no harm.

## 7. Interview Q&A

1. **TCP-like stream vs datagram for IPC?** A stream needs framing. `SOCK_SEQPACKET` on AF_UNIX keeps message boundaries and is still reliable.
2. **What does `recv` returning 0 mean?** The peer closed the connection (EOF). Fail everything that is pending.
3. **How do you resync after corrupt bytes?** Drop one byte and search for the next marker. Never drop the whole buffer, because a good frame may follow.
4. **Why `unlink()` before `bind()`?** A leftover socket file from a crashed run would make bind fail with EADDRINUSE.
5. **SIGPIPE?** Use `MSG_NOSIGNAL` or ignore SIGPIPE. Otherwise a disconnected peer kills your daemon.
6. **Why does `accept4` loop until EAGAIN?** On a nonblocking listener, one readiness event can stand for several pending connections.
7. **How do you test IPC deterministically?** Use a socketpair and scripted writes, with no sleeps.

## 8. Try it yourself

1. Add a `--chunk N` option to `modem_sim` that sends replies N bytes at a time. Run `e2e_daemon` with N=1.
2. Switch the transport to `SOCK_SEQPACKET`. Which parts of FrameBuffer become unnecessary?
3. Use `SO_PEERCRED` in the simulator to log the client's pid/uid. rild-style daemons use this for access control.
