/* -x hl2: the Hermes-Lite 2 transport -- see hl2.h.
 *
 * One thread does everything.  It finds the radio, primes its registers,
 * starts the stream, and then answers every EP6 packet (126 RX samples,
 * 2.625 ms) with one EP2 packet (126 TX samples and two C&C registers):
 * TX is paced by the radio's own clock and cannot drift from it.  If EP6
 * stops while transmitting, TX free-runs on the local clock until it comes
 * back.  The radio keeps receiving while it transmits (duplex); those
 * samples are its own signal, so the receiver is muted for the
 * transmission and HL2_MUTE_TAIL_PKTS after it.
 *
 * MOX follows the modem as in -x rtp: on while the modem transmits or still
 * has audio queued.  The HL2 holds HL2_WIRE_TX_LATENCY_MS of TX samples
 * before RF starts; the modem's 100 ms PTT tail flushes them.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "hl2.h"
#include "hl2_dsp.h"
#include "hl2_wire.h"

#include "hermes_log.h"
#include "os_interop.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#endif

extern _Atomic bool mercury_stop;
extern int arq_get_trx(void);                        /* 1 = transmitting */
extern void radio_io_report_frequency(uint64_t hz);

#define LOG_TAG               "audio-hl2"
#define DISC_TRIES            4
#define DISC_WAIT_MS          700
#define DISC_RETRY_MS         2000    /* radio absent or busy: ask again    */
#define PRIME_PKTS            HL2_CC_SLOTS
#define PKT_US                2625    /* 126 samples at 48 kHz              */
#define RX_PAUSE_US           20000   /* no EP6 this long while TX: free-run */
#define FREE_RESYNC_US        100000
#define KEEPALIVE_US          100000  /* idle, no EP6: C&C still goes out   */
#define STALL_US              2000000 /* no EP6 at all: restart the stream  */
#define MUTE_TAIL_PKTS        30      /* ~80 ms: TX latency + PTT hang      */
#define MAX_GAP_PKTS          400     /* fill lost packets up to ~1 s       */
#define LOG_EVERY_MS          10000

static _Atomic uint64_t g_freq_hz;
static _Atomic int      g_rx_gain_db = HL2_DEFAULT_RX_GAIN_DB;
static _Atomic int      g_tx_drive   = HL2_DEFAULT_TX_DRIVE;
static _Atomic bool     g_pa         = true;

void hl2_set_frequency(uint64_t hz)
{
    atomic_store(&g_freq_hz, hz);
    if (hz)
        radio_io_report_frequency(hz);
}
uint64_t hl2_get_frequency(void)   { return atomic_load(&g_freq_hz); }
void hl2_set_rx_gain_db(int db)
{
    atomic_store(&g_rx_gain_db, db < HL2_WIRE_LNA_MIN_DB ? HL2_WIRE_LNA_MIN_DB :
                                db > HL2_WIRE_LNA_MAX_DB ? HL2_WIRE_LNA_MAX_DB : db);
}
int  hl2_get_rx_gain_db(void)      { return atomic_load(&g_rx_gain_db); }
void hl2_set_tx_drive(int level)
{
    atomic_store(&g_tx_drive, level < 0 ? 0 : level > 255 ? 255 : level);
}
int  hl2_get_tx_drive(void)        { return atomic_load(&g_tx_drive); }
void hl2_set_pa(bool on)           { atomic_store(&g_pa, on); }
bool hl2_get_pa(void)              { return atomic_load(&g_pa); }

static uint64_t now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / freq.QuadPart) * 1000000u +
           (uint64_t)(c.QuadPart % freq.QuadPart) * 1000000u / (uint64_t) freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000u + (uint64_t) ts.tv_nsec / 1000u;
#endif
}

static void addr_str(const struct sockaddr_in *a, char *buf, size_t n)
{
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
    snprintf(buf, n, "%s:%u", ip, (unsigned) ntohs(a->sin_port));
}

static int open_socket(void)
{
    int fd = (int) socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
        return -1;
    int one = 1, rcvbuf = 256 * 1024, tos = 0xB8;      /* DSCP EF */
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char *) &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char *) &rcvbuf, sizeof(rcvbuf));
    setsockopt(fd, IPPROTO_IP, IP_TOS, (const char *) &tos, sizeof(tos));
    struct sockaddr_in any = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(fd, (struct sockaddr *) &any, sizeof(any)) < 0)
    {
        SOCK_CLOSE(fd);
        return -1;
    }
    return fd;
}

/* "auto"/"" -> broadcast; "host[:port]" -> that address. */
static bool resolve_target(const char *dev, struct sockaddr_in *sa, bool *broadcast)
{
    char host[128];
    unsigned port = HL2_WIRE_PORT;
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    *broadcast = !dev || !dev[0] || !strcmp(dev, "auto") || !strcmp(dev, "default");
    if (*broadcast)
    {
        sa->sin_addr.s_addr = htonl(INADDR_BROADCAST);
        sa->sin_port = htons(HL2_WIRE_PORT);
        return true;
    }
    snprintf(host, sizeof(host), "%s", dev);
    char *colon = strrchr(host, ':');
    if (colon)
    {
        *colon = '\0';
        port = (unsigned) strtoul(colon + 1, NULL, 10);
        if (port == 0 || port > 65535)
            return false;
    }
    sa->sin_port = htons((uint16_t) port);
    if (inet_pton(AF_INET, host, &sa->sin_addr) == 1)
        return true;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
        return false;
    sa->sin_addr = ((struct sockaddr_in *) res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

/* Wait up to timeout_ms for a datagram; 1 if one arrived. */
static int wait_readable(int fd, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    return poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLIN);
}

static int recv_from(int fd, uint8_t *buf, size_t n, struct sockaddr_in *from)
{
    socklen_t fl = sizeof(*from);
    return (int) recvfrom(fd, (char *) buf, (int) n, 0, (struct sockaddr *) from, &fl);
}

static void send_to(int fd, const uint8_t *p, size_t n, const struct sockaddr_in *to)
{
    (void) sendto(fd, (const char *) p, (int) n, 0, (const struct sockaddr *) to, sizeof(*to));
}

static void board_from_reply(hl2_board *b, const hl2_wire_disc *d, const struct sockaddr_in *from)
{
    memset(b, 0, sizeof(*b));
    addr_str(from, b->addr, sizeof(b->addr));
    memcpy(b->mac, d->mac, sizeof(b->mac));
    b->gateware = d->gateware;
    b->gateware_minor = d->gateware_minor;
    b->board_id = d->board_id;
    b->busy = d->busy;
}

int hl2_discover(hl2_board *boards, int max, int timeout_ms)
{
    int fd = open_socket(), found = 0;
    if (fd < 0)
        return 0;
    uint8_t req[HL2_WIRE_DISC_BYTES], buf[HL2_WIRE_PKT_BYTES];
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(HL2_WIRE_PORT),
                              .sin_addr.s_addr = htonl(INADDR_BROADCAST) }, from;
    send_to(fd, req, hl2_wire_build_discovery(req), &to);
    uint64_t end = now_us() + (uint64_t) timeout_ms * 1000u;
    for (uint64_t t; found < max && (t = now_us()) < end;)
    {
        if (!wait_readable(fd, (int)((end - t) / 1000u) + 1))
            continue;
        hl2_wire_disc d;
        int n = recv_from(fd, buf, sizeof(buf), &from);
        if (n > 0 && hl2_wire_parse_discovery(buf, (size_t) n, &d) == 0)
            board_from_reply(&boards[found++], &d, &from);
    }
    SOCK_CLOSE(fd);
    return found;
}

/* Find the radio: up to DISC_TRIES requests, the first reply wins.
 * 1 = found (*radio, *disc set), 0 = no answer. */
static int find_radio(int fd, const struct sockaddr_in *target, struct sockaddr_in *radio,
                      hl2_wire_disc *disc, volatile bool *stop)
{
    uint8_t req[HL2_WIRE_DISC_BYTES], buf[HL2_WIRE_PKT_BYTES];
    size_t n = hl2_wire_build_discovery(req);
    for (int i = 0; i < DISC_TRIES && !mercury_stop && !*stop; i++)
    {
        send_to(fd, req, n, target);
        uint64_t end = now_us() + DISC_WAIT_MS * 1000u;
        for (uint64_t t; (t = now_us()) < end && !mercury_stop && !*stop;)
        {
            if (!wait_readable(fd, (int)((end - t) / 1000u) + 1))
                continue;
            int len = recv_from(fd, buf, sizeof(buf), radio);
            if (len > 0 && hl2_wire_parse_discovery(buf, (size_t) len, disc) == 0)
                return 1;
        }
    }
    return 0;
}

static void sleep_ms_interruptible(int ms, volatile bool *stop)
{
    for (int i = 0; i < ms / 50 && !mercury_stop && !*stop; i++)
    {
#ifdef _WIN32
        Sleep(50);
#else
        struct timespec ts = { 0, 50 * 1000000L };
        nanosleep(&ts, NULL);
#endif
    }
}

static double power_watts(uint16_t raw)
{
    double v = (raw + 34.0) / 4095.0 * 3.3;
    return v * v / 1.52;
}

typedef struct {
    int                fd;
    struct sockaddr_in radio;
    uint32_t           seq;
    int                slot;
    bool               keyed;
    bool               warned_nofreq;
    hl2_tx             dsp;
    uint64_t           last_ep2_us;
} tx_state;

/* One EP2 packet: 21 modem samples (126 at 48 kHz) while the modem
 * transmits or still has audio queued, silence otherwise.  Returns true on
 * the packet that ends a transmission. */
static bool tx_step(tx_state *tx)
{
    hl2_wire_cfg cfg = {
        .freq_hz = (uint32_t) atomic_load(&g_freq_hz),
        .drive   = (uint8_t) atomic_load(&g_tx_drive),
        .lna_db  = atomic_load(&g_rx_gain_db),
        .pa      = atomic_load(&g_pa),
    };
    size_t have = size_buffer(playback_buffer);
    bool active = arq_get_trx() == 1 || have >= sizeof(int32_t);
    bool ended = false;
    int16_t iq[2 * HL2_WIRE_PKT_SAMPLES];

    if (active && !cfg.freq_hz)
    {
        if (!tx->warned_nofreq)
            HLOGE(LOG_TAG, "no frequency set: not transmitting (set hl2 frequency_hz)");
        tx->warned_nofreq = true;
        active = false;
        clear_buffer(playback_buffer);
    }
    if (active)
    {
        int32_t in[HL2_WIRE_PKT_SAMPLES / HL2_DSP_L] = { 0 };
        have -= have % sizeof(int32_t);
        if (have > sizeof(in))
            have = sizeof(in);
        if (have)
            read_buffer(playback_buffer, (uint8_t *) in, have);
        if (!tx->keyed)
        {
            hl2_tx_init(&tx->dsp);
            HLOGI(LOG_TAG, "TX start, %.3f kHz, drive %u%s", cfg.freq_hz / 1000.0,
                  cfg.drive, cfg.pa ? "" : ", PA off");
        }
        hl2_tx_process(&tx->dsp, in, HL2_WIRE_PKT_SAMPLES / HL2_DSP_L, iq);
        tx->keyed = true;
    }
    else if (tx->keyed)
    {
        tx->keyed = false;
        ended = true;
    }

    uint8_t cc1[5], cc2[5], pkt[HL2_WIRE_PKT_BYTES];
    hl2_wire_cc_config(cc1, &cfg, tx->keyed);
    hl2_wire_cc_slot(cc2, tx->slot, &cfg, tx->keyed);
    tx->slot = (tx->slot + 1) % HL2_CC_SLOTS;
    hl2_wire_build_ep2(pkt, tx->seq++, cc1, cc2, tx->keyed ? iq : NULL);
    send_to(tx->fd, pkt, sizeof(pkt), &tx->radio);
    tx->last_ep2_us = now_us();
    return ended;
}

static void send_command(int fd, const struct sockaddr_in *radio, bool start)
{
    uint8_t cmd[HL2_WIRE_CMD_BYTES];
    send_to(fd, cmd, hl2_wire_build_command(cmd, start), radio);
}

void *hl2_transport_thread(void *arg)
{
    const hl2_ctx *ctx = arg;
    volatile bool *stop = ctx->stop;
    struct sockaddr_in target;
    bool broadcast;
    char where[96];

    if (!resolve_target(ctx->device, &target, &broadcast))
    {
        HLOGE(LOG_TAG, "bad address '%s' (want auto, or host[:port])", ctx->device);
        ctx->health(true, AUDIO_HEALTH_FAILED, "bad Hermes-Lite 2 address");
        ctx->health(false, AUDIO_HEALTH_FAILED, "bad Hermes-Lite 2 address");
        return NULL;
    }
    tx_state tx = { .fd = open_socket() };
    if (tx.fd < 0)
    {
        ctx->health(true, AUDIO_HEALTH_FAILED, "cannot open a UDP socket");
        ctx->health(false, AUDIO_HEALTH_FAILED, "cannot open a UDP socket");
        return NULL;
    }
    if (broadcast)
        snprintf(where, sizeof(where), "the local network (broadcast)");
    else
        addr_str(&target, where, sizeof(where));

    /* Find a radio that is free.  Absent or busy is reported and retried:
     * the operator may be switching it on, or closing another program. */
    hl2_wire_disc disc;
    bool reported = false;
    for (;;)
    {
        if (mercury_stop || *stop)
            goto out;
        if (find_radio(tx.fd, &target, &tx.radio, &disc, stop))
        {
            if (!disc.busy)
                break;
            char a[64];
            addr_str(&tx.radio, a, sizeof(a));
            HLOGW(LOG_TAG, "Hermes-Lite 2 at %s is busy: another program is streaming from it", a);
            if (!reported)
            {
                ctx->health(true, AUDIO_HEALTH_FAILED, "Hermes-Lite 2 busy (close the other SDR program)");
                ctx->health(false, AUDIO_HEALTH_FAILED, "Hermes-Lite 2 busy (close the other SDR program)");
            }
            reported = true;
        }
        else if (!reported)
        {
            HLOGW(LOG_TAG, "no Hermes-Lite 2 answered on %s; still looking", where);
            ctx->health(true, AUDIO_HEALTH_FAILED, "no Hermes-Lite 2 answered");
            ctx->health(false, AUDIO_HEALTH_FAILED, "no Hermes-Lite 2 answered");
            reported = true;
        }
        sleep_ms_interruptible(DISC_RETRY_MS, stop);
    }

    char radio_s[64];
    addr_str(&tx.radio, radio_s, sizeof(radio_s));
    HLOGI(LOG_TAG, "Hermes-Lite 2 at %s, MAC %02x:%02x:%02x:%02x:%02x:%02x, gateware %u.%u",
          radio_s, disc.mac[0], disc.mac[1], disc.mac[2], disc.mac[3], disc.mac[4], disc.mac[5],
          disc.gateware, disc.gateware_minor);
    if (disc.board_id != HL2_WIRE_BOARD_HL)
        HLOGW(LOG_TAG, "board id %u is not a Hermes-Lite; trying anyway", disc.board_id);
    else if (10 * disc.gateware + disc.gateware_minor < 400 && disc.gateware_minor)
        HLOGW(LOG_TAG, "gateware %u.%u is a Hermes-Lite 1; its registers may differ",
              disc.gateware, disc.gateware_minor);
    if (!atomic_load(&g_freq_hz))
        HLOGW(LOG_TAG, "no frequency set: receiving at the bottom of the range, not transmitting");
    else
        radio_io_report_frequency(atomic_load(&g_freq_hz));

    /* Every register once, then start. */
    for (int i = 0; i < PRIME_PKTS; i++)
        tx_step(&tx);
    send_command(tx.fd, &tx.radio, true);
    ctx->health(false, AUDIO_HEALTH_RUNNING, NULL);

    static hl2_rx rx;
    hl2_rx_init(&rx);
    hl2_wire_status st = { 0 };
    bool rx_running = false, have_seq = false, stall_logged = false;
    uint32_t next_seq = 0;
    uint64_t last_rx_us = 0, next_free_us = 0, last_start_us = now_us();
    uint64_t lost = 0, dropped = 0, underflows = 0, overloads = 0, last_log_ms = 0;
    int mute_tail = 0;
    uint16_t tx_fwd_peak = 0, tx_rev_peak = 0;
    float iq[2 * HL2_WIRE_PKT_SAMPLES], zero_iq[2 * HL2_WIRE_PKT_SAMPLES] = { 0 };
    int32_t audio[HL2_WIRE_PKT_SAMPLES / HL2_DSP_L + HL2_DSP_BLOCK];

    while (!mercury_stop && !*stop)
    {
        uint64_t now = now_us();
        bool want_tx = tx.keyed || arq_get_trx() == 1;

        /* EP6 paused (some gateware, or the network) while transmitting:
         * keep TX going on the local clock. */
        if (want_tx && (!last_rx_us || now - last_rx_us >= RX_PAUSE_US))
        {
            if (!next_free_us || now >= next_free_us + FREE_RESYNC_US)
                next_free_us = now;
            while (now >= next_free_us)
            {
                if (tx_step(&tx))
                    mute_tail = MUTE_TAIL_PKTS;
                next_free_us += PKT_US;
            }
        }
        else
            next_free_us = 0;

        if (!want_tx && (!last_rx_us || now - last_rx_us >= KEEPALIVE_US) &&
            now - tx.last_ep2_us >= KEEPALIVE_US)
            tx_step(&tx);                       /* registers keep flowing */

        if ((!last_rx_us && now - last_start_us >= STALL_US) ||
            (last_rx_us && now - last_rx_us >= STALL_US && now - last_start_us >= STALL_US))
        {
            if (!stall_logged)
                HLOGW(LOG_TAG, "no data from the radio for %d s: restarting the stream",
                      STALL_US / 1000000);
            stall_logged = true;
            send_command(tx.fd, &tx.radio, true);
            last_start_us = now;
        }

        if (!wait_readable(tx.fd, want_tx ? 2 : 20))
            continue;
        uint8_t pkt[2048];
        struct sockaddr_in from;
        int len = recv_from(tx.fd, pkt, sizeof(pkt), &from);
        uint32_t seq;
        if (len <= 0 || from.sin_addr.s_addr != tx.radio.sin_addr.s_addr ||
            from.sin_port != tx.radio.sin_port ||
            hl2_wire_parse_ep6(pkt, (size_t) len, &seq, iq, &st) != 0)
            continue;

        now = now_us();
        if (!rx_running)
        {
            HLOGI(LOG_TAG, "receiving");
            ctx->health(true, AUDIO_HEALTH_RUNNING, NULL);
            rx_running = true;
        }
        if (stall_logged)
            HLOGI(LOG_TAG, "stream back");
        stall_logged = false;
        last_rx_us = now;

        /* Lost packets become silence, so the demodulator sees the loss
         * instead of a splice.  Sequence 0 is the radio restarting. */
        uint32_t gap = have_seq && seq ? seq - next_seq : 0;
        if (gap > 0 && gap <= MAX_GAP_PKTS)
        {
            lost += gap;
            for (uint32_t g = 0; g < gap; g++)
            {
                size_t k = hl2_rx_process(&rx, zero_iq, HL2_WIRE_PKT_SAMPLES, true, audio);
                if (k && circular_buf_free_size(capture_buffer) >= k * sizeof(int32_t))
                    write_buffer(capture_buffer, (uint8_t *) audio, k * sizeof(int32_t));
            }
        }
        have_seq = true;
        next_seq = seq + 1;

        bool mute = tx.keyed || mute_tail > 0;
        if (mute_tail > 0 && !tx.keyed)
            mute_tail--;
        size_t k = hl2_rx_process(&rx, iq, HL2_WIRE_PKT_SAMPLES, mute, audio);
        if (k && circular_buf_free_size(capture_buffer) >= k * sizeof(int32_t))
            write_buffer(capture_buffer, (uint8_t *) audio, k * sizeof(int32_t));
        else if (k)
            dropped += k;

        if (tx.keyed)
        {
            tx_fwd_peak = st.fwd_raw > tx_fwd_peak ? st.fwd_raw : tx_fwd_peak;
            tx_rev_peak = st.rev_raw > tx_rev_peak ? st.rev_raw : tx_rev_peak;
            underflows += st.tx_underflow;
        }
        overloads += st.adc_overload && !mute;

        /* Lockstep: one TX packet per RX packet. */
        if (!next_free_us && tx_step(&tx))
            mute_tail = MUTE_TAIL_PKTS;
        if (!tx.keyed && (tx_fwd_peak || tx_rev_peak))
        {
            uint16_t f = tx_fwd_peak > tx_rev_peak ? tx_fwd_peak : tx_rev_peak;
            uint16_t r = tx_fwd_peak > tx_rev_peak ? tx_rev_peak : tx_fwd_peak;
            HLOGI(LOG_TAG, "TX end: forward %.1f W, reflected %.2f W", power_watts(f), power_watts(r));
            tx_fwd_peak = tx_rev_peak = 0;
        }

        uint64_t ms = now / 1000u;
        if (ms - last_log_ms >= LOG_EVERY_MS && (lost || dropped || underflows || overloads))
        {
            if (lost)       HLOGW(LOG_TAG, "%llu packets lost on the network so far", (unsigned long long) lost);
            if (dropped)    HLOGW(LOG_TAG, "capture ring full: %llu samples dropped so far", (unsigned long long) dropped);
            if (underflows) HLOGW(LOG_TAG, "radio TX buffer ran dry %llu times so far", (unsigned long long) underflows);
            if (overloads)  HLOGW(LOG_TAG, "ADC overload in %llu packets so far: lower the RX gain", (unsigned long long) overloads);
            last_log_ms = ms;
        }
    }

    /* Never leave the radio keyed, and stop its stream. */
    for (int i = 0; i < 3; i++)
    {
        uint8_t cc1[5], cc2[5], pkt[HL2_WIRE_PKT_BYTES];
        hl2_wire_cfg cfg = { .freq_hz = (uint32_t) atomic_load(&g_freq_hz),
                             .lna_db = atomic_load(&g_rx_gain_db), .pa = atomic_load(&g_pa),
                             .drive = (uint8_t) atomic_load(&g_tx_drive) };
        hl2_wire_cc_config(cc1, &cfg, false);
        hl2_wire_cc_slot(cc2, HL2_CC_DRIVE, &cfg, false);
        hl2_wire_build_ep2(pkt, tx.seq++, cc1, cc2, NULL);
        send_to(tx.fd, pkt, sizeof(pkt), &tx.radio);
    }
    send_command(tx.fd, &tx.radio, false);
    send_command(tx.fd, &tx.radio, false);
out:
    SOCK_CLOSE(tx.fd);
    ctx->health(true, AUDIO_HEALTH_STOPPED, NULL);
    ctx->health(false, AUDIO_HEALTH_STOPPED, NULL);
    HLOGI(LOG_TAG, "transport thread exit");
    return NULL;
}
