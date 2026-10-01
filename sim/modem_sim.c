/*
 * modem_sim -- a SIMULATED QMI modem listening on a Unix domain socket.
 *
 * In real life the host talks QMUX to the modem through a character device
 * (/dev/cdc-wdm0 for USB, /dev/wwan0qmi0 for MHI/PCIe). Here a SOCK_STREAM Unix
 * socket stands in for that pipe. A byte stream has no message boundaries, which
 * is exactly why both sides need framing buffers.
 *
 * Event loop: one epoll set holding
 *   - the listening socket      (EPOLLIN = new client)
 *   - each client socket        (EPOLLIN = bytes, EPOLLOUT = can flush backlog)
 *   - a timerfd                 (periodic NAS signal-strength indications)
 *   - a signalfd                (SIGINT/SIGTERM -> clean shutdown)
 *
 * usage: modem_sim --socket PATH [--ind-ms N]   (N=0 disables indications)
 */
#define _GNU_SOURCE
#include "sim_core.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_CLIENTS 16
#define RBUF 4096
#define WBUF 16384

typedef struct {
    int fd;
    uint8_t rbuf[RBUF]; size_t rlen;   /* partial frames accumulate here */
    uint8_t wbuf[WBUF]; size_t wlen;   /* bytes the kernel didn't accept yet */
    sim_state st;
} client;

static client clients[MAX_CLIENTS];
static int ep;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[modem_sim] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void drop(client *c)
{
    logf_("client fd=%d disconnected", c->fd);
    epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    c->fd = -1;
}

/* Try to push wbuf out; arm EPOLLOUT only while a backlog exists, otherwise
 * a level-triggered EPOLLOUT would wake us constantly (busy loop). */
static void flush(client *c)
{
    while (c->wlen) {
        ssize_t n = send(c->fd, c->wbuf, c->wlen, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            drop(c);
            return;
        }
        memmove(c->wbuf, c->wbuf + n, c->wlen - (size_t)n);
        c->wlen -= (size_t)n;
    }
    struct epoll_event ev = {.events = EPOLLIN | (c->wlen ? EPOLLOUT : 0u), .data.ptr = c};
    epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &ev);
}

static void queue(client *c, const uint8_t *p, size_t n)
{
    if (c->wlen + n > WBUF) {   /* slow consumer: real modems drop too */
        logf_("client fd=%d write backlog full, dropping %zu bytes", c->fd, n);
        return;
    }
    memcpy(c->wbuf + c->wlen, p, n);
    c->wlen += n;
}

static void on_readable(client *c)
{
    for (;;) {
        ssize_t n = recv(c->fd, c->rbuf + c->rlen, RBUF - c->rlen, 0);
        if (n == 0) { drop(c); return; }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            drop(c);
            return;
        }
        c->rlen += (size_t)n;

        /* Extract as many COMPLETE frames as the buffer holds; keep the tail. */
        size_t off = 0;
        for (;;) {
            ssize_t fl = qmi_frame_length(c->rbuf + off, c->rlen - off);
            if (fl == 0 || (fl > 0 && (size_t)fl > c->rlen - off))
                break;                       /* need more bytes */
            if (fl < 0) {                    /* garbage: resync by skipping a byte */
                off++;
                continue;
            }
            qmi_msg req;
            int rc = qmi_parse(c->rbuf + off, (size_t)fl, &req);
            if (rc == QMI_OK) {
                uint8_t out[QMI_MAX_FRAME];
                ssize_t rl = sim_handle(&c->st, &req, out, sizeof out);
                logf_("rx svc=%u cid=%u txn=%u msg=0x%04x -> %zd byte reply",
                      req.service, req.client_id, req.txn_id, req.msg_id, rl);
                if (rl > 0)
                    queue(c, out, (size_t)rl);
            } else {
                logf_("bad frame: %s", qmi_strerror(rc));
            }
            off += (size_t)fl;
        }
        memmove(c->rbuf, c->rbuf + off, c->rlen - off);
        c->rlen -= off;
    }
    flush(c);
}

static void send_indications(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client *c = &clients[i];
        if (c->fd < 0)
            continue;
        /* Unicast an indication to every NAS client id this connection owns. */
        for (int cid = 1; cid < 256; cid++) {
            if (!sim_cid_allocated(&c->st, QMI_SVC_NAS, (uint8_t)cid))
                continue;
            uint8_t out[64];
            ssize_t n = sim_build_signal_indication(&c->st, (uint8_t)cid, out, sizeof out);
            if (n > 0)
                queue(c, out, (size_t)n);
        }
        flush(c);
    }
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    long ind_ms = 200;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "--ind-ms") && i + 1 < argc) ind_ms = atol(argv[++i]);
        else { fprintf(stderr, "usage: %s --socket PATH [--ind-ms N]\n", argv[0]); return 2; }
    }
    if (!path) { fprintf(stderr, "--socket required\n"); return 2; }
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;

    int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof sa.sun_path) { fprintf(stderr, "path too long\n"); return 2; }
    strcpy(sa.sun_path, path);
    unlink(path);
    if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(ls, 8) < 0) {
        perror("listen");
        return 1;
    }

    /* Block SIGINT/SIGTERM and receive them as readable fd events instead. */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (ind_ms > 0) {
        struct itimerspec its = {
            .it_interval = {ind_ms / 1000, (ind_ms % 1000) * 1000000L},
            .it_value = {ind_ms / 1000, (ind_ms % 1000) * 1000000L}};
        timerfd_settime(tfd, 0, &its, NULL);
    }

    ep = epoll_create1(EPOLL_CLOEXEC);
    /* data.ptr == NULL/sentinels distinguishes the special fds from clients */
    static int tag_listen, tag_timer, tag_signal;
    struct epoll_event ev = {.events = EPOLLIN, .data.ptr = &tag_listen};
    epoll_ctl(ep, EPOLL_CTL_ADD, ls, &ev);
    ev.data.ptr = &tag_timer;
    epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev);
    ev.data.ptr = &tag_signal;
    epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &ev);

    logf_("listening on %s (indications every %ld ms)", path, ind_ms);
    printf("READY\n");
    fflush(stdout);

    for (int running = 1; running;) {
        struct epoll_event evs[16];
        int n = epoll_wait(ep, evs, 16, -1);
        if (n < 0 && errno == EINTR)
            continue;
        for (int i = 0; i < n; i++) {
            void *p = evs[i].data.ptr;
            if (p == &tag_listen) {
                int cfd;
                while ((cfd = accept4(ls, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0) {
                    client *slot = NULL;
                    for (int k = 0; k < MAX_CLIENTS && !slot; k++)
                        if (clients[k].fd < 0) slot = &clients[k];
                    if (!slot) { close(cfd); continue; }
                    slot->fd = cfd;
                    slot->rlen = slot->wlen = 0;
                    sim_init(&slot->st);
                    struct epoll_event cev = {.events = EPOLLIN, .data.ptr = slot};
                    epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &cev);
                    logf_("client fd=%d connected", cfd);
                }
            } else if (p == &tag_timer) {
                uint64_t expirations;
                if (read(tfd, &expirations, sizeof expirations) == sizeof expirations)
                    send_indications();
            } else if (p == &tag_signal) {
                running = 0;
            } else {
                client *c = p;
                if (c->fd < 0) continue;
                if (evs[i].events & (EPOLLHUP | EPOLLERR)) { drop(c); continue; }
                if (evs[i].events & EPOLLIN) on_readable(c);
                if (c->fd >= 0 && (evs[i].events & EPOLLOUT)) flush(c);
            }
        }
    }
    logf_("shutting down");
    unlink(path);
    return 0;
}
