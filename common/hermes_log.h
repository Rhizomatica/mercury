/* Mercury Modem — async logging interface
 *
 * Copyright (C) 2025-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HERMES_LOG_H_
#define HERMES_LOG_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    HERMES_LOG_LEVEL_DEBUG  = 0, /* verbose internals, verbose mode only        */
    HERMES_LOG_LEVEL_TIMING = 1, /* protocol timing events (TMG tag)            */
    HERMES_LOG_LEVEL_INFO   = 2, /* general status                              */
    HERMES_LOG_LEVEL_WARN   = 3,
    HERMES_LOG_LEVEL_ERROR  = 4
} hermes_log_level_t;

int  hermes_log_init(size_t capacity);
void hermes_log_shutdown(void);
void hermes_log_set_level(hermes_log_level_t level);
void hermes_log_set_component_level(const char *component, hermes_log_level_t level);

/**
 * @brief Add (or replace) the file sink, opened for appending.
 *
 * @param path      Log file path; relative paths are resolved against the
 *                  process's current working directory.
 * @param min_level Lowest level written to the file.
 * @param jsonl     Write JSONL records instead of the human-readable format.
 *
 * @return 0 on success; -1 if @p path is NULL or cannot be opened, in which
 *         case a diagnostic naming the path and the errno reason is written
 *         to stderr and file logging stays disabled.
 */
int  hermes_log_set_file(const char *path, hermes_log_level_t min_level, bool jsonl);
void hermes_log_close_file(void);
unsigned long hermes_log_dropped_count(void);
void hermes_logf(hermes_log_level_t level, const char *component, const char *fmt, ...);

/**
 * @brief Return milliseconds elapsed since hermes_log_init().
 *
 * Uses CLOCK_MONOTONIC; always increases, unaffected by wall-clock changes.
 * Returns 0 before hermes_log_init() is called.
 */
uint64_t hermes_uptime_ms(void);

#define HLOGD(component, fmt, ...) hermes_logf(HERMES_LOG_LEVEL_DEBUG,  component, fmt, ##__VA_ARGS__)
#define HLOGT(component, fmt, ...) hermes_logf(HERMES_LOG_LEVEL_TIMING, component, fmt, ##__VA_ARGS__)
#define HLOGI(component, fmt, ...) hermes_logf(HERMES_LOG_LEVEL_INFO,   component, fmt, ##__VA_ARGS__)
#define HLOGW(component, fmt, ...) hermes_logf(HERMES_LOG_LEVEL_WARN,   component, fmt, ##__VA_ARGS__)
#define HLOGE(component, fmt, ...) hermes_logf(HERMES_LOG_LEVEL_ERROR,  component, fmt, ##__VA_ARGS__)

#endif /* HERMES_LOG_H_ */
