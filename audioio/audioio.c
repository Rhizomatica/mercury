/* Audio subsystem
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@rhizomatica.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */


#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <limits.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <ffbase/stringz.h>
#include "os_interop.h"
#include <ffaudio/audio.h>
#include "std.h"
#ifndef FF_WIN
#include <time.h>
#endif

#include "ring_buffer_posix.h"
#include "shm_posix.h"
#include "defines_modem.h"

#include "audioio.h"
#include "hermes_log.h"
#include "cfg_utils.h"
#include "resampler.h"
#include "pcm24.h"

/* Must match the definition in common/mercury_engine.c: _Atomic, not
 * volatile.  This global is written from the termination signal handler and
 * polled by every worker loop; volatile orders nothing between threads, and
 * declaring the same object differently in different translation units is
 * undefined behaviour on top of that.  Plain assignment and test still work. */
extern _Atomic bool shutdown_;

/* ------------------------------------------------------------------ */
/*  DirectSound GUID ↔ string helpers (Windows only)                  */
/* ------------------------------------------------------------------ */
#if defined(_WIN32)

/* Format a GUID as "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}".
 * buf must be at least 39 bytes (38 chars + NUL).                    */
static void guid_to_str(const GUID *g, char *buf, size_t bufsize)
{
    snprintf(buf, bufsize,
             "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             (unsigned long)g->Data1, g->Data2, g->Data3,
             g->Data4[0], g->Data4[1],
             g->Data4[2], g->Data4[3], g->Data4[4],
             g->Data4[5], g->Data4[6], g->Data4[7]);
}

/* Parse a GUID string back into a GUID struct.  Returns 0 on success. */
static int str_to_guid(const char *s, GUID *g)
{
    unsigned long d1;
    unsigned int d2, d3;
    unsigned int d4[8];
    if (sscanf(s, "{%8lX-%4X-%4X-%2X%2X-%2X%2X%2X%2X%2X%2X}",
               &d1, &d2, &d3,
               &d4[0], &d4[1], &d4[2], &d4[3],
               &d4[4], &d4[5], &d4[6], &d4[7]) != 11)
        return -1;
    g->Data1 = d1;
    g->Data2 = (unsigned short)d2;
    g->Data3 = (unsigned short)d3;
    for (int i = 0; i < 8; i++)
        g->Data4[i] = (unsigned char)d4[i];
    return 0;
}

#endif /* _WIN32 */

cbuf_handle_t capture_buffer;
cbuf_handle_t playback_buffer;

int audio_subsystem;

/* ffaudio's OSS backend is only compiled where the OSSv4 API exists (see the
 * HAVE_OSS4 probe in audioio/Makefile).  Stock Linux ships only the OSS3 stub,
 * where oss.c does not compile, so &ffoss must not even be referenced there --
 * it is declared unconditionally in ffaudio/audio.h and would fail at link.
 * Selecting -x oss on such a build leaves `audio` NULL and is reported as an
 * unavailable subsystem. */
#if defined(HAVE_OSS4) || defined(__FreeBSD__)
#define OSS_IFACE ((ffaudio_interface *) &ffoss)
#else
#define OSS_IFACE NULL
#endif
static int capture_input_channel_layout = LEFT;

// Internal state for restart support
static pthread_t s_radio_capture;
static pthread_t s_radio_playback;
static char s_capture_dev[256];
static char s_playback_dev[256];
static int s_buffers_initialized = 0;
static volatile bool audio_shutdown_ = false;  // local stop flag for audio threads

/* Audio path health, reported to the UI so a bad device choice is visible.
 * Deliberately does NOT stop the process: the operator needs mercury alive to
 * pick a different card.  See audioio.h. */
static pthread_mutex_t s_health_lock = PTHREAD_MUTEX_INITIALIZER;
static audio_health_t  s_cap_health  = AUDIO_HEALTH_STOPPED;
static audio_health_t  s_play_health = AUDIO_HEALTH_STOPPED;
static char            s_health_reason[192];

static void audio_health_set(bool capture, audio_health_t st, const char *reason)
{
    pthread_mutex_lock(&s_health_lock);
    if (capture) s_cap_health = st; else s_play_health = st;
    if (st == AUDIO_HEALTH_FAILED && reason)
        snprintf(s_health_reason, sizeof(s_health_reason), "%s: %s",
                 capture ? "capture" : "playback", reason);
    else if (st == AUDIO_HEALTH_RUNNING)
        s_health_reason[0] = '\0';
    pthread_mutex_unlock(&s_health_lock);
}

audio_health_t audioio_capture_health(void)
{
    pthread_mutex_lock(&s_health_lock);
    audio_health_t v = s_cap_health;
    pthread_mutex_unlock(&s_health_lock);
    return v;
}

audio_health_t audioio_playback_health(void)
{
    pthread_mutex_lock(&s_health_lock);
    audio_health_t v = s_play_health;
    pthread_mutex_unlock(&s_health_lock);
    return v;
}

void audioio_health_reason(char *buf, size_t buflen)
{
    if (!buf || buflen == 0) return;
    pthread_mutex_lock(&s_health_lock);
    snprintf(buf, buflen, "%s", s_health_reason);
    pthread_mutex_unlock(&s_health_lock);
}

bool audioio_health_ok(char *reason, size_t reasonlen)
{
    pthread_mutex_lock(&s_health_lock);
    bool ok = (s_cap_health != AUDIO_HEALTH_FAILED) &&
              (s_play_health != AUDIO_HEALTH_FAILED);
    if (reason && reasonlen) snprintf(reason, reasonlen, "%s", s_health_reason);
    pthread_mutex_unlock(&s_health_lock);
    return ok;
}

/* Operator-facing fix for a rate the modem cannot use.  ALSA's plug layer
 * resamples when the device is named plughw: instead of hw:, but the Windows
 * and macOS backends have no such indirection: the device itself must be set
 * to a supported rate in the OS sound settings.  Telling a Windows user to
 * try plughw: is worse than saying nothing.
 *
 * Reaching this at all now means the rate is outside the supported set
 * entirely -- the 44.1 kHz family is resampled rather than refused -- so the
 * advice names a rate that is certain to work. */
static const char *audio_rate_mismatch_hint(void)
{
    switch (audio_subsystem)
    {
    case AUDIO_SUBSYSTEM_ALSA:
        return "try plughw:X,Y instead of hw:X,Y so ALSA converts";
    case AUDIO_SUBSYSTEM_WASAPI:
    case AUDIO_SUBSYSTEM_DSOUND:
    case AUDIO_SUBSYSTEM_COREAUDIO:
        return "set the device sample rate to 48000 Hz in the OS sound settings";
    default:
        return "set the device sample rate to a supported value";
    }
}

static void format_device_display(int audio_subsys, int mode, const char *id, char *out, size_t outsz);
static int get_soundcard_list_int(int audio_system, int mode,
                                  char ids[][AUDIO_DEV_STR_MAX], char dev_names[][AUDIO_DEV_STR_MAX], int max_count,
                                  bool pulse_lock_held);

#define NULL_AUDIO_PERIOD_MS 20
#define NULL_AUDIO_SAMPLES_PER_PERIOD 160
#define FIFO_AUDIO_POLL_MS 10
#define FIFO_AUDIO_CHUNK_BYTES 4096

struct conf {
    const char *cmd;
    ffaudio_conf buf;
    uint8_t flags;
    uint8_t exclusive;
    uint8_t hwdev;
    uint8_t loopback;
    uint8_t nonblock;
    uint8_t wav;
};


static inline void ffthread_sleep(ffuint msec)
{
#ifdef FF_WIN
    Sleep(msec);
#else
    struct timespec ts = {
        .tv_sec = msec / 1000,
        .tv_nsec = (msec % 1000) * 1000000,
    };
    nanosleep(&ts, NULL);
#endif
}

static inline uint64_t audioio_monotonic_ms(void)
{
#ifdef FF_WIN
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
#endif
}

#if defined(__linux__)
static pthread_mutex_t s_pulse_lock = PTHREAD_MUTEX_INITIALIZER;

static bool pulse_init_already_initialized(const ffaudio_init_conf *aconf)
{
    return aconf && aconf->error && strcmp(aconf->error, "already initialized") == 0;
}

static int pulse_shared_init(bool *did_init)
{
    ffaudio_interface *audio = (ffaudio_interface *) &ffpulse;
    ffaudio_init_conf aconf = {};
    aconf.app_name = "mercury";
    if (did_init)
        *did_init = false;

    if (audio->init(&aconf) != 0)
    {
        if (pulse_init_already_initialized(&aconf))
            return 0;
        HLOGE("audio-pulse", "Error initializing PulseAudio: %s",
              aconf.error ? aconf.error : "unknown");
        return -1;
    }

    if (did_init)
        *did_init = true;
    return 0;
}

static void pulse_shared_uninit(void)
{
    ffaudio_interface *audio = (ffaudio_interface *) &ffpulse;
    audio->uninit();
}
#endif

int audioio_pick_default_subsystem(void)
{
#if defined(__linux__)
    return AUDIO_SUBSYSTEM_ALSA;
#elif defined(_WIN32)
    return AUDIO_SUBSYSTEM_WASAPI;
#elif defined(__FreeBSD__)
    return AUDIO_SUBSYSTEM_OSS;
#elif defined(__APPLE__)
    return AUDIO_SUBSYSTEM_COREAUDIO;
#elif defined(__ANDROID__)
    return AUDIO_SUBSYSTEM_AAUDIO;
#else
    return AUDIO_SUBSYSTEM_ALSA;
#endif
}

static void *null_capture_thread(void *unused)
{
    (void) unused;
    int32_t silence[NULL_AUDIO_SAMPLES_PER_PERIOD] = {0};
    const size_t bytes = sizeof(silence);

    HLOGI("audio-null", "capture silence thread started");
    while (!shutdown_ && !audio_shutdown_)
    {
        if (circular_buf_free_size(capture_buffer) >= bytes)
            write_buffer(capture_buffer, (uint8_t *) silence, bytes);
        ffthread_sleep(NULL_AUDIO_PERIOD_MS);
    }
    HLOGI("audio-null", "capture silence thread exit");
    return NULL;
}

static void *null_playback_thread(void *unused)
{
    (void) unused;
    uint8_t discard[4096];

    HLOGI("audio-null", "playback discard thread started");
    while (!shutdown_ && !audio_shutdown_)
    {
        size_t bytes = size_buffer(playback_buffer);
        if (bytes > sizeof(discard))
            bytes = sizeof(discard);
        if (bytes > 0)
            read_buffer(playback_buffer, discard, bytes);
        else
            ffthread_sleep(NULL_AUDIO_PERIOD_MS);
    }
    HLOGI("audio-null", "playback discard thread exit");
    return NULL;
}

static int fifo_open_retry(const char *path, int flags, const char *log_tag)
{
    bool logged_wait = false;

    if (!path || path[0] == '\0')
    {
        HLOGE(log_tag, "missing FIFO path");
        return -1;
    }

    while (!shutdown_ && !audio_shutdown_)
    {
        int fd = open(path, flags);
        if (fd >= 0)
        {
            HLOGI(log_tag, "opened %s", path);
            return fd;
        }

        int err = errno;
        if (err == ENOENT || err == ENXIO)
        {
            if (!logged_wait)
            {
                HLOGW(log_tag, "waiting for FIFO %s: %s", path, strerror(err));
                logged_wait = true;
            }
        }
        else if (err != EINTR)
        {
            HLOGW(log_tag, "open(%s) failed: %s", path, strerror(err));
        }
        ffthread_sleep(FIFO_AUDIO_POLL_MS);
    }
    return -1;
}

static void *fifo_capture_thread(void *device_ptr)
{
    const char *path = (const char *) device_ptr;
    uint8_t buf[FIFO_AUDIO_CHUNK_BYTES];
    size_t buf_len = 0;
    bool logged_eof = false;

    int fd = fifo_open_retry(path, O_RDONLY | O_NONBLOCK, "audio-fifo-cap");
    if (fd < 0)
    {
        return NULL;   // audio failure ends this thread, not the engine
    }

    while (!shutdown_ && !audio_shutdown_)
    {
        /* Try to consume aligned int32_t samples from buf first */
        if (buf_len >= sizeof(int32_t))
        {
            size_t aligned = buf_len - (buf_len % sizeof(int32_t));
            if (circular_buf_free_size(capture_buffer) >= aligned)
            {
                write_buffer(capture_buffer, buf, aligned);
                buf_len -= aligned;
                if (buf_len > 0)
                    memmove(buf, buf + aligned, buf_len);
                continue;
            }
            ffthread_sleep(FIFO_AUDIO_POLL_MS);
            continue;
        }

        /* Read more data from FIFO */
        ssize_t n = read(fd, buf + buf_len, sizeof(buf) - buf_len);
        if (n > 0)
        {
            logged_eof = false;
            buf_len += (size_t)n;
            continue;
        }

        if (n == 0)
        {
            if (!logged_eof)
            {
                HLOGI("audio-fifo-cap", "read(%s) reached EOF; reopening", path);
                logged_eof = true;
            }
            buf_len = 0;
            close(fd);
            fd = fifo_open_retry(path, O_RDONLY | O_NONBLOCK, "audio-fifo-cap");
            if (fd < 0)
                break;
            ffthread_sleep(FIFO_AUDIO_POLL_MS);
            continue;
        }

        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            HLOGW("audio-fifo-cap", "read(%s) failed: %s", path, strerror(errno));
            buf_len = 0;
            close(fd);
            fd = fifo_open_retry(path, O_RDONLY | O_NONBLOCK, "audio-fifo-cap");
            if (fd < 0)
                break;
        }
        ffthread_sleep(FIFO_AUDIO_POLL_MS);
    }

    close(fd);
    HLOGI("audio-fifo-cap", "capture FIFO thread exit");
    return NULL;
}

static void *fifo_playback_thread(void *device_ptr)
{
    const char *path = (const char *) device_ptr;
    uint8_t buf[FIFO_AUDIO_CHUNK_BYTES];
    size_t pending_off = 0;
    size_t pending_len = 0;

    int fd = fifo_open_retry(path, O_WRONLY | O_NONBLOCK, "audio-fifo-play");
    if (fd < 0)
    {
        return NULL;   // audio failure ends this thread, not the engine
    }

    while (!shutdown_ && !audio_shutdown_)
    {
        if (pending_off >= pending_len)
        {
            pending_off = 0;
            pending_len = 0;
            size_t available = size_buffer(playback_buffer);
            if (available > sizeof(buf))
                available = sizeof(buf);
            available -= available % sizeof(int32_t);
            if (available > 0)
            {
                read_buffer(playback_buffer, buf, available);
                pending_len = available;
            }
            else
            {
                ffthread_sleep(FIFO_AUDIO_POLL_MS);
                continue;
            }
        }

        ssize_t n = write(fd, buf + pending_off, pending_len - pending_off);
        if (n > 0)
        {
            pending_off += (size_t)n;
            continue;
        }

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        {
            ffthread_sleep(FIFO_AUDIO_POLL_MS);
            continue;
        }

        HLOGW("audio-fifo-play", "write(%s) failed: %s", path, strerror(errno));
        close(fd);
        fd = fifo_open_retry(path, O_WRONLY | O_NONBLOCK, "audio-fifo-play");
        if (fd < 0)
            break;
    }

    close(fd);
    HLOGI("audio-fifo-play", "playback FIFO thread exit");
    return NULL;
}

static void fifo_ignore_sigpipe(void)
{
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
}

/* ------------------------------------------------------------------ */
/*  Framed-socket lockstep backend (-x sock)                          */
/* ------------------------------------------------------------------ */
/*
 * Virtual-clock bench transport: an AF_UNIX SOCK_STREAM connection to a
 * channel simulator that owns time.  The frame codec (layout, endianness,
 * sample conversion) lives in sock_wire.h, pinned byte-exact by
 * tests/audioio/test_sock_wire.c against the skywave sock_frames.py
 * contract.
 *
 * The sim SENDS the RX block first, then waits for the station's TX
 * block (lockstep barrier), so this thread does recv -> send per block
 * and cannot deadlock against it.  virtual_now_ms rides on every sim
 * frame; after depositing the RX samples the thread advances the
 * process virtual clock (common/virtual_clock.h) and wakes the ARQ
 * event loop, so every ARQ timer runs in signal time -- there is no
 * real-time pacing anywhere on this path.
 */
#ifndef _WIN32

#include <sys/socket.h>
#include <sys/un.h>

#include "virtual_clock.h"
#include "sock_wire.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0   /* macOS: SIGPIPE is ignored via fifo_ignore_sigpipe() */
#endif

#define SOCK_AUDIO_IO_TIMEOUT_MS 200           /* recv/send slice, so the loop
                                                  keeps seeing shutdown flags   */

/* Demod backpressure: hold the station's reply frame (which is what releases
 * the sim's next block) until the RX capture backlog is at most this many
 * samples, so signal time never runs more than ~1 s ahead of the demod.
 * Must stay below modem.c's RX_MAX_BACKLOG_SAMPLES stale-audio flush cap
 * (which is additionally disabled under a virtual clock).  The wait is
 * bounded so a wedged demod degrades to a warning, not a stalled sim. */
#define SOCK_AUDIO_RX_HIGH_WATER_SAMPLES 8000
#define SOCK_AUDIO_BACKPRESSURE_CAP_MS   30000

/* From datalink_arq/arq.c.  Declared here instead of including arq.h so the
 * audioio library keeps its narrow include surface (see audioio/Makefile).
 * arq_get_trx() returns 0 = RX, 1 = TX (arq.h). */
extern void arq_notify_virtual_time(void);
extern int  arq_get_trx(void);

static int sock_connect_retry(const char *path)
{
    bool logged_wait = false;

    if (!path || path[0] == '\0')
    {
        HLOGE("audio-sock", "missing socket path (MERCURY_AUDIO_SOCK)");
        return -1;
    }

    while (!shutdown_ && !audio_shutdown_)
    {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (strlen(path) >= sizeof(addr.sun_path))
        {
            HLOGE("audio-sock", "socket path too long (%zu >= %zu): %s",
                  strlen(path), sizeof(addr.sun_path), path);
            return -1;
        }
        memcpy(addr.sun_path, path, strlen(path) + 1);

        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
        {
            HLOGE("audio-sock", "socket() failed: %s", strerror(errno));
            return -1;
        }

        if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) == 0)
        {
            struct timeval tv = {
                .tv_sec  = SOCK_AUDIO_IO_TIMEOUT_MS / 1000,
                .tv_usec = (SOCK_AUDIO_IO_TIMEOUT_MS % 1000) * 1000,
            };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            HLOGI("audio-sock", "connected to %s", path);
            return fd;
        }

        close(fd);
        if (!logged_wait)
        {
            HLOGW("audio-sock", "waiting for sim socket %s: %s",
                  path, strerror(errno));
            logged_wait = true;
        }
        ffthread_sleep(FIFO_AUDIO_POLL_MS * 10);
    }
    return -1;
}

/* Read exactly len bytes.  1 = success, 0 = clean EOF at a frame boundary
 * (only reported when nothing of this read was consumed), -1 = error or
 * shutdown.  SO_RCVTIMEO keeps each recv() bounded so shutdown is seen. */
static int sock_recv_full(int fd, uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        if (shutdown_ || audio_shutdown_)
            return -1;
        ssize_t n = recv(fd, buf + off, len - off, 0);
        if (n > 0)
        {
            off += (size_t)n;
            continue;
        }
        if (n == 0)
            return (off == 0) ? 0 : -1;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            continue;
        return -1;
    }
    return 1;
}

static int sock_send_full(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        if (shutdown_ || audio_shutdown_)
            return -1;
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n > 0)
        {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            continue;
        return -1;
    }
    return 1;
}

/* The single lockstep worker: RX deposit, clock advance, TX drain -- one
 * frame exchange per sim block.  Runs as the `radio_capture` thread; the
 * paired `radio_playback` slot is sock_idle_thread (teardown joins both). */
static void *sock_transport_thread(void *device_ptr)
{
    const char *path = (const char *) device_ptr;

    uint8_t *payload = (uint8_t *) malloc(SOCK_WIRE_MAX_SAMPLES * sizeof(int16_t));
    int32_t *ring32  = (int32_t *) malloc(SOCK_WIRE_MAX_SAMPLES * sizeof(int32_t));
    uint8_t *txframe = (uint8_t *) malloc(4 + SOCK_WIRE_HDR_STA_BYTES +
                                          SOCK_WIRE_MAX_SAMPLES * sizeof(int16_t));
    if (!payload || !ring32 || !txframe)
    {
        HLOGE("audio-sock", "out of memory");
        free(payload); free(ring32); free(txframe);
        return NULL;
    }

    /* The virtual clock never runs backward, even across a sim reconnect
     * (a fresh sim restarts virtual_now_ms at its first block). */
    uint64_t clock_floor_ms = time_now_ms();

    int fd = sock_connect_retry(path);

    while (fd >= 0 && !shutdown_ && !audio_shutdown_)
    {
        /* ---- recv one sim frame ---- */
        uint8_t hdr[SOCK_WIRE_HDR_SIM_BYTES];
        uint8_t lenbuf[4];
        int rc = sock_recv_full(fd, lenbuf, sizeof(lenbuf));
        uint32_t len = (rc == 1) ? sock_wire_rd_u32(lenbuf) : 0;

        uint16_t n = 0;
        uint64_t seq = 0, vnow_ms = 0;
        if (rc == 1)
        {
            if (len < SOCK_WIRE_HDR_SIM_BYTES ||
                len > SOCK_WIRE_HDR_SIM_BYTES + SOCK_WIRE_MAX_SAMPLES * sizeof(int16_t))
            {
                HLOGE("audio-sock", "bad sim frame length %u", len);
                rc = -1;
            }
            else if ((rc = sock_recv_full(fd, hdr, sizeof(hdr))) == 1)
            {
                if (sock_wire_parse_sim(hdr, len, &seq, &vnow_ms, &n) != 0)
                {
                    HLOGE("audio-sock", "sim frame length %u disagrees with header", len);
                    rc = -1;
                }
                else if (n > 0)
                {
                    rc = sock_recv_full(fd, payload, (size_t)n * sizeof(int16_t));
                    if (rc == 0)
                        rc = -1;   /* EOF inside a frame */
                }
            }
            else if (rc == 0)
            {
                rc = -1;           /* EOF between len and header */
            }
        }

        if (rc != 1)
        {
            close(fd);
            fd = -1;
            if (rc == 0)
                HLOGI("audio-sock", "sim closed the socket; reconnecting");
            else if (!shutdown_ && !audio_shutdown_)
                HLOGW("audio-sock", "socket error; reconnecting");
            if (!shutdown_ && !audio_shutdown_)
                fd = sock_connect_retry(path);
            continue;
        }

        /* ---- deposit RX samples (i16 -> i32) ---- */
        for (uint16_t i = 0; i < n; i++)
            ring32[i] = sock_wire_i16_to_ring(sock_wire_rd_u16(payload + (size_t)i * 2));

        size_t rx_bytes = (size_t)n * sizeof(int32_t);
        while (rx_bytes > 0 && !shutdown_ && !audio_shutdown_)
        {
            if (circular_buf_free_size(capture_buffer) >= rx_bytes)
            {
                write_buffer(capture_buffer, (uint8_t *) ring32, rx_bytes);
                break;
            }
            ffthread_sleep(1);
        }

        /* ---- advance signal time, wake the ARQ event loop ---- */
        uint64_t t = VIRTUAL_CLOCK_EPOCH_MS + vnow_ms;
        if (t < clock_floor_ms)
            t = clock_floor_ms;
        clock_floor_ms = t;
        virtual_clock_set(t);
        arq_notify_virtual_time();

        /* ---- backpressure: let the demod catch up before releasing the
         * sim's next block ---- */
        uint64_t bp_start = audioio_monotonic_ms();
        while (size_buffer(capture_buffer) >
                   (size_t)SOCK_AUDIO_RX_HIGH_WATER_SAMPLES * sizeof(int32_t) &&
               !shutdown_ && !audio_shutdown_)
        {
            if (audioio_monotonic_ms() - bp_start > SOCK_AUDIO_BACKPRESSURE_CAP_MS)
            {
                HLOGW("audio-sock",
                      "demod did not drain below high water in %d ms; releasing block",
                      SOCK_AUDIO_BACKPRESSURE_CAP_MS);
                break;
            }
            ffthread_sleep(1);
        }

        /* ---- drain TX (i32 -> i16, silence-padded to n) ---- */
        size_t want  = (size_t)n * sizeof(int32_t);
        size_t avail = size_buffer(playback_buffer);
        avail -= avail % sizeof(int32_t);
        if (avail > want)
            avail = want;
        if (avail > 0)
            read_buffer(playback_buffer, (uint8_t *) ring32, avail);

        uint8_t ptt = (arq_get_trx() == 1) ? SOCK_WIRE_PTT_ON : SOCK_WIRE_PTT_OFF;
        size_t frame_len = sock_wire_build_station(txframe, seq, ptt, n,
                                                   ring32, avail / sizeof(int32_t));

        if (sock_send_full(fd, txframe, frame_len) != 1)
        {
            close(fd);
            fd = -1;
            if (!shutdown_ && !audio_shutdown_)
            {
                HLOGW("audio-sock", "send failed; reconnecting");
                fd = sock_connect_retry(path);
            }
        }
    }

    if (fd >= 0)
        close(fd);
    free(payload);
    free(ring32);
    free(txframe);
    HLOGI("audio-sock", "lockstep transport thread exit");
    return NULL;
}

/* Placeholder for the radio_playback slot: the lockstep thread owns both
 * directions, but teardown (audioio_stop_threads/audioio_deinit) joins two
 * threads, so both handles must be real. */
static void *sock_idle_thread(void *unused)
{
    (void) unused;
    while (!shutdown_ && !audio_shutdown_)
        ffthread_sleep(FIFO_AUDIO_POLL_MS * 10);
    return NULL;
}

/* Resolve the sim socket path into s_capture_dev: MERCURY_AUDIO_SOCK wins,
 * else the -i capture device string.  Returns NULL if neither is set. */
static const char *sock_resolve_path(void)
{
    const char *env = getenv("MERCURY_AUDIO_SOCK");
    if (env && env[0] != '\0')
    {
        strncpy(s_capture_dev, env, sizeof(s_capture_dev) - 1);
        s_capture_dev[sizeof(s_capture_dev) - 1] = '\0';
    }
    if (s_capture_dev[0] == '\0')
    {
        HLOGE("audio-sock", "no socket path: set MERCURY_AUDIO_SOCK (or -i <path>)");
        return NULL;
    }
    return s_capture_dev;
}

#endif /* !_WIN32 */


void *radio_playback_thread(void *device_ptr)
{
    ffaudio_interface *audio = NULL;
    struct conf conf = {};
    conf.buf.app_name = "mercury_playback";
    conf.buf.format = FFAUDIO_F_INT32;
    conf.buf.sample_rate = 48000;
    conf.buf.channels = 2;
    conf.buf.device_id = (device_ptr && ((const char *)device_ptr)[0] != '\0')
                         ? (const char *) device_ptr : NULL;
    uint32_t period_ms;


#if defined(_WIN32)
    conf.buf.buffer_length_msec = 40;
    period_ms = conf.buf.buffer_length_msec / 4;
    if (audio_subsystem == AUDIO_SUBSYSTEM_WASAPI)
        audio = (ffaudio_interface *) &ffwasapi;
    if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND)
        audio = (ffaudio_interface *) &ffdsound;
#elif defined(__linux__)
    conf.buf.buffer_length_msec = 30;
    period_ms = conf.buf.buffer_length_msec / 3;
    if (audio_subsystem == AUDIO_SUBSYSTEM_ALSA)
        audio = (ffaudio_interface *) &ffalsa;
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
        audio = (ffaudio_interface *) &ffpulse;
    if (audio_subsystem == AUDIO_SUBSYSTEM_OSS)
    {
        /* Buffer sizing follows the backend, not the OS, so use the same
         * request FreeBSD does.  OSS only treats this as a hint: it rounds to
         * the driver's fragment geometry and writes back what it really got
         * (observed here as 170 ms playback / 10 ms capture). */
        conf.buf.buffer_length_msec = 40;
        period_ms = conf.buf.buffer_length_msec / 4;
        audio = OSS_IFACE;
    }
#elif defined(__FreeBSD__)
    conf.buf.buffer_length_msec = 40;
    period_ms = conf.buf.buffer_length_msec / 4;
    if (audio_subsystem == AUDIO_SUBSYSTEM_OSS)
        audio = OSS_IFACE;
#elif defined(__APPLE__)
    conf.buf.buffer_length_msec = 40;
    period_ms = conf.buf.buffer_length_msec / 4;
    if (audio_subsystem == AUDIO_SUBSYSTEM_COREAUDIO)
        audio = (ffaudio_interface *) &ffcoreaudio;
#endif


#if defined(_WIN32)
    /* DirectSound device IDs are GUID strings from get_soundcard_list().
     * Convert back to binary GUID for the DirectSound API. */
    GUID play_guid;
    if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND && conf.buf.device_id &&
        conf.buf.device_id[0] == '{' && str_to_guid(conf.buf.device_id, &play_guid) == 0)
        conf.buf.device_id = (const char *)&play_guid;
#endif

    if (audio == NULL)
    {
        HLOGE("audio-playback", "sound system '%s' is not available in this build",
              cfg_sound_system_name(audio_subsystem));
        return NULL;
    }

    conf.flags = FFAUDIO_PLAYBACK;
    ffaudio_init_conf aconf = {};
    aconf.app_name = "mercury_playback";

    int r;
    ffaudio_buf *b;
    ffaudio_conf *cfg;

    ffuint frame_size;
    ffuint msec_bytes;

    /* Per-period resampler scratch.  These only ever hold ONE 8 kHz read
     * period (period_bytes_8k below) and its 1:6 upsampled 48 kHz stereo
     * expansion.  Sizing from SIGNAL_BUFFER_SIZE allocated ~590 MB for the
     * stereo buffer alone (~1 GB across both audio threads), which fails on
     * low-RAM 32-bit hosts such as the 512 MB Pi Zero2W — the modem could not
     * run there (issue #79).  Size from the actual period instead. */
    size_t period_scratch_bytes = (size_t) 8000u * sizeof(int32_t) * period_ms / 1000u + 64;

    // input is int32_t (8kHz samples from playback_buffer)
    int32_t *input_buffer = (int32_t *) malloc(period_scratch_bytes);

    /* Upsampled mono scratch.  Sized for the WIDEST supported ratio, not the
     * expected one: the device rate is not known until open() below, and a
     * 96 kHz negotiation would overrun a buffer sized for 1:6. */
    int32_t *buffer_upsampled = (int32_t *) malloc(period_scratch_bytes * RESAMP_L_MAX);

    // output is int32_t, up to stereo, at the device rate
    int32_t *buffer_output_stereo = (int32_t *) malloc(period_scratch_bytes * 2 * RESAMP_L_MAX);

    if (!input_buffer || !buffer_upsampled || !buffer_output_stereo)
    {
        HLOGE("audio-play", "Failed to allocate playback buffers");
        goto finish_play;
    }

    ffuint total_written = 0;
    int ch_layout = STEREO;

    /* Resampling ratio: set once the device has told us its real rate (below,
     * after open()), never assumed. */
    int resample_ratio = RESAMP_L;
    /* Non-NULL when the device rate is not an integer multiple of 8 kHz and
     * the rational engine is doing the conversion instead. */
    resamp_rat_t *rat_up = NULL;

    /* PulseAudio uses a single global context (gconn in pulse.c).
     * If init() returns "already initialized" it means the capture thread
     * already called init() successfully and we can proceed normally.
     * Track whether we initialized so we only uninit once.
     */
    bool did_init_play = false;
    r = audio->init(&aconf);
    if (r != 0)
    {
        if (aconf.error == NULL || strcmp(aconf.error, "already initialized") != 0)
        {
            HLOGE("audio-play", "Error in audio->init(): %s", aconf.error ? aconf.error : "unknown");
            goto finish_play;
        }
        // "already initialized" is fine - another thread owns the context
    }
    else
    {
        did_init_play = true;
    }

    // playback code...
    b = audio->alloc();
    if (b == NULL)
    {
        HLOGE("audio-play", "Error in audio->alloc()");
        goto finish_play;
    }

    cfg = &conf.buf;
    r = audio->open(b, cfg, conf.flags);
    if (r == FFAUDIO_EFORMAT)
        r = audio->open(b, cfg, conf.flags);
    if (r != 0)
    {
        HLOGE("audio-play", "error in audio->open(): %d: %s", r, audio->error(b));
        audio_health_set(false, AUDIO_HEALTH_FAILED, audio->error(b));
        goto cleanup_play;
    }

    char play_dev_display[96];
    format_device_display(audio_subsystem, FFAUDIO_DEV_PLAYBACK,
                           device_ptr ? (const char *)device_ptr : NULL,
                           play_dev_display, sizeof(play_dev_display));
    audio_health_set(false, AUDIO_HEALTH_RUNNING, NULL);
    HLOGI("audio-play", "I/O playback (%s) %s / %dHz / %dch / %dms buffer",
          play_dev_display,
          cfg->format == FFAUDIO_F_FLOAT32 ? "float32" :
          cfg->format == FFAUDIO_F_INT32   ? "int32"   :
          cfg->format == FFAUDIO_F_INT24_4 ? "int24in32" :
          cfg->format == FFAUDIO_F_INT24   ? "int24"   :
          cfg->format == FFAUDIO_F_INT16   ? "int16"   : "unknown",
          cfg->sample_rate, cfg->channels, cfg->buffer_length_msec);

    /* Channel-count safety.  buffer_output_stereo is sized for at most 2
     * channels, and the emit loop below only fills 2.  Forcing cfg->channels
     * back to 2 would avoid the overflow but leave frame_size disagreeing
     * with the device's real frame layout — every write would be
     * misinterpreted (wrong interleave, samples on the wrong channels) and
     * the radio would be keyed with garbage audio, the same failure mode the
     * format guard below exists to prevent.  So a >2-channel negotiation
     * (e.g. 6-ch WASAPI/CoreAudio surround sink) aborts like an unsupported
     * format; point Mercury at a stereo/mono endpoint (or a plug/dmix alias)
     * for such devices. */
    if (cfg->channels < 1 || cfg->channels > 2)
    {
        HLOGE("audio-play",
              "Device negotiated %d channels; only mono/stereo playback is supported, aborting",
              cfg->channels);
        goto cleanup_play;
    }

    /* Rate safety, same reasoning as the channel/format guards: resample by
     * what the device ACTUALLY negotiated, never by the 48 kHz we asked for.
     * A device already locked to another rate by a second client hands that
     * rate back, and a fixed 1:6 then transmits everything off-frequency by
     * rate/48000 with no other symptom — measured, a 1 kHz tone came out at
     * 166.8 Hz on a device pinned to 8 kHz: right level, spectrally pure,
     * wrong frequency.  Refuse rather than key the radio with that. */
    resample_ratio = resampler_ratio_for_rate((int)cfg->sample_rate);
    if (resample_ratio == 0)
    {
        /* Not an integer multiple of 8 kHz: convert by L/M instead (8000 ->
         * 44100 is 441/80).  Only a rate outside the supported set is fatal. */
        if (resampler_rate_supported((int)cfg->sample_rate))
            rat_up = resamp_rat_create(RESAMP_MODEM_FS, (int)cfg->sample_rate);

        if (!rat_up)
        {
            char why[220];
            snprintf(why, sizeof(why),
                     "device negotiated %u Hz, which the modem cannot resample "
                     "(supported 8k/11.025k/16k/22.05k/24k/32k/44.1k/48k/"
                     "88.2k/96k/176.4k/192k) -- %s",
                     cfg->sample_rate, audio_rate_mismatch_hint());
            HLOGE("audio-play", "%s", why);
            audio_health_set(false, AUDIO_HEALTH_FAILED, why);
            goto cleanup_play;
        }
        int rl = 0, rm = 0;
        resamp_rat_ratio(rat_up, &rl, &rm);
        HLOGI("audio-play", "device negotiated %u Hz; resampling %d/%d",
              cfg->sample_rate, rl, rm);
    }
    if (cfg->sample_rate != 48000)
        HLOGW("audio-play", "device negotiated %u Hz (not the requested 48000); "
              "resampling 1:%d", cfg->sample_rate, resample_ratio);

    frame_size = cfg->channels * (cfg->format & 0xff) / 8;
    msec_bytes = cfg->sample_rate * frame_size / 1000;

    /* The device can negotiate a different format/channel count than the
     * int32 / 2ch we requested at open().  A 16-bit-only USB codec (e.g. a
     * Yaesu opened via hw:) comes back FFAUDIO_F_INT16; some hosts return
     * float32; a mono-only card comes back with cfg->channels == 1.  Emit
     * samples in the negotiated layout below so the bytes we write match what
     * frame_size counts -- otherwise the device reads our int32 stereo as
     * garbage and only part of each period is written. */
    bool playback_is_float = (cfg->format == FFAUDIO_F_FLOAT32);
    bool playback_is_int16 = (cfg->format == FFAUDIO_F_INT16);
    /* FFAUDIO_F_INT24 is 24 bits PACKED into 3 bytes — the 24-bit format ALSA
     * and PulseAudio expose (SND_PCM_FORMAT_S24_3LE).  It is not the same as
     * FFAUDIO_F_INT24_4, which carries a 24-bit sample in a 4-byte container
     * and is what WASAPI negotiates on Windows.  Handling only the latter left
     * 24-bit playback working on Windows but rejected on Linux. */
    bool playback_is_int24 = (cfg->format == FFAUDIO_F_INT24);

    if (!playback_is_float && !playback_is_int16 && !playback_is_int24 &&
        cfg->format != FFAUDIO_F_INT32 && cfg->format != FFAUDIO_F_INT24_4)
    {
        HLOGE("audio-play", "Unsupported playback format %d, aborting", cfg->format);
        goto cleanup_play;
    }

    ch_layout = STEREO;
    
    /* Bytes per period of 8 kHz mono int32 modem audio.  The previous
     * formula (48 kHz x sizeof(double) / resample_ratio) worked out to
     * 8 bytes per 8 kHz sample, i.e. chunks of 2x period_ms. */
    uint32_t period_bytes_8k = 8000u * sizeof(int32_t) * period_ms / 1000u;

    /* Polyphase anti-imaging upsampler, stateful across periods (its filter
     * history bridges read boundaries, so no per-period click — issue #81).
     * Built for the rate the device ACTUALLY negotiated: resampling by the
     * requested 48 kHz ratio when the device came back at some other rate
     * transmits everything off-frequency, silently and convincingly. */
    resamp_up_t up_rs;
    if (!rat_up)
    {
        resampler_init_up(resample_ratio);
        resamp_up_reset(&up_rs);
    }

    while (!shutdown_ && !audio_shutdown_)
    {
        ffssize n;
        size_t buffer_size = size_buffer(playback_buffer);
        if (buffer_size == 0)
        {
            ffthread_sleep(period_ms ? period_ms : 5);
            continue;
        }
        if (buffer_size >= period_bytes_8k)
        {
            read_buffer(playback_buffer, (uint8_t *) input_buffer, period_bytes_8k);
            n = period_bytes_8k;
        }
        else
        {
            // we just play zeros if there is nothing to play
            memset(input_buffer, 0, period_bytes_8k);
            if (buffer_size > 0)
                read_buffer(playback_buffer, (uint8_t *) input_buffer, buffer_size);
            n = buffer_size;
        }

        total_written = 0;

        int samples_read_8k = n / sizeof(int32_t);

        /* Upsample 8 kHz -> the device rate through the polyphase anti-imaging
         * FIR: integer ratio where the rate allows it, rational L/M otherwise
         * (the 44.1 kHz family). */
        int samples_upsampled =
            rat_up ? resamp_rat_process(rat_up, input_buffer, samples_read_8k,
                                        buffer_upsampled)
                   : resamp_up_process(&up_rs, input_buffer, samples_read_8k,
                                       buffer_upsampled);

        /* Expand the upsampled mono modem signal into the device's negotiated
         * channel/format layout.  cfg->channels is 1 or 2; cfg->format is what
         * open() actually got (int16 / int32 / int24-in-32 / float32). */
        for (int i = 0; i < samples_upsampled; i++)
        {
            int32_t s = buffer_upsampled[i];
            int32_t left  = (ch_layout == RIGHT) ? 0 : s;
            int32_t right = (ch_layout == LEFT)  ? 0 : s;
            int idx = i * cfg->channels;

            if (playback_is_int16)
            {
                int16_t *o = (int16_t *) buffer_output_stereo;
                o[idx] = (int16_t)(left >> 16);
                if (cfg->channels > 1)
                    o[idx + 1] = (int16_t)(right >> 16);
            }
            else if (playback_is_int24)
            {
                /* 3 bytes per sample, little-endian, sample value scaled from
                 * int32 full scale down to 24-bit (/256 — the exact inverse of
                 * the capture side's *256).  Division, not >>8: it is defined
                 * for negatives, and the byte extraction goes through an
                 * unsigned copy because right-shifting a negative int is
                 * implementation-defined. */
                uint8_t *o = (uint8_t *) buffer_output_stereo;
                size_t off = (size_t) idx * PCM24_BYTES;
                pcm24_wr_le(o + off, left);
                if (cfg->channels > 1)
                    pcm24_wr_le(o + off + PCM24_BYTES, right);
            }
            else if (playback_is_float)
            {
                float *o = (float *) buffer_output_stereo;
                o[idx] = (float)(left / 2147483648.0);
                if (cfg->channels > 1)
                    o[idx + 1] = (float)(right / 2147483648.0);
            }
            else   /* int32 or 24-bit-in-32 container */
            {
                buffer_output_stereo[idx] = left;
                if (cfg->channels > 1)
                    buffer_output_stereo[idx + 1] = right;
            }
        }

        n = samples_upsampled * frame_size;

        while (n >= frame_size)
        {
            if (audio_shutdown_) break;  // exit fast on restart

            r = audio->write(b, ((uint8_t *)buffer_output_stereo) + total_written, n);

            if (r == -FFAUDIO_ESYNC) {
                HLOGW("audio-play", "detected underrun");
                continue;
            }
            if (r < 0)
            {
                HLOGE("audio-play", "ffaudio.write: %s", audio->error(b));
                break;
            }
            total_written += r;
            n -= r;
        }
        // printf("n = %lld total written = %u\n", n, total_written);
    }
    // Only drain when doing a full shutdown, not a restart
    // audio->drain() blocks until all buffered data is played out
    // which can hang indefinitely during a device switch
    if (!audio_shutdown_) {
        r = audio->drain(b);
        if (r < 0)
            HLOGE("audio-play", "ffaudio.drain: %s", audio->error(b));
    }
    r = audio->stop(b);
    if (r != 0)
        HLOGE("audio-play", "ffaudio.stop: %s", audio->error(b));

    r = audio->clear(b);
    if (r != 0)
        HLOGE("audio-play", "ffaudio.clear: %s", audio->error(b));

cleanup_play:
    resamp_rat_free(rat_up);
    rat_up = NULL;

    audio->free(b);

    // Only uninit if this thread was the one that initialized the PA context
    if (did_init_play)
        audio->uninit();

finish_play:

    free(input_buffer);
    free(buffer_upsampled);
    free(buffer_output_stereo);

    HLOGI("audio-play", "radio_playback_thread exit");

    // An audio-device failure (or a restart-initiated stop) ends only this
    // thread; it does NOT take the engine down.  The control/websocket
    // interfaces stay up so the operator can select a working device
    // (audioio_restart), and a transient glitch no longer kills the modem.

    return NULL;
}


void *radio_capture_thread(void *device_ptr)
{
    ffaudio_interface *audio = NULL;
    struct conf conf = {};
    conf.buf.app_name = "mercury_capture";
    conf.buf.format = FFAUDIO_F_INT32;
    conf.buf.sample_rate = 48000;
    conf.buf.channels = 2;
    conf.buf.device_id = (device_ptr && ((const char *)device_ptr)[0] != '\0')
                         ? (const char *) device_ptr : NULL;

#if defined(_WIN32)
    if (audio_subsystem == AUDIO_SUBSYSTEM_WASAPI) {
        conf.buf.buffer_length_msec = 40;
        audio = (ffaudio_interface *) &ffwasapi;
    }
    if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND) {
        /* DSound on Win10/11 is emulated via WASAPI. A small looping buffer
         * causes the write cursor to lap our read position between polls,
         * losing most captured data.  Use 500ms (DSound's own default). */
        conf.buf.buffer_length_msec = 200;
        audio = (ffaudio_interface *) &ffdsound;
    }
#elif defined(__linux__)
    conf.buf.buffer_length_msec = 30;
    if (audio_subsystem == AUDIO_SUBSYSTEM_ALSA)
        audio = (ffaudio_interface *) &ffalsa;
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
        audio = (ffaudio_interface *) &ffpulse;
    if (audio_subsystem == AUDIO_SUBSYSTEM_OSS)
    {
        conf.buf.buffer_length_msec = 40;
        audio = OSS_IFACE;
    }
#elif defined(__FreeBSD__)
    conf.buf.buffer_length_msec = 40;
    if (audio_subsystem == AUDIO_SUBSYSTEM_OSS)
        audio = OSS_IFACE;
#elif defined(__APPLE__)
    conf.buf.buffer_length_msec = 40;
    if (audio_subsystem == AUDIO_SUBSYSTEM_COREAUDIO)
        audio = (ffaudio_interface *) &ffcoreaudio;
#endif

#if defined(_WIN32)
    /* DirectSound device IDs are GUID strings from get_soundcard_list().
     * Convert back to binary GUID for the DirectSound API. */
    GUID cap_guid;
    if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND && conf.buf.device_id &&
        conf.buf.device_id[0] == '{' && str_to_guid(conf.buf.device_id, &cap_guid) == 0)
        conf.buf.device_id = (const char *)&cap_guid;
#endif

    if (audio == NULL)
    {
        HLOGE("audio-capture", "sound system '%s' is not available in this build",
              cfg_sound_system_name(audio_subsystem));
        return NULL;
    }

    /* Open non-blocking.  A blocking ffalsa_read() spins internally on
     * readonce->start->sleep forever once the device stops producing samples
     * (e.g. a half-duplex ALSA capture wedged after a TX): it never returns,
     * so the loop below could never recover it.  Non-blocking returns 0
     * immediately on no-data, handing control back so the stall watchdog can
     * tear the device down and reopen it. */
    conf.flags = FFAUDIO_CAPTURE | FFAUDIO_O_NONBLOCK;
    ffaudio_init_conf aconf = {};
    aconf.app_name = "mercury_capture";

    int r;
    ffaudio_buf *b;
    ffaudio_conf *cfg;

    ffuint frame_size;
    ffuint msec_bytes;

    int32_t *buffer = NULL;

    int ch_layout = STEREO;

    int32_t *buffer_output = NULL;
    int32_t *buffer_downsampled = NULL;

    /* Resampling ratio: set once the device reports its real rate (below,
     * after open()), never assumed. */
    int resample_ratio = RESAMP_L;
    resamp_rat_t *rat_down = NULL;

    /* PulseAudio uses a single global context (gconn in pulse.c).
     * If init() returns "already initialized" it means the playback thread
     * already called init() successfully and we can proceed normally.
     * Track whether we initialized so we only uninit once.
     */
    bool did_init_cap = false;
    r = audio->init(&aconf);
    if (r != 0)
    {
        if (aconf.error == NULL || strcmp(aconf.error, "already initialized") != 0)
        {
            HLOGE("audio-cap", "Error in audio->init(): %s", aconf.error ? aconf.error : "unknown");
            goto finish_cap;
        }
        // "already initialized" is fine - another thread owns the context
    }
    else
    {
        did_init_cap = true;
    }

    // capture code
    b = audio->alloc();
    if (b == NULL)
    {
        HLOGE("audio-cap", "Error in audio->alloc()");
        goto finish_cap;
    }

    cfg = &conf.buf;
    r = audio->open(b, cfg, conf.flags);
    if (r == FFAUDIO_EFORMAT)
        r = audio->open(b, cfg, conf.flags);
    if (r != 0)
    {
        HLOGE("audio-cap", "error in audio->open(): %d: %s", r, audio->error(b));
        audio_health_set(true, AUDIO_HEALTH_FAILED, audio->error(b));
        goto cleanup_cap;
    }

    char cap_dev_display[96];
    format_device_display(audio_subsystem, FFAUDIO_DEV_CAPTURE,
                           device_ptr ? (const char *)device_ptr : NULL,
                           cap_dev_display, sizeof(cap_dev_display));
    audio_health_set(true, AUDIO_HEALTH_RUNNING, NULL);
    HLOGI("audio-cap", "I/O capture (%s) %s / %dHz / %dch / %dms buffer",
          cap_dev_display,
          cfg->format == FFAUDIO_F_FLOAT32 ? "float32" :
          cfg->format == FFAUDIO_F_INT32   ? "int32"   :
          cfg->format == FFAUDIO_F_INT24_4 ? "int24in32" :
          cfg->format == FFAUDIO_F_INT24   ? "int24"   :
          cfg->format == FFAUDIO_F_INT16   ? "int16"   : "unknown",
          cfg->sample_rate, cfg->channels, cfg->buffer_length_msec);

    frame_size = cfg->channels * (cfg->format & 0xff) / 8;
    msec_bytes = cfg->sample_rate * frame_size / 1000;

    bool capture_is_float = (cfg->format == FFAUDIO_F_FLOAT32);
    bool capture_is_int16 = (cfg->format == FFAUDIO_F_INT16);
    /* 24-bit packed in 3 bytes — see the matching note in the playback path. */
    bool capture_is_int24 = (cfg->format == FFAUDIO_F_INT24);
    int  capture_channels = cfg->channels;

    if (!capture_is_float && !capture_is_int16 && !capture_is_int24 &&
        cfg->format != FFAUDIO_F_INT32 && cfg->format != FFAUDIO_F_INT24_4)
    {
        HLOGE("audio-cap", "Unsupported capture format %d, aborting", cfg->format);
        audio->free(b);
        return NULL;
    }

    /* Decimate by what the device ACTUALLY negotiated — see the matching
     * comment in the playback path.  On capture a wrong ratio detunes
     * everything we try to demodulate, so RX simply stops working. */
    resample_ratio = resampler_ratio_for_rate((int)cfg->sample_rate);
    if (resample_ratio == 0)
    {
        /* 44100 and 22050 are the common ones here: both are what a consumer
         * USB codec reports natively, and neither is an integer multiple of
         * the modem rate.  They are converted by L/M rather than refused --
         * on Windows and macOS there is no plug layer to fall back on, so
         * refusing meant the card simply could not be used (issue #193). */
        if (resampler_rate_supported((int)cfg->sample_rate))
            rat_down = resamp_rat_create((int)cfg->sample_rate, RESAMP_MODEM_FS);

        if (!rat_down)
        {
            char why[220];
            snprintf(why, sizeof(why),
                     "device negotiated %u Hz, which the modem cannot resample "
                     "(supported 8k/11.025k/16k/22.05k/24k/32k/44.1k/48k/"
                     "88.2k/96k/176.4k/192k) -- %s",
                     cfg->sample_rate, audio_rate_mismatch_hint());
            HLOGE("audio-cap", "%s", why);
            audio_health_set(true, AUDIO_HEALTH_FAILED, why);
            audio->free(b);
            return NULL;
        }
        int rl = 0, rm = 0;
        resamp_rat_ratio(rat_down, &rl, &rm);
        HLOGI("audio-cap", "device negotiated %u Hz; resampling %d/%d",
              cfg->sample_rate, rl, rm);
    }
    if (cfg->sample_rate != 48000)
        HLOGW("audio-cap", "device negotiated %u Hz (not the requested 48000); "
              "decimating %d:1", cfg->sample_rate, resample_ratio);

    /* Per-read resampler scratch, sized from the device buffer length (was
     * SIGNAL_BUFFER_SIZE — same ~1 GB oversizing as the playback path,
     * issue #79).  buffer_output holds one device read of mono device-rate
     * samples; buffer_downsampled its resample_ratio:1 decimation. */
    size_t cap_frames_max = (size_t) cfg->sample_rate * cfg->buffer_length_msec / 1000;
    cap_frames_max += cap_frames_max / 2 + 64;   /* margin over one read */

    buffer_output = (int32_t *) malloc(cap_frames_max * sizeof(int32_t));
    buffer_downsampled = (int32_t *) malloc(
        (rat_down ? (size_t)resamp_rat_max_out(rat_down, (int)cap_frames_max)
                  : cap_frames_max / resample_ratio + 2) * sizeof(int32_t));
    if (!buffer_output || !buffer_downsampled)
    {
        HLOGE("audio-cap", "Failed to allocate capture buffers");
        free(buffer_output);
        free(buffer_downsampled);
        buffer_output = NULL;
        buffer_downsampled = NULL;
        goto finish_cap;
    }

    ch_layout = capture_input_channel_layout;

    /* Polyphase anti-aliasing downsampler, stateful across reads. */
    resamp_down_t down_rs;
    if (!rat_down)
    {
        resampler_init_down(resample_ratio);
        if (rat_down) resamp_rat_reset(rat_down); else resamp_down_reset(&down_rs);
    }

    /* --- Capture rate diagnostics (prints every ~5 seconds) --- */
    uint64_t diag_start_ms = audioio_monotonic_ms();
    uint64_t diag_total_48k_frames = 0;   /* frames read from audio device (48kHz) */
    uint64_t diag_total_8k_samples = 0;   /* samples after downsampling (8kHz) */
    uint32_t diag_read_calls = 0;
    uint32_t diag_read_errors = 0;
    uint32_t diag_buf_full_drops = 0;
    int      diag_last_read_bytes = 0;

    /* Stall watchdog.  With the non-blocking open above, audio->read() returns
     * 0 the instant no samples are available instead of spinning inside
     * ffalsa_read() forever.  Brief idle is normal; if the device stays silent
     * past CAP_STALL_MS it is wedged (a half-duplex capture can stop delivering
     * after a TX and never self-recover) so we free and reopen it.  The window
     * is deliberately longer than a single control-frame transmission so a
     * normal TX gap on a full-duplex device does not trip it -- tune from the
     * on-air "reopen #N" log lines. */
    const uint64_t CAP_STALL_MS = 2000;
    const uint64_t CAP_POLL_MS  = 5;
    uint64_t cap_last_data_ms   = audioio_monotonic_ms();
    uint32_t diag_reopens       = 0;

    while (!shutdown_ && !audio_shutdown_)
    {
        r = audio->read(b, (const void **)&buffer);
        if (r < 0)
        {
            diag_read_errors++;
            HLOGE("audio-cap", "ffaudio.read: %s", audio->error(b));
        }
        if (r <= 0)
        {
            /* No samples this poll.  Reopen the device if it has been silent
             * past the watchdog window (wedged, not merely idle). */
            uint64_t now = audioio_monotonic_ms();
            if (now - cap_last_data_ms >= CAP_STALL_MS)
            {
                HLOGW("audio-cap",
                      "capture stalled %llu ms, reopening device (reopen #%u)",
                      (unsigned long long)(now - cap_last_data_ms), ++diag_reopens);
                audio->free(b);
                b = NULL;
                while (!shutdown_ && !audio_shutdown_)
                {
                    b = audio->alloc();
                    if (b != NULL && audio->open(b, cfg, conf.flags) == 0)
                        break;
                    HLOGE("audio-cap", "capture reopen failed: %s",
                          b ? audio->error(b) : "alloc()");
                    if (b != NULL) { audio->free(b); b = NULL; }
                    ffthread_sleep(200);
                }
                if (b == NULL)   /* shutdown requested mid-reopen */
                {
                    free(buffer_output);
                    free(buffer_downsampled);
                    goto finish_cap;
                }
                if (rat_down) resamp_rat_reset(rat_down); else resamp_down_reset(&down_rs);
                cap_last_data_ms = audioio_monotonic_ms();
            }
            ffthread_sleep(CAP_POLL_MS);
            continue;
        }

        cap_last_data_ms = audioio_monotonic_ms();
        diag_read_calls++;
        diag_last_read_bytes = r;

        int frames_read = r / frame_size;
        int frames_to_write = frames_read;

        /* S3: PipeWire's PulseAudio compatibility layer can return larger
         * fragments than the buffer_length_msec we requested.  Without a
         * bound check the loop below would write past the end of
         * buffer_output (cap_frames_max slots).  Clamp here; the excess
         * samples are silently dropped which is preferable to a heap
         * overflow. */
        if (frames_to_write > (int)cap_frames_max)
        {
            HLOGW("audio-cap",
                  "capture read %d frames exceeds scratch capacity %zu; clamping",
                  frames_to_write, (size_t)cap_frames_max);
            frames_to_write = (int)cap_frames_max;
        }

        // Extract one mono int32 sample per 48 kHz frame into the scratch
        // buffer, then run the polyphase anti-aliasing downsampler.  The old
        // path decimated 1-in-6 with NO filter, folding everything above
        // 4 kHz into the modem band.
        for (int i = 0; i < frames_to_write; i++)
        {
            int32_t sample;

            if (capture_channels == 1)
            {
                // Mono: one sample per frame
                if (capture_is_float)
                {
                    float fsample = ((float *)buffer)[i];
                    if (fsample > 1.0f) fsample = 1.0f;
                    else if (fsample < -1.0f) fsample = -1.0f;
                    sample = (int32_t)(fsample * 2147483647.0f);
                }
                else if (capture_is_int16)
                {
                    sample = (int32_t)((int16_t *)buffer)[i] * 65536;
                }
                else if (capture_is_int24)
                {
                    sample = pcm24_rd_le((const uint8_t *)buffer + (size_t)i * PCM24_BYTES);
                }
                else
                {
                    sample = buffer[i];
                }
            }
            else
            {
                // Stereo: two samples per frame, extract based on ch_layout
                if (capture_is_float)
                {
                    float *fbuf = (float *)buffer;
                    float fl = fbuf[i*2];
                    float fr = fbuf[i*2 + 1];
                    float fs;
                    if (ch_layout == LEFT)
                        fs = fl;
                    else if (ch_layout == RIGHT)
                        fs = fr;
                    else
                        fs = (fl + fr) * 0.5f;
                    if (fs > 1.0f) fs = 1.0f;
                    else if (fs < -1.0f) fs = -1.0f;
                    sample = (int32_t)(fs * 2147483647.0f);
                }
                else if (capture_is_int16)
                {
                    int16_t *i16buf = (int16_t *)buffer;
                    if (ch_layout == LEFT)
                        sample = (int32_t)i16buf[i*2] * 65536;
                    else if (ch_layout == RIGHT)
                        sample = (int32_t)i16buf[i*2 + 1] * 65536;
                    else
                        sample = ((int32_t)i16buf[i*2] + (int32_t)i16buf[i*2 + 1]) * 32768;
                }
                else if (capture_is_int24)
                {
                    /* 6 bytes per stereo frame: L then R, 3 bytes each. */
                    const uint8_t *p = (const uint8_t *)buffer + (size_t)i * 2u * PCM24_BYTES;
                    if (ch_layout == LEFT)
                        sample = pcm24_rd_le(p);
                    else if (ch_layout == RIGHT)
                        sample = pcm24_rd_le(p + PCM24_BYTES);
                    else
                        /* Widen before adding, as in the int32 branch below:
                         * two full-scale int32 values sum past INT32_MAX. */
                        sample = (int32_t)(((int64_t)pcm24_rd_le(p) +
                                            (int64_t)pcm24_rd_le(p + PCM24_BYTES)) / 2);
                }
                else
                {
                    if (ch_layout == LEFT)
                        sample = buffer[i*2];
                    else if (ch_layout == RIGHT)
                        sample = buffer[i*2 + 1];
                    else
                        /* Widen BEFORE adding: two int32 samples sum past
                         * INT32_MAX as soon as both pass half scale, and the
                         * signed overflow (UB) wraps to the opposite sign — a
                         * loud correlated stereo input came out sign-flipped.
                         * The int16 branch above is safe only because int16
                         * operands promote to int; this one has no headroom. */
                        sample = (int32_t)(((int64_t)buffer[i*2] +
                                            (int64_t)buffer[i*2 + 1]) / 2);
                }
            }

            buffer_output[i] = sample;   // mono 48 kHz scratch
        }

        int downsampled_frames =
            rat_down ? resamp_rat_process(rat_down, buffer_output,
                                          frames_to_write, buffer_downsampled)
                     : resamp_down_process(&down_rs, buffer_output,
                                           frames_to_write, buffer_downsampled);

        if (downsampled_frames > 0)
        {
            if (circular_buf_free_size(capture_buffer) >= (size_t)(downsampled_frames * sizeof(int32_t)))
                write_buffer(capture_buffer, (uint8_t *)buffer_downsampled, downsampled_frames * sizeof(int32_t));
            else
            {
                diag_buf_full_drops += downsampled_frames;
                HLOGW("audio-cap", "Buffer full in capture buffer!");
            }
        }

        diag_total_48k_frames += frames_read;
        diag_total_8k_samples += downsampled_frames;

        /* Print diagnostics every ~5 seconds */
        uint64_t diag_now = audioio_monotonic_ms();
        uint64_t diag_elapsed = diag_now - diag_start_ms;
        if (diag_elapsed >= 5000)
        {
#ifdef DEBUG_IO
            double elapsed_sec = diag_elapsed / 1000.0;
            double rate_48k = diag_total_48k_frames / elapsed_sec;
            double rate_8k  = diag_total_8k_samples / elapsed_sec;
            size_t buf_used = size_buffer(capture_buffer);
            size_t buf_free = circular_buf_free_size(capture_buffer);
            HLOGD("audio-cap",
                  "DIAG: %.1fs | reads=%u errs=%u | 48kHz=%.0f Hz (expect 48000) | 8kHz=%.0f Hz (expect 8000) | last_read=%d B | ringbuf used=%zu free=%zu | drops=%u",
                  elapsed_sec, diag_read_calls, diag_read_errors,
                  rate_48k, rate_8k, diag_last_read_bytes,
                  buf_used, buf_free, diag_buf_full_drops);
#endif /* DEBUG_IO */
            /* reset counters */
            diag_start_ms = diag_now;
            diag_total_48k_frames = 0;
            diag_total_8k_samples = 0;
            diag_read_calls = 0;
            diag_read_errors = 0;
            diag_buf_full_drops = 0;
        }
    }

    r = audio->stop(b);
    if (r != 0)
        HLOGE("audio-cap", "ffaudio.stop: %s", audio->error(b));

    r = audio->clear(b);
    if (r != 0)
        HLOGE("audio-cap", "ffaudio.clear: %s", audio->error(b));

    free(buffer_output);
    free(buffer_downsampled);

cleanup_cap:

    audio->free(b);

    // Only uninit if this thread was the one that initialized the PA context
    if (did_init_cap)
        audio->uninit();

finish_cap:
    resamp_rat_free(rat_down);
    rat_down = NULL;
    HLOGI("audio-cap", "radio_capture_thread exit");

    // An audio-device failure (or a restart-initiated stop) ends only this
    // thread; it does NOT take the engine down.  The control/websocket
    // interfaces stay up so the operator can select a working device
    // (audioio_restart), and a transient glitch no longer kills the modem.

    return NULL;
}

/* Enumerate devices.  `pulse_lock_held` is for the one caller that already
 * owns s_pulse_lock (audioio_restart): that mutex is NOT recursive, so
 * re-taking it here self-deadlocks the process with audio already stopped. */
static int get_soundcard_list_int(int audio_system, int mode,
                                  char ids[][AUDIO_DEV_STR_MAX], char dev_names[][AUDIO_DEV_STR_MAX], int max_count,
                                  bool pulse_lock_held)
{
    ffaudio_interface *audio = NULL;
    (void) pulse_lock_held;
    int count = 0;
    bool did_init = false;

    if (audio_system == AUDIO_SUBSYSTEM_SHM ||
        audio_system == AUDIO_SUBSYSTEM_NULL ||
        audio_system == AUDIO_SUBSYSTEM_FIFO)
        return 0;

#if defined(_WIN32)
    if (audio_system == AUDIO_SUBSYSTEM_WASAPI)
        audio = (ffaudio_interface *) &ffwasapi;
    if (audio_system == AUDIO_SUBSYSTEM_DSOUND)
        audio = (ffaudio_interface *) &ffdsound;
#elif defined(__linux__)
    if (audio_system == AUDIO_SUBSYSTEM_ALSA)
        audio = (ffaudio_interface *) &ffalsa;
    if (audio_system == AUDIO_SUBSYSTEM_PULSE)
        audio = (ffaudio_interface *) &ffpulse;
    if (audio_system == AUDIO_SUBSYSTEM_OSS)
        audio = OSS_IFACE;
#elif defined(__FreeBSD__)
    if (audio_system == AUDIO_SUBSYSTEM_OSS)
        audio = OSS_IFACE;
#elif defined(__APPLE__)
    if (audio_system == AUDIO_SUBSYSTEM_COREAUDIO)
        audio = (ffaudio_interface *) &ffcoreaudio;
#elif defined(__ANDROID__)
    if (audio_system == AUDIO_SUBSYSTEM_AAUDIO)
        audio = (ffaudio_interface *) &ffaaudio;
#endif

    if (!audio)
        return 0;

#if defined(__linux__)
    if (audio_system == AUDIO_SUBSYSTEM_PULSE)
    {
        if (!pulse_lock_held)
            pthread_mutex_lock(&s_pulse_lock);
        if (pulse_shared_init(&did_init) != 0)
        {
            if (!pulse_lock_held)
                pthread_mutex_unlock(&s_pulse_lock);
            return 0;
        }
    }
    else
#endif
    {
        ffaudio_init_conf aconf = {};
        if (audio->init(&aconf) != 0)
            return 0;
        did_init = true;
    }

    ffaudio_dev *d = audio->dev_alloc(mode);
    if (d == NULL)
    {
        if (did_init)
            audio->uninit();
#if defined(__linux__)
        if (audio_system == AUDIO_SUBSYSTEM_PULSE && !pulse_lock_held)
            pthread_mutex_unlock(&s_pulse_lock);
#endif
        return 0;
    }

    for (;;)
    {
        int r = audio->dev_next(d);
        if (r != 0)
            break;
        const char *id = audio->dev_info(d, FFAUDIO_DEV_ID);
        const char *name = audio->dev_info(d, FFAUDIO_DEV_NAME);
        if (id && count < max_count)
        {
            strncpy(ids[count], id, AUDIO_DEV_STR_MAX - 1);
            ids[count][AUDIO_DEV_STR_MAX - 1] = '\0';
            if (name) {
                strncpy(dev_names[count], name, AUDIO_DEV_STR_MAX - 1);
                dev_names[count][AUDIO_DEV_STR_MAX - 1] = '\0';
            } else {
                strncpy(dev_names[count], id, AUDIO_DEV_STR_MAX - 1);
                dev_names[count][AUDIO_DEV_STR_MAX - 1] = '\0';
            }
            count++;
        }
    }

    audio->dev_free(d);
    if (did_init)
        audio->uninit();
#if defined(__linux__)
    if (audio_system == AUDIO_SUBSYSTEM_PULSE && !pulse_lock_held)
        pthread_mutex_unlock(&s_pulse_lock);
#endif
    return count;
}

int get_soundcard_list(int audio_system, int mode,
                       char ids[][AUDIO_DEV_STR_MAX], char dev_names[][AUDIO_DEV_STR_MAX], int max_count)
{
    return get_soundcard_list_int(audio_system, mode, ids, dev_names, max_count, false);
}

#define DEVICE_RESOLVE_MAX 64

/* Resolve a user/config-supplied capture or playback device string to the
 * native device id the active backend expects, in place.
 *
 * Priority order:
 *   1. exact id match (e.g. "149", or a WASAPI/DSound GUID) -- left as-is,
 *      this is already what audio->open() wants, and is the fast path for
 *      anything already using -z output or an existing config.
 *   2. exact device name match, case-insensitive.
 *   3. a single, unambiguous case-insensitive substring match against a
 *      device name (e.g. "blackhole 2ch" or "usb").
 *
 * If the string is empty (default device), the subsystem has no
 * enumerable devices (NULL/FIFO/SOCK/SHM), enumeration fails, or a
 * substring match is ambiguous, buf is left untouched -- callers get
 * exactly today's behavior. */
static void resolve_device_string(int audio_subsys, int mode, char *buf, size_t bufsz,
                                  const char *log_tag, bool pulse_lock_held)
{
    /* An empty device means "the default".  Most backends take that as NULL and
     * choose sensibly, but OSS cannot: ffaudio falls back to /dev/dsp for BOTH
     * directions, and on OSSv4 /dev/dsp is one node -- commonly playback-only.
     * Capture then opens a playback device and spins on EACCES/EBUSY.  So for
     * OSS resolve an empty device to the first one enumerated for THIS
     * direction. */
    bool want_default = (buf[0] == '\0');
    if (want_default && audio_subsys != AUDIO_SUBSYSTEM_OSS)
        return;

    if (audio_subsys == AUDIO_SUBSYSTEM_SHM || audio_subsys == AUDIO_SUBSYSTEM_NULL ||
        audio_subsys == AUDIO_SUBSYSTEM_FIFO || audio_subsys == AUDIO_SUBSYSTEM_SOCK)
        return;

    /* Heap, not stack: DEVICE_RESOLVE_MAX x AUDIO_DEV_STR_MAX is 16 KB per
     * array, and this runs on secondary threads whose default stack is only
     * 512 KB on macOS.  Two 16 KB frames are survivable but pointless -- the
     * enumeration allocates far more internally anyway. */
    char (*ids)[AUDIO_DEV_STR_MAX]   = malloc(sizeof(*ids)   * DEVICE_RESOLVE_MAX);
    char (*names)[AUDIO_DEV_STR_MAX] = malloc(sizeof(*names) * DEVICE_RESOLVE_MAX);
    if (!ids || !names) {
        free(ids); free(names);
        HLOGE(log_tag, "out of memory enumerating audio devices");
        return;
    }
    int n = get_soundcard_list_int(audio_subsys, mode, ids, names, DEVICE_RESOLVE_MAX, pulse_lock_held);
    if (n <= 0) {
        free(ids); free(names);
        return;
    }

    if (want_default) {
        snprintf(buf, bufsz, "%s", ids[0]);
        HLOGI(log_tag, "using default %s device '%s' (%s)",
              (mode == FFAUDIO_DEV_CAPTURE) ? "capture" : "playback", ids[0], names[0]);
        goto done;
    }

    bool already_native = false;
    for (int i = 0; i < n; i++) {
        if (strcmp(buf, ids[i]) == 0) {
            already_native = true;   // already a valid native id
            break;
        }
    }
    if (already_native)
        goto done;

    /* Count ALL exact matches rather than stopping at the first: identical
     * display names are common (two of the same USB codec, or a card and its
     * loopback), and silently binding whichever enumerates first can key the
     * wrong radio -- in a feature whose whole premise is that ids move. */
    int match = -1, matches = 0;
    for (int i = 0; i < n; i++) {
        if (ffsz_ieq(buf, names[i])) {
            if (matches == 0)
                match = i;
            matches++;
        }
    }

    if (matches == 0) {
        size_t buf_len = strlen(buf);
        for (int i = 0; i < n; i++) {
            if (ffs_ifindstr(names[i], strlen(names[i]), buf, buf_len) >= 0) {
                match = i;
                matches++;
            }
        }
    }

    if (matches == 1) {
        HLOGI(log_tag, "device '%s' matched by name to '%s' (id=%s)", buf, names[match], ids[match]);
        strncpy(buf, ids[match], bufsz - 1);
        buf[bufsz - 1] = '\0';
    } else if (matches > 1) {
        HLOGW(log_tag, "device name '%s' is ambiguous (%d matches) -- use an exact id from -z instead", buf, matches);
    } else {
        /* Not a prediction of failure: enumeration does not see every valid
         * node.  Every /dev/dsp* symlink is a working OSS device that never
         * appears in SNDCTL_AUDIOINFO_EX under that name, and ALSA takes
         * plughw:/hw: strings that are absent from the list too.  If the open
         * really does fail, that is reported with the driver's own reason. */
        HLOGI(log_tag, "device '%s' is not in the enumerated list -- passing it to the "
                       "driver as given (-z lists the enumerated devices)", buf);
    }

done:
    free(ids);
    free(names);
}

/* Format "<id> '<name>'" for a resolved native device id, for display in
 * startup logs. Falls back to just the id if the id isn't found in the
 * current device list (e.g. subsystem without enumeration support), and to
 * "default" if no id was requested. out is always NUL-terminated. */
static void format_device_display(int audio_subsys, int mode, const char *id, char *out, size_t outsz)
{
    if (id == NULL || id[0] == '\0') {
        snprintf(out, outsz, "default");
        return;
    }

    /* Heap for the same reason as resolve_device_string() above. */
    char (*ids)[AUDIO_DEV_STR_MAX]   = malloc(sizeof(*ids)   * DEVICE_RESOLVE_MAX);
    char (*names)[AUDIO_DEV_STR_MAX] = malloc(sizeof(*names) * DEVICE_RESOLVE_MAX);
    if (!ids || !names) {
        free(ids); free(names);
        snprintf(out, outsz, "%s", id);
        return;
    }

    int n = get_soundcard_list(audio_subsys, mode, ids, names, DEVICE_RESOLVE_MAX);
    for (int i = 0; i < n; i++) {
        if (strcmp(id, ids[i]) == 0) {
            snprintf(out, outsz, "%s '%s'", id, names[i]);
            free(ids); free(names);
            return;
        }
    }

    free(ids); free(names);
    snprintf(out, outsz, "%s", id);
}

void list_soundcards(int audio_system)
{
    ffaudio_interface *audio = NULL;
    bool did_init = false;
    audio_subsystem = audio_system;

    if (audio_subsystem == AUDIO_SUBSYSTEM_SHM)
    {
        // TODO: connect to the shared memory
        printf("Shared Memory (SHM) audio subsystem selected.\n");
        audio = NULL;
        return;
    }
    if (audio_subsystem == AUDIO_SUBSYSTEM_NULL)
    {
        printf("Null audio subsystem selected (developer/test backend; no devices).\n");
        audio = NULL;
        return;
    }
    if (audio_subsystem == AUDIO_SUBSYSTEM_FIFO)
    {
        printf("FIFO audio subsystem selected (developer/test backend; no devices).\n");
        audio = NULL;
        return;
    }
    
#if defined(_WIN32)
    if (audio_subsystem == AUDIO_SUBSYSTEM_WASAPI)
        audio = (ffaudio_interface *) &ffwasapi;
    if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND)
        audio = (ffaudio_interface *) &ffdsound;
#elif defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_ALSA)
    {
        printf("Listing ALSA soundcards:\n");
        audio = (ffaudio_interface *) &ffalsa;
    }
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
        audio = (ffaudio_interface *) &ffpulse;
    if (audio_subsystem == AUDIO_SUBSYSTEM_OSS)
    {
        audio = OSS_IFACE;
        if (audio)
            printf("Listing OSS soundcards:\n");
    }
#elif defined(__FreeBSD__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_OSS)
        audio = OSS_IFACE;
#elif defined(__APPLE__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_COREAUDIO)
        audio = (ffaudio_interface *) &ffcoreaudio;
#elif defined(__ANDROID__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_AAUDIO)
        audio = (ffaudio_interface *) &ffaaudio;
#endif

    if (!audio)
    {
        printf("Sound system '%s' is not available in this build.\n",
               cfg_sound_system_name(audio_subsystem));
        return;
    }

#if defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
    {
        if (pulse_shared_init(&did_init) != 0)
        {
            printf("Error in audio->init()\n");
            return;
        }
    }
    else
#endif
    {
        ffaudio_init_conf aconf = {};
        if (audio->init(&aconf) != 0)
        {
            printf("Error in audio->init()\n");
            return;
        }
        did_init = true;
    }

    ffaudio_dev *d;

    // FFAUDIO_DEV_PLAYBACK, FFAUDIO_DEV_CAPTURE
    static const char* const mode[] = { "playback", "capture" };
    for (ffuint i = 0;  i != 2;  i++)
    {
        printf("%s devices:\n", mode[i]);
        d = audio->dev_alloc(i);
        if (d == NULL)
        {
            printf("Error in audio->dev_alloc\n");
            if (did_init)
                audio->uninit();
            return;
        }

        for (;;)
        {
            int r = audio->dev_next(d);
            if (r > 0)
                break;
            else
                if (r < 0)
                {
                    printf("error: %s", audio->dev_error(d));
                    break;
                }

            printf("device: name: '%s'  id: '%s'  default: %s\n"
                   , audio->dev_info(d, FFAUDIO_DEV_NAME)
                   , audio->dev_info(d, FFAUDIO_DEV_ID)
                   , audio->dev_info(d, FFAUDIO_DEV_IS_DEFAULT)
                );
        }

        audio->dev_free(d);
    }

    if (did_init)
        audio->uninit();
}

static int audioio_init_local_buffers(void)
{
    uint8_t *buffer_cap = (uint8_t *) malloc(SIGNAL_BUFFER_SIZE);
    uint8_t *buffer_play = (uint8_t *) malloc(SIGNAL_BUFFER_SIZE);
    if (!buffer_cap || !buffer_play)
    {
        free(buffer_cap);
        free(buffer_play);
        return -1;
    }

    capture_buffer = circular_buf_init(buffer_cap, SIGNAL_BUFFER_SIZE);
    playback_buffer = circular_buf_init(buffer_play, SIGNAL_BUFFER_SIZE);
    if (!capture_buffer || !playback_buffer)
    {
        if (capture_buffer)
            circular_buf_free(capture_buffer);
        if (playback_buffer)
            circular_buf_free(playback_buffer);
        free(buffer_cap);
        free(buffer_play);
        capture_buffer = NULL;
        playback_buffer = NULL;
        return -1;
    }
    return 0;
}

static void audioio_deinit_local_buffers(void)
{
    if (capture_buffer)
    {
        free(capture_buffer->buffer);
        circular_buf_free(capture_buffer);
    }
    if (playback_buffer)
    {
        free(playback_buffer->buffer);
        circular_buf_free(playback_buffer);
    }
    capture_buffer = NULL;
    playback_buffer = NULL;
}

int audioio_init_buffers(void)
{
    if (s_buffers_initialized)
        return 0;  // already created

    if (audio_subsystem == AUDIO_SUBSYSTEM_SHM)
    {
        // Named cross-process SHM: the transport to an EXTERNAL signal source
        // (the radio daemon, e.g. sbitx_controller, via -x shm).  Only this
        // backend needs the fixed SHM names.
        capture_buffer  = circular_buf_init_shm(SIGNAL_BUFFER_SIZE, (char *) SIGNAL_INPUT);
        playback_buffer = circular_buf_init_shm(SIGNAL_BUFFER_SIZE, (char *) SIGNAL_OUTPUT);
    }
    else
    {
        // ALSA / PULSE / OSS / CoreAudio / WASAPI / DSOUND / NULL / FIFO: a real
        // (or loopback) sound device, self-contained.  The capture/playback
        // rings are just in-process staging between the audio threads and the
        // modem (same process), so they need NOT be named SHM.  Anonymous local
        // buffers let multiple mercury instances run on one host (e.g. two wired
        // via snd-aloop + a channel sim) without colliding on the fixed SHM
        // names.  (The radio-daemon audio path is the SHM backend above.)
        if (audioio_init_local_buffers() != 0)
            return -1;
    }

    clear_buffer(capture_buffer);
    clear_buffer(playback_buffer);
    s_buffers_initialized = 1;
    return 0;
}

void audioio_deinit_buffers(void)
{
    if (!s_buffers_initialized)
        return;

    if (audio_subsystem == AUDIO_SUBSYSTEM_NULL ||
        audio_subsystem == AUDIO_SUBSYSTEM_FIFO)
    {
        audioio_deinit_local_buffers();
    }
#if defined(_WIN32)
    else
    {
        audioio_deinit_local_buffers();
    }
#else
    /* S5-item1: d99e3d8 moved ALSA/Pulse/OSS/CoreAudio from named SHM to
     * in-process malloc'd buffers (audioio_init_local_buffers).  The
     * original else-branch here unconditionally called
     * circular_buf_destroy_shm (munmap of a heap pointer + shm_unlink of
     * a wrong segment name) for every non-NULL/FIFO backend, which is
     * wrong and causes heap corruption.  Only AUDIO_SUBSYSTEM_SHM needs
     * the SHM teardown path; every other non-NULL/FIFO backend was
     * allocated locally and must go through the local free path. */
    else if (audio_subsystem == AUDIO_SUBSYSTEM_SHM)
    {
        circular_buf_destroy_shm(capture_buffer, SIGNAL_BUFFER_SIZE, (char *) SIGNAL_INPUT);
        circular_buf_free_shm(capture_buffer);
        circular_buf_destroy_shm(playback_buffer, SIGNAL_BUFFER_SIZE, (char *) SIGNAL_OUTPUT);
        circular_buf_free_shm(playback_buffer);
        capture_buffer = NULL;
        playback_buffer = NULL;
    }
    else
    {
        audioio_deinit_local_buffers();
    }
#endif
    s_buffers_initialized = 0;
}

int audioio_init_internal(char *capture_dev, char *playback_dev, int audio_subsys, int capture_channel_layout, pthread_t *radio_capture,
                          pthread_t *radio_playback)
{
    audio_subsystem = audio_subsys;
    if (capture_channel_layout == LEFT ||
        capture_channel_layout == RIGHT ||
        capture_channel_layout == STEREO)
        capture_input_channel_layout = capture_channel_layout;
    else
        capture_input_channel_layout = LEFT;

    // Store device names for restart support
    if (capture_dev)
    {
        strncpy(s_capture_dev, capture_dev, sizeof(s_capture_dev) - 1);
        s_capture_dev[sizeof(s_capture_dev) - 1] = '\0';
    }
    else
        s_capture_dev[0] = '\0';
    if (playback_dev)
    {
        strncpy(s_playback_dev, playback_dev, sizeof(s_playback_dev) - 1);
        s_playback_dev[sizeof(s_playback_dev) - 1] = '\0';
    }
    else
        s_playback_dev[0] = '\0';

    resolve_device_string(audio_subsystem, FFAUDIO_DEV_CAPTURE, s_capture_dev, sizeof(s_capture_dev), "audio-cap", false);
    resolve_device_string(audio_subsystem, FFAUDIO_DEV_PLAYBACK, s_playback_dev, sizeof(s_playback_dev), "audio-play", false);

    // Create buffers if not already created
    if (audioio_init_buffers() != 0)
        return -1;

    if (audio_subsystem == AUDIO_SUBSYSTEM_FIFO)
    {
        fifo_ignore_sigpipe();
        pthread_create(radio_capture, NULL, fifo_capture_thread, (void *) s_capture_dev);
        pthread_create(radio_playback, NULL, fifo_playback_thread, (void *) s_playback_dev);
        s_radio_capture = *radio_capture;
        s_radio_playback = *radio_playback;
        return 0;
    }

    if (audio_subsystem == AUDIO_SUBSYSTEM_SOCK)
    {
#ifdef _WIN32
        HLOGE("audio-sock", "-x sock is not supported on Windows");
        return -1;
#else
        const char *sock_path = sock_resolve_path();
        if (!sock_path)
            return -1;
        fifo_ignore_sigpipe();
        pthread_create(radio_capture, NULL, sock_transport_thread, (void *) s_capture_dev);
        pthread_create(radio_playback, NULL, sock_idle_thread, NULL);
        s_radio_capture = *radio_capture;
        s_radio_playback = *radio_playback;
        return 0;
#endif
    }

    if (audio_subsystem == AUDIO_SUBSYSTEM_NULL)
    {
        pthread_create(radio_capture, NULL, null_capture_thread, NULL);
        pthread_create(radio_playback, NULL, null_playback_thread, NULL);
        s_radio_capture = *radio_capture;
        s_radio_playback = *radio_playback;
        return 0;
    }

    /* Pre-initialize PulseAudio once here in the main thread before spawning
     * capture/playback threads. ffpulse_init() uses a single global context
     * (gconn) and returns an error if called more than once. By initializing
     * here, both threads will see "already initialized" and proceed normally
     * rather than one of them failing and exiting early.
     */
#if defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
    {
        (void) pulse_shared_init(NULL);
    }
#endif

    pthread_create(radio_capture, NULL, radio_capture_thread, (void *) s_capture_dev);
    pthread_create(radio_playback, NULL, radio_playback_thread, (void *) s_playback_dev);

    // Keep internal copies of thread handles
    s_radio_capture = *radio_capture;
    s_radio_playback = *radio_playback;

    return 0;
}

static void audioio_stop_threads(void)
{
    audio_shutdown_ = true;
    pthread_join(s_radio_capture, NULL);
    pthread_join(s_radio_playback, NULL);
    audio_shutdown_ = false;

    HLOGI("audio-stop", "audioio threads stopped");
}

int audioio_restart(const char *capture_dev, const char *playback_dev,
                    int audio_subsys, int capture_channel_layout)
{
    HLOGI("audio-restart", "stopping audio threads...");
    audioio_stop_threads();

#if defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE || audio_subsys == AUDIO_SUBSYSTEM_PULSE)
        pthread_mutex_lock(&s_pulse_lock);
    bool was_pulse = (audio_subsystem == AUDIO_SUBSYSTEM_PULSE);
    if (was_pulse)
        pulse_shared_uninit();
#endif

    // Update stored parameters
    audio_subsystem = audio_subsys;
    if (capture_channel_layout == LEFT ||
        capture_channel_layout == RIGHT ||
        capture_channel_layout == STEREO)
        capture_input_channel_layout = capture_channel_layout;
    else
        capture_input_channel_layout = LEFT;

    if (capture_dev && capture_dev[0] != '\0')
    {
        strncpy(s_capture_dev, capture_dev, sizeof(s_capture_dev) - 1);
        s_capture_dev[sizeof(s_capture_dev) - 1] = '\0';
    }

    if (playback_dev && playback_dev[0] != '\0')
    {
        strncpy(s_playback_dev, playback_dev, sizeof(s_playback_dev) - 1);
        s_playback_dev[sizeof(s_playback_dev) - 1] = '\0';
    }

    resolve_device_string(audio_subsystem, FFAUDIO_DEV_CAPTURE, s_capture_dev, sizeof(s_capture_dev), "audio-cap", true);
    resolve_device_string(audio_subsystem, FFAUDIO_DEV_PLAYBACK, s_playback_dev, sizeof(s_playback_dev), "audio-play", true);

    // Clear buffers (NEVER destroy/recreate them)
    clear_buffer(capture_buffer);
    clear_buffer(playback_buffer);

    HLOGI("audio-restart", "starting audio threads (capture=%s playback=%s channel=%d)...",
           s_capture_dev[0] ? s_capture_dev : "default",
           s_playback_dev[0] ? s_playback_dev : "default",
           capture_input_channel_layout);

    if (audio_subsystem == AUDIO_SUBSYSTEM_NULL)
    {
#if defined(__linux__)
        if (was_pulse || audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
            pthread_mutex_unlock(&s_pulse_lock);
#endif
        pthread_create(&s_radio_capture, NULL, null_capture_thread, NULL);
        pthread_create(&s_radio_playback, NULL, null_playback_thread, NULL);
        HLOGI("audio-restart", "null audio threads restarted");
        return 0;
    }

    if (audio_subsystem == AUDIO_SUBSYSTEM_FIFO)
    {
#if defined(__linux__)
        if (was_pulse || audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
            pthread_mutex_unlock(&s_pulse_lock);
#endif
        fifo_ignore_sigpipe();
        pthread_create(&s_radio_capture, NULL, fifo_capture_thread, (void *) s_capture_dev);
        pthread_create(&s_radio_playback, NULL, fifo_playback_thread, (void *) s_playback_dev);
        HLOGI("audio-restart", "FIFO audio threads restarted");
        return 0;
    }

#ifndef _WIN32
    if (audio_subsystem == AUDIO_SUBSYSTEM_SOCK)
    {
#if defined(__linux__)
        if (was_pulse || audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
            pthread_mutex_unlock(&s_pulse_lock);
#endif
        if (!sock_resolve_path())
            return -1;
        fifo_ignore_sigpipe();
        pthread_create(&s_radio_capture, NULL, sock_transport_thread, (void *) s_capture_dev);
        pthread_create(&s_radio_playback, NULL, sock_idle_thread, NULL);
        HLOGI("audio-restart", "sock lockstep threads restarted");
        return 0;
    }
#endif

#if defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
    {
        (void) pulse_shared_init(NULL);
    }
#endif

    pthread_create(&s_radio_capture, NULL, radio_capture_thread, (void *) s_capture_dev);
    pthread_create(&s_radio_playback, NULL, radio_playback_thread, (void *) s_playback_dev);

#if defined(__linux__)
    if (was_pulse || audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
        pthread_mutex_unlock(&s_pulse_lock);
#endif

    HLOGI("audio-restart", "audio threads restarted");
    return 0;
}

int audioio_deinit(pthread_t *radio_capture, pthread_t *radio_playback)
{
    // The external thread handles may be stale after a restart; use internal statics instead.
    (void) radio_capture;
    (void) radio_playback;
    pthread_join(s_radio_capture, NULL);
    pthread_join(s_radio_playback, NULL);

#if defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
    {
        pthread_mutex_lock(&s_pulse_lock);
        pulse_shared_uninit();
        pthread_mutex_unlock(&s_pulse_lock);
    }
#endif

    audioio_deinit_buffers();
    return 0;
}
