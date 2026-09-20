/* Flat-JSON helpers for the Mercury UI command protocol
 *
 * Copyright (C) 2026 Rhizomatica
 * Authors: Pedro Messetti <pedromessetti.rhizomatica@gmail.com>
 *          Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ws_json.h"

/* Local memmem so this unit needs nothing but libc: it is linked into the
 * fuzz target as well as into mercury. */
static const void *json_memmem(const void *haystack, size_t haystack_len,
                               const void *needle, size_t needle_len)
{
    const unsigned char *h = (const unsigned char *) haystack;
    const unsigned char *n = (const unsigned char *) needle;
    size_t i;

    if (needle_len == 0)
        return haystack;
    if (haystack_len < needle_len)
        return NULL;

    for (i = 0; i + needle_len <= haystack_len; i++)
    {
        if (h[i] == n[0] && memcmp(h + i, n, needle_len) == 0)
            return h + i;
    }

    return NULL;
}

int ws_json_find_key(const char *json, size_t json_len, const char *key,
                     char *out, size_t out_sz)
{
    char needle[128];
    int nlen;
    const char *p;
    const char *end;

    if (json == NULL || key == NULL || out == NULL || out_sz == 0)
        return 0;

    nlen = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (nlen <= 0 || (size_t) nlen >= sizeof(needle))
        return 0;

    p = json;
    end = json + json_len;

    while (p < end)
    {
        const char *found = (const char *) json_memmem(p, (size_t) (end - p),
                                                       needle, (size_t) nlen);
        const char *vp;

        if (found == NULL)
            return 0;

        /* Skip the key, then whitespace and the colon. */
        vp = found + nlen;
        while (vp < end && (*vp == ' ' || *vp == '\t' || *vp == ':'))
            vp++;

        if (vp >= end)
            return 0;

        if (*vp == '"')
        {
            const char *ve;
            size_t vlen;

            vp++;
            ve = vp;
            while (ve < end && *ve != '"')
                ve++;

            vlen = (size_t) (ve - vp);
            if (vlen >= out_sz)
                vlen = out_sz - 1;
            memcpy(out, vp, vlen);
            out[vlen] = '\0';
            return 1;
        }
        else
        {
            /* Bare value: number, true, false, null. */
            const char *ve = vp;
            size_t vlen;

            while (ve < end && *ve != ',' && *ve != '}' && *ve != ' ' &&
                   *ve != '\n')
                ve++;

            vlen = (size_t) (ve - vp);
            if (vlen >= out_sz)
                vlen = out_sz - 1;
            memcpy(out, vp, vlen);
            out[vlen] = '\0';
            return 1;
        }
    }

    return 0;
}

int ws_json_parse_command(const char *json, size_t len, ws_command_t *cmd)
{
    if (cmd == NULL)
        return -1;

    memset(cmd, 0, sizeof(*cmd));

    if (!ws_json_find_key(json, len, "command", cmd->command,
                          sizeof(cmd->command)))
        return -1;                      /* mandatory field missing */

    ws_json_find_key(json, len, "value",  cmd->value,  sizeof(cmd->value));
    ws_json_find_key(json, len, "value2", cmd->value2, sizeof(cmd->value2));
    ws_json_find_key(json, len, "value3", cmd->value3, sizeof(cmd->value3));
    ws_json_find_key(json, len, "value4", cmd->value4, sizeof(cmd->value4));
    ws_json_find_key(json, len, "value5", cmd->value5, sizeof(cmd->value5));
    ws_json_find_key(json, len, "value6", cmd->value6, sizeof(cmd->value6));
    ws_json_find_key(json, len, "value7", cmd->value7, sizeof(cmd->value7));

    return 0;
}

void ws_json_sanitise_utf8(char *s, size_t len)
{
    size_t i = 0;

    if (s == NULL)
        return;

    while (i < len)
    {
        unsigned char c = (unsigned char) s[i];
        size_t seq;
        size_t j;
        bool ok;

        if (c < 0x20) { s[i] = '?'; i++; continue; }    /* JSON control char */
        if (c <= 0x7F) { i++; continue; }               /* printable ASCII */
        else if ((c & 0xE0) == 0xC0) seq = 2;           /* 110xxxxx */
        else if ((c & 0xF0) == 0xE0) seq = 3;           /* 1110xxxx */
        else if ((c & 0xF8) == 0xF0) seq = 4;           /* 11110xxx */
        else { s[i] = '?'; i++; continue; }             /* bad lead byte */

        if (i + seq > len) { s[i] = '?'; i++; continue; }   /* truncated */

        ok = true;
        for (j = 1; j < seq; j++)
        {
            if (((unsigned char) s[i + j] & 0xC0) != 0x80) { ok = false; break; }
        }

        if (ok)
            i += seq;
        else
        {
            s[i] = '?';
            i++;
        }
    }
}
