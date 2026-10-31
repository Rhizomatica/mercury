/* tests/sim/carousel_sim.c -- two carousel data planes (datalink_arq/carousel.c)
 * on the sim's channel; carousel_bench and test_carousel run it.
 *
 * Two carousel instances, each with what Mercury's runtime gives it: a
 * half-duplex medium, a control decoder that hears every DATAC16 frame and one
 * payload decoder that hears only the mode it is bound to, and carrier sense
 * (a decoder in sync from CS_ACQ_MS after the peer keys until CS_ACQ_MS after
 * its last frame ends).  Channel, airtime and guards are the sim's
 * (sim_channel.c and the ARQ mode table), as in ab_bench with SIM_CS=400.
 *
 * The session is connected when the run starts, as in ab_bench (the connect is
 * not measured): the callee's ACCEPT was the first poll.
 *
 *   CAR_TRACE=1  print every keydown
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "carousel_sim.h"
#include "modem_mfsk.h"            /* MERCURY_MODE_MFSK */
#include "sim_channel.h"
#include "carousel.h"
#include "arq_protocol.h"
#include "freedv_api.h"

#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEAD_MS         110       /* tx delay + head silence (as carousel.c) */
#define TAIL_MS         200
#ifndef MFSK_CS_EXTRA_MS
#define MFSK_CS_EXTRA_MS 1000
#endif
#ifndef CS_ACQ_MS
#define CS_ACQ_MS       400       /* carrier sense: sync this long after keying */
#endif

/* ---- events ---------------------------------------------------------------- */
enum { EV_ARRIVE, EV_TXEND };
typedef struct {
    uint64_t t;
    int      type, st;             /* st = station the event is for */
    int      mode;
    size_t   len;
    uint8_t *bytes;
    uint64_t f_start, f_end;       /* EV_ARRIVE: the frame's air interval */
    bool     injected;             /* an explorer's spurious pattern: never a collision */
} event_t;

#define MAXEV 4096
static event_t evq[MAXEV];
static int     nev;

static void push(event_t e)
{
    if (nev >= MAXEV) { fprintf(stderr, "event queue full\n"); exit(3); }
    evq[nev++] = e;
}
static int pop(event_t *e)
{
    if (!nev) return 0;
    int b = 0;
    for (int i = 1; i < nev; i++) if (evq[i].t < evq[b].t) b = i;
    *e = evq[b];
    evq[b] = evq[--nev];
    return 1;
}
static uint64_t next_event_time(void)
{
    uint64_t t = UINT64_MAX;
    for (int i = 0; i < nev; i++) if (evq[i].t < t) t = evq[i].t;
    return t;
}

/* ---- stations ------------------------------------------------------------- */
typedef struct {
    int      id;
    car_t    car;
    uint8_t  tx[CAR_SIM_BYTES];  size_t tx_len, tx_read;
    uint8_t  rx[CAR_SIM_BYTES + 64]; size_t rx_len;
    int      rx_mode;              /* the payload decoder's binding */
    uint64_t bound_at;             /* ...since */
    uint64_t tx_start, tx_end;     /* current/last keydown */
    /* The frames of that keydown, for CAR_CS_DECODABLE: a decoder syncs on a
     * frame only from its preamble, so only one it was bound to by then. */
    struct { uint64_t start; int mode; bool sync; } kd[CAR_KEYDOWN_MAX];
    int      kd_n;
    int      kd_no;               /* this keydown's index, for carousel_sim_force_unsensed */
    bool     turn_mine;           /* the last trace line was "turn: mine" (recv_fallbacks) */
    /* CAR_NAV: the keydown's header pattern, heard by the peer or not, and
     * when the peer's detector reports it. */
    bool     nav_ok, nav_sent;
    uint64_t nav_from;
    uint64_t air_ms;               /* on the air, all keydowns */
    uint64_t heard_at, silent_ms;  /* the peer last heard (frame or pattern); the longest gap */
} station_t;

static station_t S[2];
static sim_channel_t *ch;
static int collisions;
static const int *spurious_idx;
static int spurious_n;
void carousel_sim_force_spurious_break(const int *kd_idx, int n) { spurious_idx = kd_idx; spurious_n = n; }
static const int *corrupt_idx;
static int corrupt_n;
void carousel_sim_force_corrupt(const int *frame_idx, int n) { corrupt_idx = frame_idx; corrupt_n = n; }
/* carousel_sim_force_losses: an explorer's choice of lost frames. */
static const int *force_idx;
static int force_n = -1;
static int frame_no;
static const int *unsensed_idx;
static int unsensed_n;
static int keydown_no;
static int kd_overlaps, kd_unexplained;
static int non_mfsk[2];   /* frames each station keyed on another mode than MFSK */
void carousel_sim_force_unsensed(const int *kd_idx, int n) { unsensed_idx = kd_idx; unsensed_n = n; }
static bool unsensed(int kd)
{
    for (int i = 0; i < unsensed_n; i++)
        if (unsensed_idx[i] == kd) return true;
    return false;
}
void carousel_sim_force_losses(const int *frame_idx, int n) { force_idx = frame_idx; force_n = n; }
/* Is this frame delivered?  The channel model's answer, or the explorer's. */
static bool spurious_kd(int kd)
{
    for (int i = 0; i < spurious_n; i++)
        if (spurious_idx[i] == kd) return true;
    return false;
}
static bool deliver(uint64_t t, int dir, int mode, uint64_t *d)
{
    int k = frame_no++;
    if (spurious_kd(S[dir].kd_no)) return false;   /* faded: a foreign BREAK answers it */
    if (force_n < 0) return sim_channel_schedule(ch, t, dir, mode, 0, d);
    for (int i = 0; i < force_n; i++)
        if (force_idx[i] == k) return false;
    *d = t + sim_channel_airtime_ms(mode, 0);
    return true;
}
static int bad_frames;
static int slot_breaches;              /* S's keydowns outside the slot M's log holds */
static uint64_t now_ms;
static bool trace;
static double snr_now = 12.0;          /* the SNR stamped on delivered frames */
static bool cs_decodable;              /* CAR_CS_DECODABLE: carrier sense needs a decodable frame */
static int nav_mode;                   /* CAR_NAV: 0 off, 1 modelled header, 2 always heard */
static uint32_t nav_air;               /* CAR_NAV_AIR: the header's lead, header + gap */
/* CAR_SNR_BIAS: the estimate as the bench read it at 10 %: DATAC15, DATAC4 and
 * DATAC16 frames about 3.5 dB below DATAC3 and DATAC1 on the same link. */
static bool snr_biased;
static bool asym;                                      /* an asym:A:B channel */
static double snr_dir[2];                              /* its SNR, by sender */
/* Never exactly 0.0: car_on_frame takes that for "no estimate", which a
 * decoded frame always has -- at 0 dB (cliff:0, fade:0) the sim ran with no
 * SNR at all. */
static double frame_snr(const station_t *rx, uint64_t f_start, int mode)
{
    /* Under fading, what the modem reports for this frame: on air the
     * estimate swings with the fade, and everything that reads it -- the
     * start rung, the control-deaf switch -- sees that. */
    double snr = sim_channel_frame_snr(ch, f_start, rx->id ^ 1, mode);
    if (isnan(snr)) snr = asym ? snr_dir[rx->id ^ 1] : snr_now;
    return snr == 0.0 ? 0.001 : snr;
}
/* CAR_SNR_OFFSET=<dB>: every estimate reads that much off, as a receiver
 * whose estimate reads low (on air estacao2 read 5-7 dB on every mode where
 * DATAC17, rated 7 dB, delivered all its frames). */
static double snr_offset;
static double snr_bias(int mode)
{
    if (!snr_biased) return snr_offset;
    return snr_offset + (mode == FREEDV_MODE_DATAC15 || mode == FREEDV_MODE_DATAC4 || mode == ARQ_CONTROL_MODE ? -3.5 : 0.0);
}
static double step_from, step_to, step_at_s = -1.0;   /* a step:A:B:T channel */
static double step_back = NAN;                         /* stepa: the way back, this much better */

/* The detector reports a pattern this long after it ends (0.23-0.24 s on air,
 * Pi 4); a poll, by contrast, is decoded as it ends. */
/* CAR_TRACE_OVERLAP: one line per keydown that starts while the peer is on
 * the air -- why it was not sensed. */
static void overlap_diag(const station_t *s, const station_t *peer, const char *what)
{
    if (!getenv("CAR_TRACE_OVERLAP")) return;
    int sync = 0, ctl = 0;
    for (int i = 0; i < peer->kd_n; i++) {
        if (peer->kd[i].sync) sync++;
        if (peer->kd[i].mode == ARQ_CONTROL_MODE) ctl++;
    }
    printf("OVERLAP t=%.1f %c keys %s %.1f s into %c's keydown (%.1f s long, %d frames, %d ctl, %d syncable)"
           " peer_nav=%s nav_from=%+.1f listener rx_mode=%d bound=%+.1f snr=%.1f\n",
           now_ms / 1000.0, 'A' + s->id, what, (now_ms - peer->tx_start) / 1000.0, 'A' + peer->id,
           (peer->tx_end - peer->tx_start) / 1000.0, peer->kd_n, ctl, sync,
           peer->nav_ok ? "heard" : peer->nav_sent ? "missed" : "none",
           peer->nav_sent ? ((double)peer->nav_from - (double)now_ms) / 1000.0 : 0.0,
           s->rx_mode, ((double)s->bound_at - (double)peer->tx_start) / 1000.0, frame_snr(s, now_ms, ARQ_CONTROL_MODE));
}

#ifndef SIM_PATTERN_DETECT_MS
#define SIM_PATTERN_DETECT_MS 240
#endif
/* The turn rules from outside (docs/CAROUSEL-TURNS.md): the callee is on the
 * air only inside a slot the caller's log still holds -- start and end, as
 * the keydown really is.  The caller keys clear of every such slot, so this
 * is NoOverlap whatever the channel lets either end hear.  Station 0 is the
 * caller. */
static void check_slot(const station_t *s)
{
    if (s->id == 0) return;
    if (!car_slot_reserved(&S[0].car, s->tx_start, s->tx_end)) {
        slot_breaches++;
        if (trace) printf("%9.1f B OUT OF SLOT: %.1f-%.1f\n", now_ms / 1000.0,
                          s->tx_start / 1000.0, s->tx_end / 1000.0);
    }
}

static void io_keydown(void *ctx, const car_frame_t *fr, int n)
{
    station_t *s = ctx, *peer = &S[s->id ^ 1];
    /* CAR_NAV: the keydown opens with a header pattern, heard (or not) as a
     * pattern is, after which the peer counts as keyed until the keydown ends. */
    bool nav = nav_mode && car_wants_nav(&s->car);
    uint64_t t = now_ms + HEAD_MS + (nav ? nav_air : 0);
    s->nav_ok = false;
    s->nav_sent = nav;
    if (nav) {
        uint64_t th = now_ms + HEAD_MS, d;
        uint64_t hair = sim_channel_airtime_ms(SIM_MODE_PATTERN, 0);
        bool peer_keyed_then = peer->tx_end > th && peer->tx_start < th + hair;
        s->nav_ok = !peer_keyed_then &&
                    (nav_mode == 2 || sim_channel_schedule(ch, th, s->id, SIM_MODE_PATTERN, 0, &d));
        s->nav_from = th + hair + SIM_PATTERN_DETECT_MS;
    }
    if (peer->tx_end > now_ms) {                  /* keyed while the peer is on the air */
        kd_overlaps++;
        if (!unsensed(peer->kd_no)) kd_unexplained++;
        overlap_diag(s, peer, n && fr[0].mode == ARQ_CONTROL_MODE ? "ctl" : "data");
    }
    s->tx_start = now_ms;
    s->kd_n = 0;
    s->kd_no = keydown_no++;
    if (trace) printf("%9.1f %c keys:", now_ms / 1000.0, 'A' + s->id);
    for (int i = 0; i < n; i++) {
        /* The modem refuses a frame that does not fill its mode (modem.c,
         * send_modulated_keydown): a keydown with one never goes out. */
        const arq_mode_timing_t *tm = arq_protocol_mode_timing(fr[i].mode);
        if (tm && fr[i].len != (size_t)tm->payload_bytes) {
            bad_frames++;
            if (trace) printf("%9.1f %c BAD FRAME %d: %zu bytes for mode %d\n", now_ms / 1000.0,
                              'A' + s->id, i, (size_t)fr[i].len, fr[i].mode);
        }
        if (i) t += fr[i].gap_ms;
        if (!mercury_mode_is_mfsk(fr[i].mode)) non_mfsk[s->id]++;
        uint64_t air = sim_channel_airtime_ms(fr[i].mode, 0), d;
        if (trace) {
            static const char *K[] = { "poll", "handover", "status", "?" };
            if (fr[i].mode == ARQ_CONTROL_MODE) printf(" %s", K[fr[i].bytes[0] >> 6]);
            else printf(" data(%d)", fr[i].mode);
        }
        /* A decoder syncs on what it could decode above its cliff, even when
         * this frame is then lost (see io_peer_keyed). */
        s->kd[s->kd_n].start = t; s->kd[s->kd_n].mode = fr[i].mode;
        s->kd[s->kd_n].sync = sim_channel_syncable(ch, t, s->id, fr[i].mode);
        s->kd_n++;
        if (deliver(t, s->id, fr[i].mode, &d)) {
            uint8_t *b = malloc(fr[i].len);
            memcpy(b, fr[i].bytes, fr[i].len);
            /* An explorer's corrupt frame that passed its CRC: one byte flipped. */
            for (int j = 0; j < corrupt_n; j++)
                if (corrupt_idx[j] == frame_no - 1) {
                    /* CAR_CORRUPT_POS: which byte (default the middle) */
                    const char *ps = getenv("CAR_CORRUPT_POS");
                    size_t pos = ps ? (size_t)atoi(ps) % fr[i].len : fr[i].len / 2;
                    b[pos] ^= 0x5A;
                }
            event_t e = { .t = t + air, .type = EV_ARRIVE, .st = peer->id, .mode = fr[i].mode,
                          .len = fr[i].len, .bytes = b, .f_start = t, .f_end = t + air };
            push(e);
        }
        t += air;
    }
    s->tx_end = t + TAIL_MS;
    s->air_ms += s->tx_end - s->tx_start;
    if (trace) printf("  until %.1f\n", s->tx_end / 1000.0);
    check_slot(s);
    event_t e = { .t = s->tx_end, .type = EV_TXEND, .st = s->id };
    push(e);
}

static void io_bind_rx(void *ctx, int mode)
{
    station_t *s = ctx;
    if (mode != s->rx_mode) { s->rx_mode = mode; s->bound_at = now_ms; }
}
static int recv_fallbacks;
static void io_trace(void *ctx, const char *line)
{
    station_t *s = ctx;
    /* the fall-back to ok_lv taken as the turn passes (m_decide_recv, #448) is
     * traced right after "turn: mine"; m_decide_send's is not */
    if (s->turn_mine && !strncmp(line, "tx: no answer on lv=", 20) && strstr(line, "back to")) recv_fallbacks++;
    s->turn_mine = !strncmp(line, "turn: mine", 10);
    if (trace) printf("%9.1f %c   %s\n", now_ms / 1000.0, 'A' + ((station_t *)ctx)->id, line);
}

/* A pattern: 0.64 s on the air, detected ~10 dB below DATAC16. */
static void io_pattern(void *ctx, int kind)
{
    station_t *s = ctx, *peer = &S[s->id ^ 1];
    uint64_t t = now_ms + HEAD_MS, d;
    uint64_t air = sim_channel_airtime_ms(SIM_MODE_PATTERN, 0);
    if (peer->tx_end > now_ms) {                  /* keyed while the peer is on the air */
        kd_overlaps++;
        if (!unsensed(peer->kd_no)) kd_unexplained++;
        overlap_diag(s, peer, "pattern");
    }
    s->tx_start = now_ms;
    s->kd_n = 0;
    s->nav_ok = false; s->nav_sent = false;
    s->kd_no = keydown_no++;
    if (trace) printf("%9.1f %c keys: pattern %s\n", now_ms / 1000.0, 'A' + s->id, kind ? "BREAK" : "ACK");
    if (deliver(t, s->id, SIM_MODE_PATTERN, &d)) {
        s->kd[0].start = t; s->kd[0].mode = SIM_MODE_PATTERN; s->kd[0].sync = true;
        s->kd_n = 1;
        event_t e = { .t = t + air + SIM_PATTERN_DETECT_MS, .type = EV_ARRIVE, .st = peer->id, .mode = SIM_MODE_PATTERN,
                      .len = (size_t)kind, .bytes = NULL, .f_start = t, .f_end = t + air };
        push(e);
    }
    s->tx_end = t + air + TAIL_MS;
    check_slot(s);
    event_t e = { .t = s->tx_end, .type = EV_TXEND, .st = s->id };
    push(e);
}

static int io_peer_keyed(void *ctx)
{
    const station_t *l = ctx, *p = &S[l->id ^ 1];
    /* CAR_CS_DECODABLE: carrier sense is a decoder in sync, so a keydown the
     * listener cannot decode is not sensed (on air: a DATAC1 probe at 4.5 dB,
     * and patterns, which no OFDM decoder syncs on) -- nor the part of it
     * before the first frame it can (on air: a floor frame behind a control
     * frame the listener cannot decode).  The control decoder always runs;
     * the payload decoder syncs on a frame only if bound to its mode by the
     * frame's preamble, and a pattern only when that is MFSK: rebound to the
     * floor mid-frame, on air, it missed that frame (car23 at 3 %). */
    if (p->tx_end && unsensed(p->kd_no)) return false;   /* an explorer's sensing failure */
    if (nav_mode && p->nav_ok && p->tx_end && now_ms >= p->nav_from && now_ms < p->tx_end - TAIL_MS + CS_ACQ_MS)
        return CAR_KEYED_NAV;                            /* its NAV header was heard */
    uint64_t from = p->tx_start;
    if (cs_decodable) {
        from = UINT64_MAX;
        for (int i = 0; i < p->kd_n && from == UINT64_MAX; i++) {
            int m = p->kd[i].mode;
            bool bound = l->bound_at <= p->kd[i].start;
            if (!p->kd[i].sync) continue;
            if (m == SIM_MODE_PATTERN) {
                if (mercury_mode_is_mfsk(l->rx_mode) && bound) from = p->kd[i].start;
            } else if (m == ARQ_CONTROL_MODE || (m == l->rx_mode && bound)) {
                /* The MFSK preamble search takes longer to be sure: under a
                 * second at -7 dB (test_mfsk_modem), more near its floor. */
                from = p->kd[i].start + (mercury_mode_is_mfsk(m) ? MFSK_CS_EXTRA_MS : 0);
            }
        }
        if (from == UINT64_MAX) return false;
    }
    return p->tx_end && from + CS_ACQ_MS <= now_ms && now_ms < p->tx_end - TAIL_MS + CS_ACQ_MS;
}

static size_t io_tx_read(void *ctx, uint8_t *buf, size_t max)
{
    station_t *s = ctx;
    size_t n = s->tx_len - s->tx_read;
    if (n > max) n = max;
    memcpy(buf, s->tx + s->tx_read, n);
    s->tx_read += n;
    return n;
}
static size_t io_tx_pending(void *ctx) { station_t *s = ctx; return s->tx_len - s->tx_read; }

static void io_deliver(void *ctx, const uint8_t *buf, size_t len)
{
    station_t *s = ctx;
    if (trace) printf("%9.1f %c delivered %zu (total %zu)\n", now_ms / 1000.0, 'A' + s->id, len, s->rx_len + len);
    if (s->rx_len + len > sizeof(s->rx)) len = sizeof(s->rx) - s->rx_len;
    memcpy(s->rx + s->rx_len, buf, len);
    s->rx_len += len;
}

/* ---- run ------------------------------------------------------------------ */
#define CHAT_REPLY_MS (getenv("CAR_CHAT_REPLY_MS") ? (uint64_t)atoi(getenv("CAR_CHAT_REPLY_MS")) : 200)

/* ---- application traffic ---------------------------------------------------
 * A script of messages, each written by one end: a gap after the one before it
 * was delivered, when the other end wrote that one (it is read first), or
 * after it was written, when the same end did (the operator types on).
 *
 *   CAR_CHAT=<exchanges>:<bytes>   request/response, as UUCP's: A, B, A, ...
 *                                  CAR_CHAT_REPLY_MS apart (default 200)
 *   CAR_SCRIPT=A30,B30,A5632+200,B20   these messages, +<gap ms> each (default
 *                                  CAR_CHAT_REPLY_MS; the first, 0)
 *   CAR_KBD=<lines>:<bytes>:<think s>[:<same %>]   keyboard to keyboard: lines
 *                                  of bytes/2..3*bytes/2, the next by the same
 *                                  end <same %> of the time (default 30), each
 *                                  after an exponential think time
 *   CAR_PACE=<bytes>:<ms>          an application that writes at most this
 *                                  much this often (uucpd: 4096:1000)
 */
typedef struct {
    int      side;
    size_t   bytes, put, end;     /* end: the writer's bytes through this one */
    uint64_t gap_ms, due, written, delivered;
    bool     armed;
} smsg_t;
static smsg_t sc[CAR_SIM_MSGS];
static int sc_n, sc_k;            /* messages; the next to write */
static size_t pace_bytes;
static uint64_t pace_ms;

static uint64_t sc_rand(uint64_t *x)   /* splitmix64 */
{
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static bool sc_add(int side, size_t bytes, uint64_t gap)
{
    if (sc_n >= CAR_SIM_MSGS || !bytes) return false;
    size_t end = bytes;
    for (int k = sc_n - 1; k >= 0; k--) if (sc[k].side == side) { end += sc[k].end; break; }
    if (end > CAR_SIM_BYTES) return false;
    sc[sc_n++] = (smsg_t){ .side = side, .bytes = bytes, .end = end, .gap_ms = gap };
    return true;
}
/* The script for this run, from the environment; false: none (a transfer). */
static bool sc_parse(uint64_t seed)
{
    sc_n = sc_k = 0;
    pace_bytes = 0; pace_ms = 0;
    const char *e;
    uint64_t reply = CHAT_REPLY_MS;
    bool ok = true;
    int n, b;
    if ((e = getenv("CAR_PACE"))) {
        unsigned long pb = 0, pm = 0;
        if (sscanf(e, "%lu:%lu", &pb, &pm) == 2 && pb && pm) { pace_bytes = pb; pace_ms = pm; }
    }
    if ((e = getenv("CAR_SCRIPT"))) {
        for (const char *p = e; *p && ok; ) {
            int side = *p == 'A' ? 0 : *p == 'B' ? 1 : -1;
            char *q;
            unsigned long by = strtoul(p + 1, &q, 10), gap = sc_n ? reply : 0;
            if (*q == '+') gap = strtoul(q + 1, &q, 10);
            ok = side >= 0 && sc_add(side, by, gap) && (*q == ',' || !*q);
            p = *q ? q + 1 : q;
        }
    } else if ((e = getenv("CAR_KBD"))) {
        double think = 0; int same = 30;
        uint64_t x = seed * 0x5851F42D4C957F2DULL + 1;
        ok = sscanf(e, "%d:%d:%lf:%d", &n, &b, &think, &same) >= 3 && n > 0 && b > 1 && think >= 0;
        for (int k = 0, side = 0; ok && k < n; k++) {
            if (k && (int)(sc_rand(&x) % 100) >= same) side ^= 1;
            size_t by = (size_t)(b / 2 + (int)(sc_rand(&x) % (uint64_t)(b + 1)));
            double u = ((sc_rand(&x) >> 11) + 0.5) / 9007199254740992.0;
            double t = -log(u) * think;
            if (t > 5 * think) t = 5 * think;
            ok = sc_add(side, by, k ? (uint64_t)(t * 1000.0) : 0);
        }
    } else if ((e = getenv("CAR_CHAT"))) {
        ok = sscanf(e, "%d:%d", &n, &b) == 2 && n > 0 && b > 0;
        for (int k = 0; ok && k < 2 * n; k++) ok = sc_add(k & 1, (size_t)b, k ? reply : 0);
    } else
        return false;
    if (!ok) {
        fprintf(stderr, "traffic script ignored (%s): at most %d messages, %d bytes each way\n",
                getenv("CAR_SCRIPT") ? "CAR_SCRIPT" : getenv("CAR_KBD") ? "CAR_KBD" : "CAR_CHAT",
                CAR_SIM_MSGS, CAR_SIM_BYTES);
        sc_n = 0;
    }
    return sc_n > 0;
}
static uint64_t sc_next(void)
{
    return sc_k < sc_n && sc[sc_k].armed ? sc[sc_k].due : UINT64_MAX;
}
/* Mark deliveries, then write what is due. */
static void sc_step(uint64_t now)
{
    for (int k = 0; k < sc_k; k++) {
        smsg_t *m = &sc[k];
        if (m->put == m->bytes && !m->delivered && S[m->side ^ 1].rx_len >= m->end) {
            m->delivered = now;
            if (trace) printf("%9.1f %c's message %d delivered (%.1f s)\n", now / 1000.0, 'A' + m->side,
                              k + 1, (now - m->written) / 1000.0);
        }
    }
    if (sc_k >= sc_n) return;
    smsg_t *m = &sc[sc_k];
    if (!m->armed) {
        const smsg_t *p = sc_k ? &sc[sc_k - 1] : NULL;
        if (!p) { m->armed = true; m->due = m->gap_ms; }
        else if (p->side == m->side ? p->put == p->bytes : p->delivered != 0) {
            m->armed = true;
            m->due = (p->side == m->side ? p->written : p->delivered) + m->gap_ms;
            if (m->due < now) m->due = now;
        }
    }
    if (!m->armed || now < m->due) return;
    size_t c = m->bytes - m->put;
    if (pace_bytes && c > pace_bytes) c = pace_bytes;
    if (!m->put) m->written = now;
    m->put += c;
    S[m->side].tx_len += c;
    if (trace) printf("%9.1f %c app writes %zu (message %d, %zu bytes)\n", now / 1000.0, 'A' + m->side, c,
                      sc_k + 1, m->bytes);
    if (m->put < m->bytes) m->due = now + pace_ms;
    else sc_k++;
}
static bool sc_done(void)
{
    for (int k = 0; k < sc_n; k++) if (!sc[k].delivered) return false;
    return true;
}

/* The channel a run uses, by name (see carousel_sim_run); *snr_out gets the
 * SNR the ends are told at connect.  Also used for bounds. */
sim_channel_t *carousel_sim_channel(uint64_t seed, const char *chan, double *snr_out)
{
    sim_channel_cfg_t cfg = { .seed = seed, .per = 0.02, .guard_ms = 150 };
    sim_channel_t *ch = sim_channel_create(&cfg);
    static const sim_mode_per_t NVIS[] = {
        { FREEDV_MODE_DATAC15, 0.20 }, { FREEDV_MODE_DATAC16, 0.20 },
        { FREEDV_MODE_DATAC13, 0.30 }, { FREEDV_MODE_DATAC14, 0.30 },
        { FREEDV_MODE_DATAC4,  0.45 }, { FREEDV_MODE_DATAC3,  0.67 },
        { FREEDV_MODE_DATAC1,  0.89 }, { FREEDV_MODE_DATAC17, 0.93 },
        { FREEDV_MODE_QAM16C2, 0.95 },
    };
    double snr_db = 12.0;
    if (!strncmp(chan, "fade:", 5)) {
        double m = 0, d = 0.5;
        sscanf(chan + 5, "%lf:%lf", &m, &d);
        sim_channel_set_fading(ch, m, d);
        snr_db = m;
    }
    else if (!strncmp(chan, "awgn:", 5)) sim_channel_set_per(ch, atof(chan + 5));
    else if (!strncmp(chan, "cliff:", 6)) {   /* cliff:S, or cliff:S:P: P of the frames above it lost too */
        double per;
        sim_channel_set_snr(ch, atof(chan + 6)); snr_db = atof(chan + 6);
        if (sscanf(chan + 6, "%*f:%lf", &per) == 1) sim_channel_set_per(ch, per);
    }
    else if (!strncmp(chan, "step:", 5)) {   /* step:A:B:T  A dB, then B dB from T s on */
        sscanf(chan + 5, "%lf:%lf:%lf", &step_from, &step_to, &step_at_s);
        sim_channel_set_snr(ch, step_from); snr_db = step_from;
    }
    else if (!strncmp(chan, "stepa:", 6)) {  /* stepa:A:B:T:D  step:A:B:T from A to B, D dB better back */
        sscanf(chan + 6, "%lf:%lf:%lf:%lf", &step_from, &step_to, &step_at_s, &step_back);
        snr_dir[0] = step_from; snr_dir[1] = step_from + step_back;
        sim_channel_set_snr_asym(ch, snr_dir[0], snr_dir[1]);
        asym = true;
        snr_db = step_from;
    }
    else if (!strncmp(chan, "asym:", 5)) {   /* asym:A:B  A dB from A to B, B dB back */
        sscanf(chan + 5, "%lf:%lf", &snr_dir[0], &snr_dir[1]);
        sim_channel_set_snr_asym(ch, snr_dir[0], snr_dir[1]);
        asym = true;
        snr_db = snr_dir[0] < snr_dir[1] ? snr_dir[0] : snr_dir[1];
    }
    else if (!strcmp(chan, "nvis")) {
        sim_channel_set_mode_per(ch, NVIS, (int)(sizeof(NVIS) / sizeof(NVIS[0])));
        snr_db = 10.0;
    }
    else if (!strncmp(chan, "nvisa:", 6)) {  /* nvisa:B  nvis, and only B dB back (#397) */
        sim_channel_set_mode_per(ch, NVIS, (int)(sizeof(NVIS) / sizeof(NVIS[0])));
        snr_dir[0] = 10.0; snr_dir[1] = atof(chan + 6);
        sim_channel_set_snr_asym(ch, snr_dir[0], snr_dir[1]);
        sim_channel_set_return_snr(ch, snr_dir[1]);
        asym = true;
        snr_db = snr_dir[1] < snr_dir[0] ? snr_dir[1] : snr_dir[0];
    }
    if (snr_out) *snr_out = snr_db;
    return ch;
}

/* s heard its peer at now_ms: the silence the FSM's lost-peer watchdog counts. */
static void heard(station_t *s)
{
    if (now_ms - s->heard_at > s->silent_ms) s->silent_ms = now_ms - s->heard_at;
    s->heard_at = now_ms;
}

void carousel_sim_run(uint64_t seed, const char *chan, bool bidir, uint64_t limit_ms, car_sim_result_t *res)
{
    trace = getenv("CAR_TRACE") != NULL;
    asym = false; step_back = NAN;
    cs_decodable = getenv("CAR_CS_DECODABLE") != NULL;
    nav_mode = getenv("CAR_NAV") ? (strcmp(getenv("CAR_NAV"), "perfect") ? 1 : 2) : 0;
    nav_air = !nav_mode ? 0 : getenv("CAR_NAV_AIR") ? (uint32_t)atoi(getenv("CAR_NAV_AIR")) : 700;
    car_set_nav_ms(nav_air);
    car_set_short_rung(!(getenv("CAR_SHORT") && !strcmp(getenv("CAR_SHORT"), "0")));   /* A/B */
    car_set_nav_below_db(getenv("CAR_NAV_BELOW") ? (float)atof(getenv("CAR_NAV_BELOW")) : 99.0f);
    car_set_nav_loss(getenv("CAR_NAV_LOSS") ? atof(getenv("CAR_NAV_LOSS")) : 0.3);
    snr_biased = getenv("CAR_SNR_BIAS") != NULL;
    snr_offset = getenv("CAR_SNR_OFFSET") ? atof(getenv("CAR_SNR_OFFSET")) : 0.0;
    nev = 0;
    collisions = 0;
    recv_fallbacks = 0;
    bad_frames = 0;
    slot_breaches = 0;
    frame_no = 0;
    non_mfsk[0] = non_mfsk[1] = 0;
    keydown_no = 0;
    kd_overlaps = 0;
    kd_unexplained = 0;
    memset(S, 0, sizeof(S));

    double snr_db = 12.0;                  /* what the sim stamps on frames */
    ch = carousel_sim_channel(seed, chan, &snr_db);
    snr_now = snr_db;
    if (getenv("CAR_NOHINT")) snr_db = -99.0;

    for (int i = 0; i < 2; i++) {
        station_t *s = &S[i];
        s->id = i;
        car_io_t io = { .keydown = io_keydown, .bind_rx = io_bind_rx, .peer_keyed = io_peer_keyed,
                        .tx_read = io_tx_read, .tx_pending = io_tx_pending, .deliver = io_deliver,
                        .pattern = getenv("CAR_NOPATTERN") ? NULL : io_pattern,
                        .trace = io_trace, .ctx = s };
        /* What each end hears, and what its peer hears of it. */
        double hears = asym ? snr_dir[i ^ 1] : snr_db, heard = asym ? snr_dir[i] : snr_db;
        hears += snr_offset; heard += snr_offset;   /* the connect estimate reads off too */
        if (getenv("CAR_NOHINT")) hears = heard = -99.0;
        /* CAR_MFSK_TX=A, B or AB: that station transmits only MFSK, and both
         * ends know it from the connect (arq_fsm.c); a, b: its peer does not */
        const char *mt = getenv("CAR_MFSK_TX");
        bool mine = mt && (strchr(mt, 'A' + i) || strchr(mt, 'a' + i)), peers = mt && strchr(mt, 'B' - i);
        bool known = mt && strchr(mt, 'A' + i);   /* ...and the peer knows it */
        car_init(&s->car, &io, peers ? 0 : car_start_level((float)hears), known ? 0 : car_start_level((float)heard));
        car_seed_ctl_deaf(&s->car, peers ? CAR_DEAF_MFSK_TX : hears < ARQ_SNR_MIN_DATAC15_DB,
                          mine ? CAR_DEAF_MFSK_TX : heard < ARQ_SNR_MIN_DATAC15_DB);
        if (!getenv("CAR_NOHINT")) car_seed_snr(&s->car, (float)hears, (float)heard);   /* as arq_fsm.c's car_start */
        /* CAR_BW=500: a BW500 session, capped at DATAC3 on the 500 Hz floor */
        if (getenv("CAR_BW")) {
            int bw = atoi(getenv("CAR_BW"));
            car_set_max_level(&s->car, car_max_level_for_bw(bw));
            car_set_floor_mode(&s->car, arq_floor_mode(bw));
        }
        if (getenv("CAR_FLOOR")) car_set_floor_mode(&s->car, atoi(getenv("CAR_FLOOR")));   /* A/B */
        s->rx_mode = -1;
    }
    for (int i = 0; i < CAR_SIM_BYTES; i++) {
        S[0].tx[i] = (uint8_t)((i * 13 + 7) & 0xFF);
        S[1].tx[i] = (uint8_t)((i * 29 + 3) & 0xFF);
    }
    S[0].tx_len = CAR_SIM_BYTES;
    S[1].tx_len = bidir ? CAR_SIM_BYTES : 0;
    /* A script of messages (CAR_CHAT, CAR_SCRIPT, CAR_KBD), or one transfer.
     * The first message, at 0, is there when the session starts. */
    bool script = sc_parse(seed);
    if (script) {
        S[0].tx_len = S[1].tx_len = 0;
        sc_step(0);
    }

    now_ms = 0;
    car_start_receiver(&S[1].car, 0);
    car_start_sender(&S[0].car, 0);

    uint64_t done_ms = 0;
    for (;;) {
        uint64_t t = next_event_time();
        for (int i = 0; i < 2; i++) {
            uint64_t d = car_next_deadline(&S[i].car);
            if (d < t) t = d;
        }
        if (script && sc_next() < t) t = sc_next();
        if (t == UINT64_MAX || t > limit_ms) break;
        now_ms = t;
        if (step_at_s >= 0 && now_ms >= (uint64_t)(step_at_s * 1000.0)) {
            if (!isnan(step_back)) {
                snr_dir[0] = step_to; snr_dir[1] = step_to + step_back;
                sim_channel_set_snr_asym(ch, snr_dir[0], snr_dir[1]);
            } else {
                sim_channel_set_snr(ch, step_to); snr_now = step_to;
            }
            step_at_s = -1.0;
        }
        event_t e;
        while (nev && next_event_time() <= now_ms && pop(&e)) {
            station_t *s = &S[e.st];
            if (e.type == EV_TXEND) {
                car_on_tx_done(&s->car, now_ms);
                /* An explorer's spurious BREAK, heard 1.2 s after this keydown:
                 * a detector false alarm, or another station's pattern. */
                if (spurious_kd(s->kd_no)) {
                    event_t b = { .t = now_ms + 1200, .type = EV_ARRIVE, .st = s->id,
                                  .mode = SIM_MODE_PATTERN, .len = CAR_PATTERN_BREAK,
                                  .f_start = now_ms + 560, .f_end = now_ms + 1200, .injected = true };
                    push(b);
                }
                continue;
            }
            /* half duplex: a station hears nothing while it transmits */
            if (s->tx_end > e.f_start && s->tx_start < e.f_end && s->tx_end) {
                if (e.injected) { free(e.bytes); continue; }
                collisions++;
                if (trace) printf("%9.1f COLLISION at %c\n", now_ms / 1000.0, 'A' + s->id);
            } else if (e.mode == SIM_MODE_PATTERN) {
                if (!getenv("CAR_NOPATTERN_RX")) {
                    heard(s);
                    car_on_pattern(&s->car, now_ms, (int)e.len);
                }
            } else if (e.mode == ARQ_CONTROL_MODE) {
                heard(s);
                car_on_frame(&s->car, now_ms, e.bytes, e.len, e.mode, true, (float)(frame_snr(s, e.f_start, e.mode) + snr_bias(e.mode)), 0);
            } else if (e.mode == s->rx_mode && (!cs_decodable || s->bound_at <= e.f_start)) {
                heard(s);
                car_on_frame(&s->car, now_ms, e.bytes, e.len, e.mode, false, (float)(frame_snr(s, e.f_start, e.mode) + snr_bias(e.mode)), 0);
            }
            free(e.bytes);
        }
        if (script) {
            size_t len[2] = { S[0].tx_len, S[1].tx_len };
            sc_step(now_ms);
            for (int i = 0; i < 2; i++) if (S[i].tx_len != len[i]) car_on_app_data(&S[i].car, now_ms);
        }
        for (int i = 0; i < 2; i++) car_on_time(&S[i].car, now_ms);
        if (car_failed(&S[0].car) || car_failed(&S[1].car)) break;   /* the session ends */
        /* Done when everything has arrived and been checked: data reaches the
         * application ahead of its block's check, and a corrupt piece in a
         * block whose check piece had not come yet passed as a completed
         * transfer. */
        if (S[1].rx_len >= S[0].tx_len && S[0].rx_len >= S[1].tx_len &&
            car_rx_verified(&S[0].car) && car_rx_verified(&S[1].car) &&
            (!script || sc_done())) { done_ms = now_ms; break; }
    }
    while (nev) { event_t e; pop(&e); free(e.bytes); }

    res->a2b = S[1].rx_len;
    res->b2a = S[0].rx_len;
    res->intact = !memcmp(S[1].rx, S[0].tx, S[1].rx_len < S[0].tx_len ? S[1].rx_len : S[0].tx_len) &&
                  !memcmp(S[0].rx, S[1].tx, S[0].rx_len < S[1].tx_len ? S[0].rx_len : S[1].tx_len);
    res->done_ms = done_ms;
    res->air_ms[0] = S[0].air_ms; res->air_ms[1] = S[1].air_ms;
    now_ms = done_ms ? done_ms : limit_ms;              /* ...and the silence still running */
    for (int i = 0; i < 2; i++) { heard(&S[i]); res->silent_ms[i] = S[i].silent_ms; }
    res->msgs = script ? sc_n : 0;
    for (int k = 0; k < res->msgs; k++)
        res->msg_delay_ms[k] = sc[k].delivered ? sc[k].delivered - sc[k].written : UINT64_MAX;
    res->collisions = collisions;
    res->frames = frame_no;
    res->non_mfsk[0] = non_mfsk[0]; res->non_mfsk[1] = non_mfsk[1];
    res->keydowns = keydown_no;
    res->kd_overlaps = kd_overlaps;
    res->recv_fallbacks = recv_fallbacks;
    res->kd_unexplained = kd_unexplained;
    res->failed = car_failed(&S[0].car) ? car_failed(&S[0].car) : car_failed(&S[1].car);
    res->bad_frames = bad_frames;
    res->overruns = car_overruns(&S[0].car) + car_overruns(&S[1].car) + slot_breaches;
    /* No timer either side, and nobody on the air: a station keyed when the
     * run ends arms its timers only as the keydown ends (a 27 s BW500 floor
     * frame often straddles the limit). */
    res->stalled = !done_ms && car_next_deadline(&S[0].car) == UINT64_MAX &&
                   car_next_deadline(&S[1].car) == UINT64_MAX &&
                   S[0].tx_end <= now_ms && S[1].tx_end <= now_ms;
    sim_channel_destroy(ch);
}
