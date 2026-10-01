# LEARN: epoll and a single-threaded event loop

## 1. What

`epoll` is a Linux API. One thread asks the kernel "tell me which of these fds is ready", then sleeps until at least one is. Combined with **nonblocking** fds, this gives you an *event loop* (also called a reactor): the thread only does work when there is work to do, and no single fd can block the others.

## 2. Why it exists

A modem daemon has to watch, all at the same time:

- the modem channel
- the netlink socket
- timeouts
- signals
- maybe some client sockets

With one thread per job you need locks and you get data races. With one blocking `read()`, everything else freezes. rild, netd, ModemManager (through GLib's main loop) and Android's `Looper` are all event loops.

## 3. How it works

### The loop

```
for (;;) {
   wait = min(max_wait, time until the nearest timer)      // timers without threads
   n = epoll_wait(ep, evs, 16, wait)
   for each ready fd: call its handler(events)
   run every timer whose deadline has passed
}
```

`daemon/event_loop.cpp:51-89`.

### Level-triggered EPOLLOUT

EPOLLOUT means "you can write". On a healthy socket that is almost **always** true, so if you leave it on, `epoll_wait` returns immediately forever and you spin at 100% CPU. The rule:

```
want = !tx_.empty();
if (want != want_out_) mod_fd(fd, EPOLLIN | (want ? EPOLLOUT : 0))
```

`daemon/qmi_client.cpp:159-163`. The C version is in `sim/modem_sim.c:67-86`.

### Stale events and fd reuse

```
one epoll_wait batch returns: [fd 7 IN] [fd 9 IN]
handler for 7 closes fd 7 and opens a new socket -> the kernel hands out fd 7 again
event for "fd 7" now refers to the NEW socket -> wrong handler!
```

Fix: pack a **generation number** with the fd into `epoll_event.data.u64`:

```
 data.u64 = [ gen (32 bits) | fd (32 bits) ]
```

On dispatch, skip the event if the stored generation does not match (`daemon/event_loop.cpp:23`, `:66-70`).

### Timers

Timers live in a map from id to (deadline, fn). Due ids are collected first, then each one is looked up again before it runs, so a timer that an earlier callback cancelled never fires (`daemon/event_loop.cpp:75-88`). The wait is rounded **up** with `std::chrono::ceil` (`:56`).

### Signals and periodic timers as fds (the simulator)

```
sigprocmask(BLOCK, SIGINT|SIGTERM); sfd = signalfd(...)   -> readable on a signal
tfd = timerfd_create(...); timerfd_settime(interval)       -> readable on each tick
epoll set = { listen fd, client fds, tfd, sfd }
```

`sim/modem_sim.c:183-206`.

## 4. Where in this repo

| item | location |
|---|---|
| EventLoop API | `daemon/event_loop.hpp` |
| add_fd with generation | `daemon/event_loop.cpp:19-27` |
| run_once | `daemon/event_loop.cpp:51` |
| keep-alive during dispatch | `daemon/event_loop.cpp:71-72` |
| run_until (used by the sync facade) | `daemon/event_loop.cpp:91` |
| EPOLLOUT only while backlogged | `daemon/qmi_client.cpp:159-163`, `sim/modem_sim.c:69` |
| signalfd / timerfd | `sim/modem_sim.c:188`, `:190` |
| kernel side of poll (`.poll` fop) | `kernel/fake_mhi_modem/fake_mhi_modem.c:267` |

## 5. How it is verified

```bash
./run-tests.sh
```

- `EventLoopTimers.CancelFromEarlierDueCallbackWins` (`tests/test_qmi_client_socketpair.cpp:188`) covers timer cancellation.
- `TimeoutFiresAndLateReplyIsIgnored` (`:114`) covers timeouts driven by the epoll wait.
- `e2e_daemon` runs the real loop for 800 ms while indications arrive.

To watch the loop sleep:

```bash
strace -e trace=epoll_wait ./build/modem_manager --socket /tmp/m.sock --run-ms 500
```

## 6. Real vs simulated

Fully real. In the kernel stage, `test_fake_qmi` also uses `poll()` on `/dev/fake_qmi0`, so the kernel half of readiness (the `.poll` file operation plus a wait queue) is real kernel code too.

## 7. Interview Q&A

1. **Level- vs edge-triggered?** LT repeats the event while the condition holds. ET fires only on a change, so you must drain until EAGAIN. This lab uses LT, but still drains reads until EAGAIN.
2. **Why disable EPOLLOUT when idle?** Under LT it would fire constantly and the loop would busy-spin.
3. **select/poll vs epoll?** select and poll pass the whole fd set on every call, which is O(n). epoll keeps the set in the kernel and returns only the ready fds.
4. **What is the fd reuse hazard?** A closed fd number can be reused within the same batch. Detect it with a generation number or a pointer to the registration.
5. **How do you implement timeouts without threads?** Compute the `epoll_wait` timeout from the nearest deadline, or use a timerfd.
6. **Why signalfd?** Async signal handlers can only call async-signal-safe functions. With signalfd, a signal becomes a normal event in the loop.
7. **How does a blocking API sit on an async loop?** Pump the loop until the reply arrives (`run_until`), as `ModemHal` does. Guard it with a mutex if several threads call in.

## 8. Try it yourself

1. Change the EventLoop to `EPOLLET` and fix whatever breaks.
2. Temporarily leave EPOLLOUT always on, and measure CPU with `top` while `modem_manager --run-ms 3000` runs.
3. Replace the timer map with a single timerfd that is re-armed to the nearest deadline.
