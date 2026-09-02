/* Broadcast a file to Mercury, or receive one from it.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Speaks to Mercury's broadcast TCP port with KISS framing, exactly as the UI
 * does and as hermes-broadcast does -- one transport, one set of framing rules.
 *
 *   bcast_file_tool send <file> [-m mode] [-c cycles] [-i ip] [-p port]
 *   bcast_file_tool recv <dir>  [-m mode] [-i ip] [-p port]
 *
 * cycles 0 (the default) repeats until interrupted.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "bcast_file.h"
#include "bcast_modes.h"

#define KISS_FEND  0xC0
#define KISS_FESC  0xDB
#define KISS_TFEND 0xDC
#define KISS_TFESC 0xDD
#define KISS_CMD_DATA 0x02

static volatile sig_atomic_t running = 1;
static void on_signal(int s) { (void)s; running = 0; }

static int dial(const char *ip, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { close(fd); return -1; }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

static int send_kiss(int fd, const uint8_t *p, size_t n)
{
    uint8_t out[2 * BCAST_FILE_MAX_FRAME + 8];
    size_t o = 0;
    out[o++] = KISS_FEND;
    out[o++] = KISS_CMD_DATA;
    for (size_t i = 0; i < n; i++)
    {
        if (p[i] == KISS_FEND)      { out[o++] = KISS_FESC; out[o++] = KISS_TFEND; }
        else if (p[i] == KISS_FESC) { out[o++] = KISS_FESC; out[o++] = KISS_TFESC; }
        else                         out[o++] = p[i];
    }
    out[o++] = KISS_FEND;
    size_t sent = 0;
    while (sent < o)
    {
        ssize_t w = write(fd, out + sent, o - sent);
        if (w <= 0) return -1;
        sent += (size_t)w;
    }
    return 0;
}

/* Incremental KISS decode: calls sink() for each complete frame. */
struct kiss_rx { uint8_t buf[BCAST_FILE_MAX_FRAME + 8]; size_t len; int in_frame, esc, first; };

static void kiss_feed(struct kiss_rx *k, const uint8_t *p, size_t n,
                      void (*sink)(const uint8_t *, size_t, void *), void *ctx)
{
    for (size_t i = 0; i < n; i++)
    {
        uint8_t c = p[i];
        if (c == KISS_FEND)
        {
            if (k->in_frame && k->len > 0) sink(k->buf, k->len, ctx);
            k->in_frame = 1; k->len = 0; k->esc = 0; k->first = 1;
            continue;
        }
        if (!k->in_frame) continue;
        if (k->first) { k->first = 0; continue; }   /* command byte */
        if (c == KISS_FESC) { k->esc = 1; continue; }
        if (k->esc) { c = (c == KISS_TFEND) ? KISS_FEND : KISS_FESC; k->esc = 0; }
        if (k->len < sizeof(k->buf)) k->buf[k->len++] = c;
    }
}

struct rx_ctx { bcast_file_rx_t *rx; int done; };

static void on_frame(const uint8_t *f, size_t n, void *vp)
{
    struct rx_ctx *c = vp;
    switch (bcast_file_rx_frame(c->rx, f, n))
    {
    case BCAST_RX_COMPLETE:
        printf("\nreceived \"%s\" -> %s\n",
               bcast_file_rx_last_name(c->rx), bcast_file_rx_last_path(c->rx));
        c->done = 1;
        break;
    case BCAST_RX_ERROR:
        fprintf(stderr, "\nreceive error: %s\n", bcast_file_rx_error(c->rx));
        break;
    case BCAST_RX_PROGRESS: {
        uint64_t sym; size_t want;
        bcast_file_rx_stats(c->rx, &sym, &want);
        fprintf(stderr, "\rsymbols %llu (need ~%zu bytes)   ",
                (unsigned long long)sym, want);
        break;
    }
    default: break;
    }
}

static void usage(const char *p)
{
    fprintf(stderr,
        "usage: %s send <file> [-m mode] [-c cycles] [-i ip] [-p port]\n"
        "       %s recv <dir>  [-m mode] [-i ip] [-p port]\n\n"
        "  -m  Mercury mode index (default 1); BOTH ends must use the same one\n"
        "  -c  carousel cycles, 0 = until interrupted (default 0)\n"
        "  -i  Mercury address (default 127.0.0.1)\n"
        "  -p  broadcast port (default 8100)\n", p, p);
}

int main(int argc, char **argv)
{
    if (argc < 3) { usage(argv[0]); return 2; }
    const char *cmd = argv[1], *arg = argv[2];
    int mode = 1, cycles = 0, port = 8100;
    const char *ip = "127.0.0.1";

    for (int i = 3; i < argc - 1; i++)
    {
        if      (!strcmp(argv[i], "-m")) mode   = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c")) cycles = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-p")) port   = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-i")) ip     = argv[++i];
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    char err[192] = {0};
    int fd = dial(ip, port);
    if (fd < 0) { fprintf(stderr, "cannot reach mercury at %s:%d\n", ip, port); return 1; }

    if (!strcmp(cmd, "send"))
    {
        bcast_file_tx_t *tx = bcast_file_tx_open(arg, mode, cycles, 0, err, sizeof(err));
        if (!tx) { fprintf(stderr, "%s\n", err); close(fd); return 1; }

        size_t bytes; int blocks;
        bcast_file_tx_source(tx, &bytes, &blocks);
        int fs = bcast_file_tx_frame_size(tx);
        printf("sending %s: bundle %zu B, mode %d (%d B/frame), %d block(s), cycles %d\n",
               arg, bytes, mode, fs, blocks, cycles);

        uint8_t *frame = malloc((size_t)fs);
        uint64_t n = 0;
        while (running)
        {
            int got = bcast_file_tx_next(tx, frame, (size_t)fs);
            if (got == 0) { printf("\ncycles complete\n"); break; }
            if (got < 0)  { fprintf(stderr, "\nencoder failed\n"); break; }
            if (send_kiss(fd, frame, (size_t)got) != 0)
            { fprintf(stderr, "\nsend failed: %s\n", strerror(errno)); break; }
            if ((++n % 10) == 0) { fprintf(stderr, "\rframes %llu", (unsigned long long)n); }
        }
        fprintf(stderr, "\nsent %llu frames\n", (unsigned long long)n);
        free(frame);
        bcast_file_tx_close(tx);
    }
    else if (!strcmp(cmd, "recv"))
    {
        struct rx_ctx c = {0};
        c.rx = bcast_file_rx_open(mode, arg, err, sizeof(err));
        if (!c.rx) { fprintf(stderr, "%s\n", err); close(fd); return 1; }
        printf("listening on %s:%d, mode %d, writing into %s\n", ip, port, mode, arg);

        struct kiss_rx k = {0};
        uint8_t buf[4096];
        while (running && !c.done)
        {
            ssize_t r = read(fd, buf, sizeof(buf));
            if (r <= 0) break;
            kiss_feed(&k, buf, (size_t)r, on_frame, &c);
        }
        bcast_file_rx_close(c.rx);
        close(fd);
        return c.done ? 0 : 1;
    }
    else { usage(argv[0]); close(fd); return 2; }

    close(fd);
    return 0;
}
