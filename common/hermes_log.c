/* Mercury Modem — async logging implementation
 *
 * Copyright (C) 2025-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "hermes_log.h"

#ifdef _WIN32
/* Windows localtime_s has reversed argument order from POSIX localtime_r */
#define localtime_r(timep, result) localtime_s((result), (timep))
#endif

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HERMES_LOG_DEFAULT_CAPACITY  1024
#define HERMES_LOG_COMPONENT_MAX     32
#define HERMES_LOG_MESSAGE_MAX       480
#define HERMES_LOG_COMP_TABLE_MAX    16

/* Per-entry routing flags are stored with the entry so the worker thread
 * needs no further access to shared state when printing. */
typedef struct
{
    struct timespec    ts;           /* wall clock at enqueue */
    uint32_t           uptime_ms;    /* monotonic ms since hermes_log_init() */
    hermes_log_level_t level;
    bool               to_stderr;
    bool               to_file;
    bool               file_jsonl;
    FILE              *file_fp;      /* snapshot of file pointer at enqueue */
    char component[HERMES_LOG_COMPONENT_MAX];
    char message[HERMES_LOG_MESSAGE_MAX];
} hermes_log_entry_t;

typedef struct
{
    char               name[HERMES_LOG_COMPONENT_MAX];
    hermes_log_level_t level;
} hermes_log_comp_t;

typedef struct
{
    pthread_t       worker;
    pthread_mutex_t lock;
    pthread_cond_t  cond;

    hermes_log_entry_t *entries;
    size_t  capacity;
    size_t  head;
    size_t  tail;
    size_t  count;
    bool    running;
    bool    initialized;

    atomic_ulong dropped;
    atomic_int   min_level;
    atomic_int   effective_min; /* fast-path: min of all active thresholds */

    uint64_t startup_mono_ms;   /* CLOCK_MONOTONIC ms at hermes_log_init()  */

    FILE              *file_fp;           /* NULL if no file sink open       */
    FILE              *file_fp_to_close;  /* fp deferred until shutdown      */
    hermes_log_level_t file_min_level;
    bool               file_jsonl;

    hermes_log_comp_t  comp_table[HERMES_LOG_COMP_TABLE_MAX];
    int                comp_table_size;
} hermes_log_state_t;

static hermes_log_state_t g_log = {
    .lock          = PTHREAD_MUTEX_INITIALIZER,
    .cond          = PTHREAD_COND_INITIALIZER,
    .min_level     = HERMES_LOG_LEVEL_INFO,
    .effective_min = HERMES_LOG_LEVEL_INFO,
};

/* ---- internal helpers ---- */

static uint64_t mono_ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

static const char *level_name(hermes_log_level_t level)
{
    switch (level)
    {
    case HERMES_LOG_LEVEL_DEBUG:  return "DBG";
    case HERMES_LOG_LEVEL_TIMING: return "TMG";
    case HERMES_LOG_LEVEL_INFO:   return "INF";
    case HERMES_LOG_LEVEL_WARN:   return "WRN";
    case HERMES_LOG_LEVEL_ERROR:
    default:                      return "ERR";
    }
}

/* Must be called under g_log.lock. */
static void update_effective_min_locked(void)
{
    int m = atomic_load_explicit(&g_log.min_level, memory_order_relaxed);
    if (g_log.file_fp && (int)g_log.file_min_level < m)
        m = (int)g_log.file_min_level;
    for (int i = 0; i < g_log.comp_table_size; i++)
    {
        if ((int)g_log.comp_table[i].level < m)
            m = (int)g_log.comp_table[i].level;
    }
    atomic_store_explicit(&g_log.effective_min, m, memory_order_relaxed);
}

/* Must be called under g_log.lock. Returns effective stderr threshold. */
static int component_min_locked(const char *component)
{
    int global = atomic_load_explicit(&g_log.min_level, memory_order_relaxed);
    if (!component)
        return global;
    for (int i = 0; i < g_log.comp_table_size; i++)
    {
        if (strcmp(g_log.comp_table[i].name, component) == 0)
            return (int)g_log.comp_table[i].level;
    }
    return global;
}

/* ---- worker thread ---- */

static void write_entry_file(const hermes_log_entry_t *entry)
{
    FILE *fp = entry->file_fp;
    if (!fp)
        return;

    if (entry->file_jsonl)
    {
        uint64_t wall_ms = (uint64_t)entry->ts.tv_sec * 1000ULL +
                           (uint64_t)(entry->ts.tv_nsec / 1000000L);
        /* Minimal JSON string escaping: backslash and double-quote. */
        char esc[HERMES_LOG_MESSAGE_MAX * 2];
        const char *src = entry->message;
        char       *dst = esc;
        const char *end = esc + sizeof(esc) - 2;
        while (*src && dst < end)
        {
            if (*src == '"' || *src == '\\')
                *dst++ = '\\';
            *dst++ = *src++;
        }
        *dst = '\0';
        fprintf(fp,
                "{\"t\":%llu,\"up\":%u,\"lv\":\"%s\",\"c\":\"%s\",\"m\":\"%s\"}\n",
                (unsigned long long)wall_ms,
                entry->uptime_ms,
                level_name(entry->level),
                entry->component,
                esc);
    }
    else
    {
        struct tm tmv;
        char tsbuf[32];
        char upbuf[20];
        time_t sec = entry->ts.tv_sec;
        long   ms  = entry->ts.tv_nsec / 1000000L;
        localtime_r(&sec, &tmv);
        snprintf(tsbuf, sizeof(tsbuf), "%02d:%02d:%02d.%03ld",
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
        snprintf(upbuf, sizeof(upbuf), "[+%u.%03us]",
                 entry->uptime_ms / 1000, entry->uptime_ms % 1000);
        fprintf(fp, "%s %s [%s] [%s] %s\n",
                tsbuf, upbuf,
                level_name(entry->level), entry->component, entry->message);
    }
    fflush(fp);
}

static void print_entry(const hermes_log_entry_t *entry)
{
    if (!entry)
        return;

    if (entry->to_stderr)
    {
        struct tm tmv;
        char tsbuf[32];
        char upbuf[20];
        time_t sec = entry->ts.tv_sec;
        long   ms  = entry->ts.tv_nsec / 1000000L;
        localtime_r(&sec, &tmv);
        snprintf(tsbuf, sizeof(tsbuf), "%02d:%02d:%02d.%03ld",
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
        snprintf(upbuf, sizeof(upbuf), "[+%u.%03us]",
                 entry->uptime_ms / 1000, entry->uptime_ms % 1000);

        fprintf(stderr, "%s %s [%s] [%s] %s\n",
                tsbuf, upbuf,
                level_name(entry->level), entry->component, entry->message);

        unsigned long dropped =
            atomic_exchange_explicit(&g_log.dropped, 0, memory_order_relaxed);
        if (dropped > 0)
            fprintf(stderr, "%s %s [WRN] [log] dropped %lu messages\n",
                    tsbuf, upbuf, dropped);
    }

    if (entry->to_file)
        write_entry_file(entry);
}

static void *log_worker(void *arg)
{
    (void)arg;

    for (;;)
    {
        hermes_log_entry_t entry;
        bool have_entry = false;

        pthread_mutex_lock(&g_log.lock);
        while (g_log.running && g_log.count == 0)
            pthread_cond_wait(&g_log.cond, &g_log.lock);

        if (g_log.count > 0)
        {
            entry = g_log.entries[g_log.head];
            g_log.head = (g_log.head + 1) % g_log.capacity;
            g_log.count--;
            pthread_cond_signal(&g_log.cond);
            have_entry = true;
        }
        else if (!g_log.running)
        {
            pthread_mutex_unlock(&g_log.lock);
            break;
        }
        pthread_mutex_unlock(&g_log.lock);

        if (have_entry)
            print_entry(&entry);
    }

    return NULL;
}

/* ---- public API ---- */

int hermes_log_init(size_t capacity)
{
    int rc;

    pthread_mutex_lock(&g_log.lock);
    if (g_log.initialized)
    {
        pthread_mutex_unlock(&g_log.lock);
        return 0;
    }

    if (capacity == 0)
        capacity = HERMES_LOG_DEFAULT_CAPACITY;

    g_log.entries = (hermes_log_entry_t *)calloc(capacity, sizeof(*g_log.entries));
    if (!g_log.entries)
    {
        pthread_mutex_unlock(&g_log.lock);
        return -1;
    }

    g_log.capacity        = capacity;
    g_log.head            = 0;
    g_log.tail            = 0;
    g_log.count           = 0;
    g_log.running         = true;
    g_log.initialized     = true;
    g_log.startup_mono_ms = mono_ms_now();
    g_log.file_fp         = NULL;
    g_log.file_fp_to_close = NULL;
    g_log.file_min_level  = HERMES_LOG_LEVEL_DEBUG;
    g_log.file_jsonl      = false;
    g_log.comp_table_size = 0;
    atomic_store_explicit(&g_log.dropped, 0, memory_order_relaxed);
    atomic_store_explicit(&g_log.min_level, HERMES_LOG_LEVEL_INFO, memory_order_relaxed);
    update_effective_min_locked();
    pthread_mutex_unlock(&g_log.lock);

    rc = pthread_create(&g_log.worker, NULL, log_worker, NULL);
    if (rc != 0)
    {
        pthread_mutex_lock(&g_log.lock);
        g_log.running     = false;
        g_log.initialized = false;
        free(g_log.entries);
        g_log.entries  = NULL;
        g_log.capacity = 0;
        pthread_mutex_unlock(&g_log.lock);
        return -1;
    }

    return 0;
}

void hermes_log_shutdown(void)
{
    pthread_t worker;
    bool should_join = false;

    pthread_mutex_lock(&g_log.lock);
    if (!g_log.initialized)
    {
        pthread_mutex_unlock(&g_log.lock);
        return;
    }

    g_log.running = false;
    worker        = g_log.worker;
    should_join   = true;
    pthread_cond_broadcast(&g_log.cond);
    pthread_mutex_unlock(&g_log.lock);

    if (should_join)
        pthread_join(worker, NULL);

    /* Worker has exited — safe to close files and free memory. */
    pthread_mutex_lock(&g_log.lock);
    if (g_log.file_fp)
    {
        fclose(g_log.file_fp);
        g_log.file_fp = NULL;
    }
    if (g_log.file_fp_to_close)
    {
        fclose(g_log.file_fp_to_close);
        g_log.file_fp_to_close = NULL;
    }
    free(g_log.entries);
    g_log.entries     = NULL;
    g_log.capacity    = 0;
    g_log.count       = 0;
    g_log.head        = 0;
    g_log.tail        = 0;
    g_log.initialized = false;
    pthread_mutex_unlock(&g_log.lock);
}

void hermes_log_set_level(hermes_log_level_t level)
{
    pthread_mutex_lock(&g_log.lock);
    atomic_store_explicit(&g_log.min_level, (int)level, memory_order_relaxed);
    update_effective_min_locked();
    pthread_mutex_unlock(&g_log.lock);
}

void hermes_log_set_component_level(const char *component, hermes_log_level_t level)
{
    if (!component)
        return;

    pthread_mutex_lock(&g_log.lock);
    for (int i = 0; i < g_log.comp_table_size; i++)
    {
        if (strcmp(g_log.comp_table[i].name, component) == 0)
        {
            g_log.comp_table[i].level = level;
            update_effective_min_locked();
            pthread_mutex_unlock(&g_log.lock);
            return;
        }
    }
    if (g_log.comp_table_size < HERMES_LOG_COMP_TABLE_MAX)
    {
        snprintf(g_log.comp_table[g_log.comp_table_size].name,
                 HERMES_LOG_COMPONENT_MAX, "%s", component);
        g_log.comp_table[g_log.comp_table_size].level = level;
        g_log.comp_table_size++;
        update_effective_min_locked();
    }
    pthread_mutex_unlock(&g_log.lock);
}

int hermes_log_set_file(const char *path, hermes_log_level_t min_level, bool jsonl)
{
    if (!path)
        return -1;

    FILE *fp = fopen(path, "a");
    if (!fp)
    {
        /* The whole point of a file sink is to survive where nobody is
         * watching the terminal, so a silent failure here is the worst
         * outcome: the operator believes the run is being recorded and only
         * finds out when there is nothing to send.  Say it, with the path and
         * the reason, on the one stream that does not depend on the log file.
         * Keep the return value so callers can react if they ever need to. */
        fprintf(stderr, "hermes_log: cannot open log file '%s': %s; "
                        "file logging disabled\n", path, strerror(errno));
        return -1;
    }

    pthread_mutex_lock(&g_log.lock);
    /* Defer close of old file until shutdown (worker may still have queued entries). */
    if (g_log.file_fp)
        g_log.file_fp_to_close = g_log.file_fp;
    g_log.file_fp        = fp;
    g_log.file_min_level = min_level;
    g_log.file_jsonl     = jsonl;
    update_effective_min_locked();
    pthread_mutex_unlock(&g_log.lock);
    return 0;
}

void hermes_log_close_file(void)
{
    pthread_mutex_lock(&g_log.lock);
    if (g_log.file_fp)
    {
        /* Defer actual fclose() until shutdown so queued entries can drain. */
        g_log.file_fp_to_close = g_log.file_fp;
        g_log.file_fp = NULL;
    }
    update_effective_min_locked();
    pthread_mutex_unlock(&g_log.lock);
}

unsigned long hermes_log_dropped_count(void)
{
    return atomic_load_explicit(&g_log.dropped, memory_order_relaxed);
}

uint64_t hermes_uptime_ms(void)
{
    if (g_log.startup_mono_ms == 0)
        return 0;
    return mono_ms_now() - g_log.startup_mono_ms;
}

void hermes_logf(hermes_log_level_t level, const char *component, const char *fmt, ...)
{
    hermes_log_entry_t entry;
    va_list ap;

    if (!fmt)
        return;

    /* Fast path: skip if below every active threshold. */
    if ((int)level < atomic_load_explicit(&g_log.effective_min, memory_order_relaxed))
        return;

    clock_gettime(CLOCK_REALTIME, &entry.ts);
    entry.uptime_ms = (uint32_t)(mono_ms_now() - g_log.startup_mono_ms);
    entry.level     = level;
    snprintf(entry.component, sizeof(entry.component), "%s",
             component ? component : "core");

    va_start(ap, fmt);
    vsnprintf(entry.message, sizeof(entry.message), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_log.lock);
    if (!g_log.initialized || !g_log.running || !g_log.entries || g_log.capacity == 0)
    {
        pthread_mutex_unlock(&g_log.lock);
        return;
    }

    entry.to_stderr = ((int)level >= component_min_locked(component));
    entry.to_file   = (g_log.file_fp != NULL &&
                       (int)level >= (int)g_log.file_min_level);
    /* Snapshot file context into the entry so the worker needs no shared state. */
    entry.file_fp   = entry.to_file ? g_log.file_fp : NULL;
    entry.file_jsonl = g_log.file_jsonl;

    if (!entry.to_stderr && !entry.to_file)
    {
        pthread_mutex_unlock(&g_log.lock);
        return;
    }

    while (g_log.running && g_log.count >= g_log.capacity)
        pthread_cond_wait(&g_log.cond, &g_log.lock);

    if (!g_log.initialized || !g_log.running || !g_log.entries || g_log.capacity == 0)
    {
        pthread_mutex_unlock(&g_log.lock);
        return;
    }

    g_log.entries[g_log.tail] = entry;
    g_log.tail = (g_log.tail + 1) % g_log.capacity;
    g_log.count++;
    pthread_cond_signal(&g_log.cond);
    pthread_mutex_unlock(&g_log.lock);
}
