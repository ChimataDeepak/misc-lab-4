# LEARN: C++17 for a stateful daemon

## 1. What

`daemon/` is C++17: `EventLoop`, `FrameBuffer`, `QmiClient`, `LinkMonitor` and `modem_manager`. It sits on top of the C libraries.

## 2. Why C++ for the daemon

A daemon keeps a lot of **state that has a lifetime**: open fds, pending requests, timers and callbacks. C++ gives you:

- **RAII**: a destructor closes the fd, so no code path leaks it.
- `std::function`: a callback that can carry its own captured data.
- `std::unordered_map`: the table of pending transactions.
- `std::optional`: "a value, or nothing", with no sentinel values.
- **Deleted copy operations**: the compiler stops you from copying something that must not be copied.

## 3. How it works

### 3.1 RAII ownership

```
QmiClient(loop, fd)          ~QmiClient()
  loop.add_fd(fd, ...)   ->    cancel timers, del_fd, close(fd)
```

See `daemon/qmi_client.cpp:55` (constructor) and `:60` (destructor). `LinkMonitor` follows the same pattern (`daemon/link_monitor.cpp:12`, `:22`).

### 3.2 Move-only `Reply`

```
Reply { std::vector<uint8_t> frame;  qmi_msg msg; }   // msg.tlvs -> frame.data()
copy:  new.frame = copy of the bytes, but new.msg.tlvs still -> OLD frame   (dangling)
move:  vector's buffer is moved, pointer stays valid                          (ok)
```

`daemon/qmi_client.hpp:41-45` defaults the move operations and **deletes** the copy operations.

### 3.3 Callbacks and lifetime

- Each handler is stored as `shared_ptr<FdHandler>`. Before the loop calls it, it copies the pointer into `keep_alive` (`daemon/event_loop.cpp:71-72`). A handler can then call `del_fd()` on itself without destroying the code that is running.
- A timer lambda captures the **map key** and not an iterator (`daemon/qmi_client.cpp:120`). A rehash invalidates iterators but not keys.
- A pending entry is erased **before** its callback runs (`daemon/qmi_client.cpp:203`). The callback may send the next request, and that may insert into the same map.

### 3.4 Continuation chain (async without threads)

```
allocate_cid(DMS) --cb--> GetRevision --cb--> GetIDs --cb--> allocate_cid(NAS)
     --cb--> GetSignalStrength --cb--> "modem ready" -> bring_up_iface()
```

`daemon/modem_manager.cpp:116` onwards. Each step starts the next one from inside its callback.

### 3.5 Exceptions only at construction

`EventLoop()` throws `std::system_error` if `epoll_create1` fails (`daemon/event_loop.cpp:14`). That way an object can never exist in a broken state. The runtime paths report errors with a `Status` enum instead (`daemon/qmi_client.hpp:26`).

## 4. Where in this repo

| idea | location |
|---|---|
| RAII destructor | `daemon/qmi_client.cpp:60`, `daemon/link_monitor.cpp:22` |
| move-only type | `daemon/qmi_client.hpp:41-45` |
| copy disabled on owners | `daemon/qmi_client.hpp:59`, `daemon/event_loop.hpp:25` |
| `shared_ptr` keep-alive | `daemon/event_loop.cpp:71` |
| key-capturing timer | `daemon/qmi_client.cpp:120` |
| erase before callback | `daemon/qmi_client.cpp:203` |
| `std::chrono::ceil` | `daemon/event_loop.cpp:56` |
| synchronous facade over async code (Android) | `android/hal/modem_hal.cpp:38` |

## 5. How it is verified

```bash
./run-tests.sh   # test_qmi_client_socketpair, test_frame_buffer, test_e2e_sim, e2e_daemon
```

The ASan pass catches lifetime mistakes such as use-after-free and heap-use-after-move.

## 6. Real vs simulated

The code is real. The peer at the other end is `modem_sim`, or a test that plays the modem over a socketpair.

## 7. Interview Q&A

1. **Why delete the copy constructor of `Reply`?** Its `msg` points into its own `frame`. A copy would point into the other object's buffer.
2. **Why capture a key and not an iterator in a timer?** Rehashing an `unordered_map` invalidates iterators, and the entry may already have been erased.
3. **Why erase before calling the callback?** Reentrancy. The callback can send a new request, change the map, or even reuse the same key.
4. **How can a handler safely remove itself?** The loop holds a `shared_ptr` copy while the handler runs, so the handler outlives its own removal.
5. **`std::function` cost?** It may allocate on the heap for large captures, and it is type-erased. That is fine at control-plane rates.
6. **Why `ceil` for timeouts?** Truncation turns 0.4 ms into 0 ms, so `epoll_wait(0)` would busy-spin until the deadline.
7. **Where is the rule of zero/five visible?** `Reply` defaults its moves and deletes its copies (rule of five, done explicitly). `Tlv` uses the rule of zero.

## 8. Try it yourself

1. Remove `= delete` on the `Reply` copy constructor. Write a test that copies a `Reply`, frees the original, and reads TLVs from the copy. Run it under ASan.
2. Change the timeout lambda to capture an iterator instead of the key. Construct a case that forces a rehash (many pending requests) and watch ASan.
3. Rewrite the bring-up chain in `modem_manager.cpp` as a small state machine (an enum plus a `switch`) instead of nested lambdas. Which version is easier to test?
