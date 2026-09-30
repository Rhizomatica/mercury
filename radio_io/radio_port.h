/* Does `radio_device` match the kind of port the chosen rig actually speaks?
 *
 * Copyright (C) 2026 Rhizomatica
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * ---------------------------------------------------------------------------
 *
 * Not every rig Hamlib supports is reached over a serial cable.  A FlexRadio
 * (SmartSDR, model 23005), rigctld (2), FLRig (4) and friends are network rigs:
 * their `rig_pathname` is an address, and Hamlib appends its own default port
 * if none is given.  Point one of those at a COM port and Hamlib dutifully
 * builds "COM4:4992", tries to resolve it as a host, and fails with the rather
 * unhelpful "Invalid parameter" -- reported as issue #179.
 *
 * The rig itself knows which it is (`caps->port_type`), so we can say so before
 * the user has to guess.  These checks only ever produce a diagnostic: the
 * device string is matched by shape, and a heuristic must never be allowed to
 * refuse a configuration that would in fact have worked.  Hamlib still gets to
 * make the final call.
 *
 * Kept free of Hamlib types so the shapes can be unit-tested on any host,
 * including builds without Hamlib at all.
 */

#ifndef RADIO_PORT_H
#define RADIO_PORT_H

#include <stddef.h>

typedef enum
{
    RADIO_PORT_KIND_OTHER = 0,  /* dummy, USB, parallel, ... -- no opinion */
    RADIO_PORT_KIND_SERIAL,
    RADIO_PORT_KIND_NETWORK,
} radio_port_kind_t;

typedef enum
{
    RADIO_PORT_OK = 0,
    RADIO_PORT_WANTS_NETWORK,   /* network rig, but radio_device names a tty  */
    RADIO_PORT_WANTS_SERIAL,    /* serial rig, but radio_device looks like an
                                   address */
} radio_port_verdict_t;

/* "COM4", "com12", "\\.\COM12", "/dev/ttyUSB0", "/dev/cu.usbserial-1234" */
static inline int radio_port_path_is_serial(const char *path)
{
    if (!path || !path[0])
        return 0;

    if (path[0] == '/')                       /* /dev/... (POSIX)            */
        return 1;

    if (path[0] == '\\')                      /* \\.\COMnn (Windows, >COM9)  */
        return 1;

    if ((path[0] == 'C' || path[0] == 'c') &&
        (path[1] == 'O' || path[1] == 'o') &&
        (path[2] == 'M' || path[2] == 'm') &&
        path[3] >= '0' && path[3] <= '9')
    {
        for (const char *p = path + 4; *p; p++)
            if (*p < '0' || *p > '9')
                return 0;                     /* "COM4x" -- not a COM port   */
        return 1;
    }

    return 0;
}

/* Judge a configured device string against the rig's port kind.  An empty
 * device is always OK: Hamlib has its own defaults and the user may well be
 * relying on them. */
static inline radio_port_verdict_t radio_port_check(radio_port_kind_t kind,
                                                    const char *path)
{
    if (!path || !path[0])
        return RADIO_PORT_OK;

    int looks_serial = radio_port_path_is_serial(path);

    if (kind == RADIO_PORT_KIND_NETWORK && looks_serial)
        return RADIO_PORT_WANTS_NETWORK;

    if (kind == RADIO_PORT_KIND_SERIAL && !looks_serial)
        return RADIO_PORT_WANTS_SERIAL;

    return RADIO_PORT_OK;
}

/* One line the user can act on, or NULL when there is nothing to say. */
static inline const char *radio_port_advice(radio_port_verdict_t v)
{
    switch (v)
    {
    case RADIO_PORT_WANTS_NETWORK:
        return "this rig is reached over the NETWORK, not a serial port: set "
               "ptt.device to the radio's address (e.g. 192.168.1.50, or "
               "192.168.1.50:4992 to override the port). "
               "ptt.hamlib_serial_speed does not apply.";
    case RADIO_PORT_WANTS_SERIAL:
        return "this rig is reached over a SERIAL port: set ptt.device to a "
               "port name (e.g. COM4 on Windows, /dev/ttyUSB0 on Linux).";
    case RADIO_PORT_OK:
    default:
        return NULL;
    }
}

/* ---- hamlib_conf pass-through ------------------------------------------------
 * "key=value[,key=value...]" -- pairs separated by commas, semicolons or
 * whitespace -- as rigctl --set-conf takes them.  Kept here, free of Hamlib
 * types, so the parsing is unit-tested on any host. */
typedef void (*radio_conf_pair_cb)(const char *key, const char *value, void *ctx);

#define RADIO_CONF_MAX 256

/* Calls cb for every pair and returns how many there were, or -1 (calling cb
 * for none) if an item has no '=' or an empty key, or the string is too long:
 * a half-applied configuration is worse than none. */
static inline int radio_conf_pairs(const char *s, radio_conf_pair_cb cb, void *ctx)
{
    char buf[RADIO_CONF_MAX];
    int n = 0;
    if (!s) return 0;
    size_t len = 0;
    while (s[len]) len++;
    if (len >= sizeof(buf)) return -1;
    for (int pass = 0; pass < 2; pass++) {
        size_t i = 0;
        for (size_t k = 0; k <= len; k++) buf[k] = s[k];
        n = 0;
        while (buf[i]) {
            while (buf[i] == ',' || buf[i] == ';' || buf[i] == ' ' || buf[i] == '\t') i++;
            if (!buf[i]) break;
            size_t start = i, eq = 0;
            while (buf[i] && buf[i] != ',' && buf[i] != ';' && buf[i] != ' ' && buf[i] != '\t') {
                if (buf[i] == '=' && !eq) eq = i;
                i++;
            }
            if (!eq || eq == start) return -1;
            char sep = buf[i];
            buf[i] = '\0';
            buf[eq] = '\0';
            if (pass == 1 && cb) cb(buf + start, buf + eq + 1, ctx);
            n++;
            if (sep) i++;
        }
    }
    return n;
}

static inline int radio_conf_ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

typedef struct { const char *key; int found; } radio_conf_find_t;
static inline void radio_conf_find_cb(const char *k, const char *v, void *ctx)
{
    (void)v;
    radio_conf_find_t *f = (radio_conf_find_t *)ctx;
    if (radio_conf_ieq(k, f->key)) f->found = 1;
}

/* Does the pass-through name this key (case-insensitively)? */
static inline int radio_conf_sets(const char *s, const char *key)
{
    radio_conf_find_t f = { key, 0 };
    return radio_conf_pairs(s, radio_conf_find_cb, &f) > 0 && f.found;
}

#endif /* RADIO_PORT_H */
