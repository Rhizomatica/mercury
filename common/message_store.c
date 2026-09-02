/* Mercury Modem — persistent message store
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "message_store.h"
#include "hermes_log.h"

#define MSG_STORE_LOG_TAG "store"

/* Stored message caps.  Chat lines are short; anything longer is more likely a
 * file transfer chunk than a message, so it is not stored. */
#define MSG_MAX_TEXT 1024
#define MSG_MAX_PEER 32

/* A JSONL line: worst case is every text byte needing a \u00xx escape (6x), so
 * keep the line buffer comfortably above MSG_MAX_TEXT * 6 + overhead. */
#define MSG_MAX_LINE 8192

/* Per-plane/direction partial-line assembly buffer. */
#define FEED_BUF_SIZE (MSG_MAX_TEXT + 2)

typedef enum
{
    PLANE_ARQ,
    PLANE_BCAST,
    PLANE_COUNT
} plane_t;

typedef enum
{
    DIR_RX,
    DIR_TX,
    DIR_COUNT
} dir_t;

static struct
{
    bool         enabled;
    FILE        *fp;                 /* append-mode JSONL file, or NULL */
    char         path[1024];

    char (*lines)[MSG_MAX_LINE];     /* in-memory ring of raw JSONL lines */
    int          cap;
    int          start;              /* index of oldest entry */
    int          count;

    /* Partial line being assembled per (plane, dir). */
    char         feed[PLANE_COUNT][DIR_COUNT][FEED_BUF_SIZE];
    size_t       feed_len[PLANE_COUNT][DIR_COUNT];
    bool         discarding[PLANE_COUNT][DIR_COUNT];
} g_store;

/* A statically-initialised mutex, never destroyed.  The ARQ event-loop thread
 * is not joined before engine shutdown (arq_shutdown() is not called on the
 * teardown path), so a msg_store_feed() can race the shutdown; keeping the
 * mutex alive for the process lifetime makes that race harmless — the feed
 * side locks, sees enabled == false, and returns without touching the ring. */
static pthread_mutex_t g_store_lock = PTHREAD_MUTEX_INITIALIZER;

static void msg_store_lock(void)   { pthread_mutex_lock(&g_store_lock); }
static void msg_store_unlock(void) { pthread_mutex_unlock(&g_store_lock); }

/* ------------------------------------------------------------------ */
/*  Time                                                               */
/* ------------------------------------------------------------------ */

static void msg_store_now(int64_t *sec_out, int *ms_out)
{
#if defined(_WIN32)
    FILETIME ft;
    uint64_t t;
    GetSystemTimeAsFileTime(&ft);
    t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= 116444736000000000ULL; /* 1601 -> 1970 */
    *sec_out = (int64_t)(t / 10000000ULL);
    *ms_out  = (int)((t / 10000ULL) % 1000ULL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *sec_out = (int64_t)ts.tv_sec;
    *ms_out  = (int)(ts.tv_nsec / 1000000);
#endif
}

/* ------------------------------------------------------------------ */
/*  Path                                                               */
/* ------------------------------------------------------------------ */

static void msg_store_default_path(char *buf, size_t bufsz)
{
    const char *base = NULL;
#if defined(_WIN32)
    base = getenv("LOCALAPPDATA");
    if (!base || !base[0]) base = getenv("APPDATA");
    if (base && base[0])
        snprintf(buf, bufsz, "%s\\mercury\\messages.jsonl", base);
    else
        snprintf(buf, bufsz, "messages.jsonl");
#elif defined(__APPLE__)
    base = getenv("HOME");
    if (base && base[0])
        snprintf(buf, bufsz, "%s/Library/Application Support/mercury/messages.jsonl", base);
    else
        snprintf(buf, bufsz, "messages.jsonl");
#else
    base = getenv("XDG_STATE_HOME");
    if (base && base[0])
        snprintf(buf, bufsz, "%s/mercury/messages.jsonl", base);
    else if ((base = getenv("HOME")) && base[0])
        snprintf(buf, bufsz, "%s/.local/state/mercury/messages.jsonl", base);
    else
        snprintf(buf, bufsz, "messages.jsonl");
#endif
}

static int msg_store_mkdirs(char *path)
{
    char *p;

    for (p = path + 1; *p; p++)
    {
        if (*p == '/' || *p == '\\')
        {
            char saved = *p;
            *p = '\0';
#if defined(_WIN32)
            (void)_mkdir(path);
#else
            (void)mkdir(path, 0755);
#endif
            *p = saved;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  JSON escaping                                                      */
/* ------------------------------------------------------------------ */

static size_t json_escape(const char *s, size_t len, char *out, size_t out_cap)
{
    size_t o = 0;

    for (size_t i = 0; i < len && o + 8 < out_cap; i++)
    {
        unsigned char c = (unsigned char)s[i];
        switch (c)
        {
        case '"':  out[o++] = '\\'; out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        default:
            if (c < 0x20)
            {
                static const char hex[] = "0123456789abcdef";
                out[o++] = '\\'; out[o++] = 'u'; out[o++] = '0'; out[o++] = '0';
                out[o++] = hex[c >> 4];
                out[o++] = hex[c & 0x0F];
            }
            else
            {
                out[o++] = (char)c;
            }
            break;
        }
    }
    out[o] = '\0';
    return o;
}

/* ------------------------------------------------------------------ */
/*  Text filter                                                        */
/* ------------------------------------------------------------------ */

/* A line is stored only if it is printable text: tabs and bytes >= 0x20 (which
 * includes UTF-8 continuation bytes) are allowed, control bytes are not.  This
 * rejects binary file-transfer chunks and AX.25/Reticulum frames. */
static bool msg_store_is_text(const uint8_t *data, size_t len)
{
    if (len == 0)
        return false;
    for (size_t i = 0; i < len; i++)
    {
        uint8_t c = data[i];
        if (c < 0x20 && c != '\t' && c != '\r')
            return false;
        if (c == 0x7F)
            return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/*  Ring                                                               */
/* ------------------------------------------------------------------ */

static void ring_push(const char *line, size_t len)
{
    if (g_store.cap <= 0)
        return;

    size_t n = len;
    if (n >= MSG_MAX_LINE)
        n = MSG_MAX_LINE - 1;

    int idx;
    if (g_store.count < g_store.cap)
    {
        idx = (g_store.start + g_store.count) % g_store.cap;
        g_store.count++;
    }
    else
    {
        idx = g_store.start;
        g_store.start = (g_store.start + 1) % g_store.cap;
    }
    memcpy(g_store.lines[idx], line, n);
    g_store.lines[idx][n] = '\0';
}

/* ------------------------------------------------------------------ */
/*  Core                                                               */
/* ------------------------------------------------------------------ */

static void msg_store_emit(const char *plane, const char *dir, const char *peer,
                           const uint8_t *text, size_t text_len)
{
    char buf[MSG_MAX_LINE];
    char text_esc[MSG_MAX_LINE];
    char peer_esc[MSG_MAX_PEER * 2 + 8];
    int64_t sec;
    int ms;

    /* Trim one trailing newline (the chat client appends "\n"). */
    while (text_len > 0 && (text[text_len - 1] == '\n' || text[text_len - 1] == '\r'))
        text_len--;

    if (text_len == 0 || text_len > MSG_MAX_TEXT)
        return;
    if (!msg_store_is_text(text, text_len))
        return;

    (void)json_escape((const char *)text, text_len, text_esc, sizeof(text_esc));
    json_escape(peer ? peer : "", peer ? strlen(peer) : 0, peer_esc, sizeof(peer_esc));

    msg_store_now(&sec, &ms);

    int n = snprintf(buf, sizeof(buf),
                     "{\"ts\":%lld,\"ms\":%d,\"plane\":\"%s\",\"dir\":\"%s\","
                     "\"peer\":\"%s\",\"text\":\"%s\"}",
                     (long long)sec, ms, plane, dir, peer_esc, text_esc);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = (int)sizeof(buf) - 1;

    buf[n] = '\n';
    buf[n + 1] = '\0';

    ring_push(buf, (size_t)(n + 1));

    if (g_store.fp)
    {
        if (fwrite(buf, 1, (size_t)(n + 1), g_store.fp) != (size_t)(n + 1) ||
            fflush(g_store.fp) != 0)
        {
            HLOGW(MSG_STORE_LOG_TAG, "write failed: %s", strerror(errno));
        }
    }
}

void msg_store_feed(const char *plane, const char *dir, const char *peer,
                    const uint8_t *data, size_t len)
{
    plane_t p;
    dir_t d;

    if (!data || len == 0)
        return;

    if (strcmp(plane, MSG_PLANE_BCAST) == 0) p = PLANE_BCAST;
    else                                    p = PLANE_ARQ;
    if (strcmp(dir, MSG_DIR_TX) == 0) d = DIR_TX;
    else                              d = DIR_RX;

    msg_store_lock();
    if (!g_store.enabled)
    {
        msg_store_unlock();
        return;
    }

    for (size_t i = 0; i < len; i++)
    {
        uint8_t c = data[i];
        if (c == '\n')
        {
            if (!g_store.discarding[p][d])
            {
                msg_store_emit(plane, dir, peer,
                               (const uint8_t *)g_store.feed[p][d],
                               g_store.feed_len[p][d]);
            }
            g_store.discarding[p][d] = false;
            g_store.feed_len[p][d] = 0;
        }
        else if (g_store.discarding[p][d])
        {
            /* Over-long line: skip everything until the next newline. */
        }
        else if (g_store.feed_len[p][d] < FEED_BUF_SIZE - 1)
        {
            g_store.feed[p][d][g_store.feed_len[p][d]++] = (char)c;
        }
        else
        {
            /* Line too long to be a chat message — discard it whole. */
            g_store.discarding[p][d] = true;
            g_store.feed_len[p][d] = 0;
        }
    }
    msg_store_unlock();
}

void msg_store_append(const char *plane, const char *dir, const char *peer,
                      const char *text)
{
    if (!text)
        return;

    msg_store_lock();
    if (!g_store.enabled)
    {
        msg_store_unlock();
        return;
    }
    msg_store_emit(plane, dir, peer, (const uint8_t *)text, strlen(text));
    msg_store_unlock();
}

size_t msg_store_count(void)
{
    size_t n = 0;
    msg_store_lock();
    if (g_store.enabled)
        n = (size_t)g_store.count;
    msg_store_unlock();
    return n;
}

size_t msg_store_get(size_t index, char *buf, size_t buf_cap)
{
    size_t written = 0;

    if (!buf || buf_cap == 0)
        return 0;

    msg_store_lock();
    if (g_store.enabled && index < (size_t)g_store.count)
    {
        int idx = (g_store.start + (int)index) % g_store.cap;
        char *line = g_store.lines[idx];
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            len--;
        if (len >= buf_cap)
            len = buf_cap - 1;
        memcpy(buf, line, len);
        buf[len] = '\0';
        written = len;
    }
    msg_store_unlock();
    return written;
}

/* ------------------------------------------------------------------ */
/*  Init / shutdown                                                    */
/* ------------------------------------------------------------------ */

int msg_store_init(const char *path, int max_lines)
{
    char resolved[1024];

    if (g_store.enabled)
        msg_store_shutdown();

    memset(&g_store, 0, sizeof(g_store));

    if (max_lines <= 0)
        max_lines = MSG_STORE_DEFAULT_MAX_LINES;

    if (path && path[0])
        snprintf(resolved, sizeof(resolved), "%s", path);
    else
        msg_store_default_path(resolved, sizeof(resolved));

    msg_store_mkdirs(resolved);

    g_store.cap = max_lines;
    g_store.lines = calloc((size_t)max_lines, sizeof(*g_store.lines));
    if (!g_store.lines)
    {
        HLOGE(MSG_STORE_LOG_TAG, "out of memory allocating %d-entry ring", max_lines);
        memset(&g_store, 0, sizeof(g_store));
        return -1;
    }

    snprintf(g_store.path, sizeof(g_store.path), "%s", resolved);

    g_store.fp = fopen(resolved, "a+");
    if (!g_store.fp)
    {
        HLOGW(MSG_STORE_LOG_TAG, "cannot open %s for append: %s — messages "
              "will be kept in memory only", resolved, strerror(errno));
    }

    /* Load existing messages into the ring so they are available (and survive)
     * after a restart. */
    if (g_store.fp)
    {
        char line[MSG_MAX_LINE];
        rewind(g_store.fp);
        while (fgets(line, sizeof(line), g_store.fp))
        {
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n')
                len--;
            if (len == 0)
                continue;
            ring_push(line, len);
        }
        /* Reposition for appends (fgets may have hit EOF). */
        fseek(g_store.fp, 0, SEEK_END);
    }

    g_store.enabled = true;
    HLOGI(MSG_STORE_LOG_TAG, "initialised (%s, ring=%d, loaded=%d)",
          resolved, g_store.cap, g_store.count);
    return 0;
}

void msg_store_shutdown(void)
{
    msg_store_lock();
    if (!g_store.enabled)
    {
        msg_store_unlock();
        return;
    }

    if (g_store.fp)
    {
        fflush(g_store.fp);
        fclose(g_store.fp);
        g_store.fp = NULL;
    }

    free(g_store.lines);
    g_store.lines = NULL;
    g_store.cap = 0;
    g_store.start = 0;
    g_store.count = 0;
    g_store.enabled = false;
    msg_store_unlock();
}
