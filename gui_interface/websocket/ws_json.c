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

/* ---- JSON scanning ----
 *
 * This walks the object's structure rather than searching for the key as a
 * substring.  The substring version looked equivalent for the messages the UI
 * actually sends, and was not: it stopped a string value at the first quote
 * byte without honouring the backslash before it, so a value like
 *
 *     {"command":"set_ptt_config","value":"a \"b\" c"}
 *
 * arrived as `a \` -- any device path or callsign containing a quote was
 * silently truncated.  It also matched a key inside a nested object before
 * the real one at the top level.
 */

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Append one code point as UTF-8.  Silently drops what will not fit: the
 * caller's buffers are fixed-size and a truncated device name is better than
 * a refused command. */
static void emit_utf8(uint32_t cp, char *out, size_t out_sz, size_t *o)
{
    if (out == NULL)
        return;

    if (cp < 0x80)
    {
        if (*o + 1 < out_sz) out[(*o)++] = (char) cp;
    }
    else if (cp < 0x800)
    {
        if (*o + 2 < out_sz)
        {
            out[(*o)++] = (char) (0xC0 | (cp >> 6));
            out[(*o)++] = (char) (0x80 | (cp & 0x3F));
        }
    }
    else if (cp < 0x10000)
    {
        if (*o + 3 < out_sz)
        {
            out[(*o)++] = (char) (0xE0 | (cp >> 12));
            out[(*o)++] = (char) (0x80 | ((cp >> 6) & 0x3F));
            out[(*o)++] = (char) (0x80 | (cp & 0x3F));
        }
    }
    else
    {
        if (*o + 4 < out_sz)
        {
            out[(*o)++] = (char) (0xF0 | (cp >> 18));
            out[(*o)++] = (char) (0x80 | ((cp >> 12) & 0x3F));
            out[(*o)++] = (char) (0x80 | ((cp >> 6) & 0x3F));
            out[(*o)++] = (char) (0x80 | (cp & 0x3F));
        }
    }
}

/* Read a JSON string starting at the opening quote.  Decodes escapes into
 * `out` when given (NUL-terminated, truncated to fit).  Returns the byte
 * after the closing quote, or NULL if the string never closes. */
static const char *parse_string(const char *p, const char *end,
                                char *out, size_t out_sz)
{
    size_t o = 0;

    if (p >= end || *p != '"')
        return NULL;
    p++;

    while (p < end)
    {
        unsigned char c = (unsigned char) *p;

        if (c == '"')
        {
            if (out != NULL && out_sz > 0)
                out[o < out_sz ? o : out_sz - 1] = '\0';
            return p + 1;
        }

        if (c != '\\')
        {
            if (out != NULL && o + 1 < out_sz)
                out[o++] = (char) c;
            p++;
            continue;
        }

        /* Escape sequence. */
        p++;
        if (p >= end)
            return NULL;

        switch (*p)
        {
        case '"':  if (out && o + 1 < out_sz) out[o++] = '"';  p++; break;
        case '\\': if (out && o + 1 < out_sz) out[o++] = '\\'; p++; break;
        case '/':  if (out && o + 1 < out_sz) out[o++] = '/';  p++; break;
        case 'b':  if (out && o + 1 < out_sz) out[o++] = '\b'; p++; break;
        case 'f':  if (out && o + 1 < out_sz) out[o++] = '\f'; p++; break;
        case 'n':  if (out && o + 1 < out_sz) out[o++] = '\n'; p++; break;
        case 'r':  if (out && o + 1 < out_sz) out[o++] = '\r'; p++; break;
        case 't':  if (out && o + 1 < out_sz) out[o++] = '\t'; p++; break;
        case 'u':
        {
            uint32_t cp = 0;
            int i;

            p++;
            if (end - p < 4)
                return NULL;
            for (i = 0; i < 4; i++)
            {
                int h = hex_val(p[i]);

                if (h < 0)
                    return NULL;
                cp = (cp << 4) | (uint32_t) h;
            }
            p += 4;

            /* Surrogate pair: 😀 is one code point, not two. */
            if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 &&
                p[0] == '\\' && p[1] == 'u')
            {
                uint32_t lo = 0;
                int ok = 1;

                for (i = 0; i < 4; i++)
                {
                    int h = hex_val(p[2 + i]);

                    if (h < 0) { ok = 0; break; }
                    lo = (lo << 4) | (uint32_t) h;
                }
                if (ok && lo >= 0xDC00 && lo <= 0xDFFF)
                {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }

            /* A lone surrogate is not a code point; it would also make the
             * frame invalid UTF-8, which a strict client drops. */
            if (cp >= 0xD800 && cp <= 0xDFFF)
                cp = '?';

            emit_utf8(cp, out, out_sz, &o);
            break;
        }
        default:
            /* Unknown escape: keep the character itself. */
            if (out && o + 1 < out_sz) out[o++] = *p;
            p++;
            break;
        }
    }

    return NULL;                        /* unterminated */
}

/* Step over one value of any type, including nested objects and arrays. */
static const char *skip_value(const char *p, const char *end)
{
    int depth = 0;

    p = skip_ws(p, end);
    if (p >= end)
        return NULL;

    if (*p == '"')
        return parse_string(p, end, NULL, 0);

    if (*p == '{' || *p == '[')
    {
        while (p < end)
        {
            if (*p == '"')
            {
                p = parse_string(p, end, NULL, 0);
                if (p == NULL)
                    return NULL;
                continue;
            }
            if (*p == '{' || *p == '[')
                depth++;
            else if (*p == '}' || *p == ']')
            {
                depth--;
                if (depth == 0)
                    return p + 1;
            }
            p++;
        }
        return NULL;
    }

    /* Number, true, false, null. */
    while (p < end && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
        p++;
    return p;
}

int ws_json_find_key(const char *json, size_t json_len, const char *key,
                     char *out, size_t out_sz)
{
    const char *p;
    const char *end;

    if (json == NULL || key == NULL || out == NULL || out_sz == 0)
        return 0;

    out[0] = '\0';
    p = skip_ws(json, json + json_len);
    end = json + json_len;

    if (p >= end || *p != '{')
        return 0;
    p++;

    while (p < end)
    {
        char name[128];
        const char *after;

        p = skip_ws(p, end);
        if (p < end && *p == '}')
            return 0;
        if (p >= end || *p != '"')
            return 0;                   /* malformed */

        after = parse_string(p, end, name, sizeof(name));
        if (after == NULL)
            return 0;
        p = skip_ws(after, end);

        if (p >= end || *p != ':')
            return 0;
        p = skip_ws(p + 1, end);
        if (p >= end)
            return 0;

        if (strcmp(name, key) == 0)
        {
            if (*p == '"')
                return parse_string(p, end, out, out_sz) != NULL;

            /* Bare value: number, literal, or a nested structure. */
            {
                const char *vend = skip_value(p, end);
                size_t vlen;

                if (vend == NULL)
                    return 0;
                vlen = (size_t) (vend - p);
                if (vlen >= out_sz)
                    vlen = out_sz - 1;
                memcpy(out, p, vlen);
                out[vlen] = '\0';
                return 1;
            }
        }

        p = skip_value(p, end);
        if (p == NULL)
            return 0;

        p = skip_ws(p, end);
        if (p < end && *p == ',')
            p++;
        else if (p < end && *p == '}')
            return 0;
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

/* ---- UTF-8 ----
 *
 * A WebSocket TEXT frame must carry valid UTF-8 (RFC 6455 8.1), and a client
 * that receives anything else is required to fail the connection.  Checking
 * only the shape of the continuation bytes was not enough: overlong forms,
 * UTF-16 surrogates and code points above U+10FFFF all have well-formed
 * continuation bytes and all make the frame invalid.  The status frame is
 * built from soundcard and rig names we do not control, so this is the
 * difference between one odd device name and a UI that keeps dropping.
 */
void ws_json_sanitise_utf8(char *s, size_t len)
{
    size_t i = 0;

    if (s == NULL)
        return;

    while (i < len)
    {
        unsigned char c = (unsigned char) s[i];
        size_t seq;
        unsigned char lo, hi;           /* allowed range of the FIRST tail byte */
        size_t j;
        bool ok = true;

        if (c < 0x20) { s[i] = '?'; i++; continue; }    /* JSON control char */
        if (c <= 0x7F) { i++; continue; }               /* printable ASCII */

        if (c >= 0xC2 && c <= 0xDF)      { seq = 2; lo = 0x80; hi = 0xBF; }
        else if (c == 0xE0)              { seq = 3; lo = 0xA0; hi = 0xBF; }
        else if (c >= 0xE1 && c <= 0xEC) { seq = 3; lo = 0x80; hi = 0xBF; }
        else if (c == 0xED)              { seq = 3; lo = 0x80; hi = 0x9F; }
        else if (c >= 0xEE && c <= 0xEF) { seq = 3; lo = 0x80; hi = 0xBF; }
        else if (c == 0xF0)              { seq = 4; lo = 0x90; hi = 0xBF; }
        else if (c >= 0xF1 && c <= 0xF3) { seq = 4; lo = 0x80; hi = 0xBF; }
        else if (c == 0xF4)              { seq = 4; lo = 0x80; hi = 0x8F; }
        else { s[i] = '?'; i++; continue; }   /* C0/C1 overlong, F5+, or a
                                               * stray continuation byte */

        if (i + seq > len) { s[i] = '?'; i++; continue; }   /* truncated */

        {
            unsigned char t = (unsigned char) s[i + 1];

            if (t < lo || t > hi)
                ok = false;
        }

        for (j = 2; ok && j < seq; j++)
        {
            unsigned char t = (unsigned char) s[i + j];

            if (t < 0x80 || t > 0xBF)
                ok = false;
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
