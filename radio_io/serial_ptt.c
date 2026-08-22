/* HERMES Modem - Serial modem-control-line PTT backend
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * DigiRig, the All-In-One Cable and most home-brew interfaces expose their
 * hardware PTT switch on a serial port's RTS and/or DTR modem-control line.
 * No bytes are exchanged and the baud rate is irrelevant -- only the line
 * states matter.
 *
 * Three things vary between cables and all three have to be configurable, or
 * the backend silently fits only the subset of hardware it was written for:
 *
 *   - WHICH line keys the radio.  RTS is the common case, DTR is not rare, and
 *     some interfaces (the AIOC among them) expect BOTH driven together.
 *   - WHETHER the line is inverted, i.e. the cable keys on the LOW state.
 *   - The IDLE state at open.  This is the one that bites: on an inverted
 *     cable, a port opened without explicitly parking the line keys the
 *     transmitter the instant Mercury starts, and stays keyed.  A stuck
 *     carrier is the worst failure this code can produce, so open() drives
 *     the lines to their un-keyed state before anything else.
 */

#include "serial_ptt.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "../common/hermes_log.h"

#define PTT_LOG_TAG "serial-ptt"

/* Which lines the configuration says we drive. */
static bool uses_rts(const ptt_config_t *c)
{
    return c->serial_line == PTT_LINE_RTS || c->serial_line == PTT_LINE_BOTH;
}

static bool uses_dtr(const ptt_config_t *c)
{
    return c->serial_line == PTT_LINE_DTR || c->serial_line == PTT_LINE_BOTH;
}

/* Cached copy of the line configuration, so set() does not need the global
 * radio config (and cannot disagree with what open() actually programmed). */
static ptt_config_t g_cfg;
static bool g_open = false;

#ifdef _WIN32

#include <windows.h>

static HANDLE g_serial = INVALID_HANDLE_VALUE;
static bool g_last_rts_high = false;
static bool g_last_dtr_high = false;

static void windows_device_path(char *out, size_t out_size, const char *device)
{
    /* CreateFile requires the \\.\ prefix for COM10 and above.  It also
     * accepts the prefixed spelling for COM1..COM9, so make all ordinary COM
     * names unambiguous while leaving an explicit device path untouched. */
    if (_strnicmp(device, "COM", 3) == 0 && strchr(device, '\\') == NULL)
        snprintf(out, out_size, "\\\\.\\%s", device);
    else
        snprintf(out, out_size, "%s", device);
}

/* Drive the configured lines.  keyed == true means "transmit"; inversion is
 * applied per line, so the caller never reasons about polarity. */
static int drive_lines(bool keyed)
{
    if (g_serial == INVALID_HANDLE_VALUE)
        return -1;

    if (uses_rts(&g_cfg))
    {
        bool high = g_cfg.serial_invert_rts ? !keyed : keyed;
        if (!EscapeCommFunction(g_serial, high ? SETRTS : CLRRTS))
        {
            HLOGE(PTT_LOG_TAG, "Could not drive RTS (Windows error %lu)",
                  (unsigned long)GetLastError());
            return -1;
        }
        g_last_rts_high = high;
    }
    if (uses_dtr(&g_cfg))
    {
        bool high = g_cfg.serial_invert_dtr ? !keyed : keyed;
        if (!EscapeCommFunction(g_serial, high ? SETDTR : CLRDTR))
        {
            HLOGE(PTT_LOG_TAG, "Could not drive DTR (Windows error %lu)",
                  (unsigned long)GetLastError());
            return -1;
        }
        g_last_dtr_high = high;
    }
    return 0;
}

int serial_ptt_open(const ptt_config_t *config)
{
    if (!config || !config->device[0])
    {
        HLOGE(PTT_LOG_TAG, "serial PTT requires a COM port");
        return -1;
    }

    g_cfg = *config;

    char path[PTT_DEVICE_PATH_MAX + 8];
    windows_device_path(path, sizeof(path), g_cfg.device);
    g_serial = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_serial == INVALID_HANDLE_VALUE)
    {
        HLOGE(PTT_LOG_TAG, "Cannot open %s (Windows error %lu)",
              g_cfg.device, (unsigned long)GetLastError());
        return -1;
    }

    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(g_serial, &dcb))
    {
        HLOGE(PTT_LOG_TAG, "GetCommState(%s) failed (Windows error %lu)",
              g_cfg.device, (unsigned long)GetLastError());
        serial_ptt_close();
        return -1;
    }

    /* Hand only the configured lines to us.  Preserve the other line's DCB
     * settings so, for example, an RTS-only PTT cannot disturb DTR when the
     * updated state is applied. */
    if (uses_rts(&g_cfg))
    {
        dcb.fOutxCtsFlow = FALSE;
        dcb.fRtsControl  = RTS_CONTROL_DISABLE;
    }
    if (uses_dtr(&g_cfg))
    {
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDtrControl  = DTR_CONTROL_DISABLE;
    }
    if (!SetCommState(g_serial, &dcb))
    {
        HLOGE(PTT_LOG_TAG, "SetCommState(%s) failed (Windows error %lu)",
              g_cfg.device, (unsigned long)GetLastError());
        serial_ptt_close();
        return -1;
    }

    g_open = true;
    if (drive_lines(false) != 0)   /* park un-keyed before anything else */
    {
        serial_ptt_close();
        return -1;
    }
    return 0;
}

int serial_ptt_set(bool on)
{
    if (!g_open)
        return -1;
    return drive_lines(on);
}

int serial_ptt_line_state(bool *rts_high, bool *dtr_high)
{
    /* Windows exposes the OUTPUT lines only indirectly; report what we last
     * drove rather than pretending to read the pins. */
    if (g_serial == INVALID_HANDLE_VALUE)
        return -1;
    if (rts_high) *rts_high = g_last_rts_high;
    if (dtr_high) *dtr_high = g_last_dtr_high;
    return 0;
}

void serial_ptt_close(void)
{
    if (g_serial == INVALID_HANDLE_VALUE)
        return;
    (void)drive_lines(false);
    CloseHandle(g_serial);
    g_serial = INVALID_HANDLE_VALUE;
    g_open = false;
}

#else

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static int g_serial_fd = -1;

static int drive_lines(bool keyed)
{
    if (g_serial_fd < 0)
        return -1;

    int flags;
    if (ioctl(g_serial_fd, TIOCMGET, &flags) < 0)
    {
        HLOGE(PTT_LOG_TAG, "TIOCMGET failed: %s", strerror(errno));
        return -1;
    }

    if (uses_rts(&g_cfg))
    {
        bool high = g_cfg.serial_invert_rts ? !keyed : keyed;
        if (high) flags |= TIOCM_RTS; else flags &= ~TIOCM_RTS;
    }
    if (uses_dtr(&g_cfg))
    {
        bool high = g_cfg.serial_invert_dtr ? !keyed : keyed;
        if (high) flags |= TIOCM_DTR; else flags &= ~TIOCM_DTR;
    }

    if (ioctl(g_serial_fd, TIOCMSET, &flags) < 0)
    {
        HLOGE(PTT_LOG_TAG, "TIOCMSET failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int serial_ptt_open(const ptt_config_t *config)
{
    if (!config || !config->device[0])
    {
        HLOGE(PTT_LOG_TAG, "serial PTT requires a serial device path");
        return -1;
    }

    g_cfg = *config;

    g_serial_fd = open(g_cfg.device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (g_serial_fd < 0)
    {
        HLOGE(PTT_LOG_TAG, "Cannot open %s: %s", g_cfg.device, strerror(errno));
        return -1;
    }

    struct termios tio;
    if (tcgetattr(g_serial_fd, &tio) == 0)
    {
        cfmakeraw(&tio);
#ifdef CRTSCTS
        tio.c_cflag &= ~CRTSCTS;   /* we own RTS, not the flow-control layer */
#endif
        (void)tcsetattr(g_serial_fd, TCSANOW, &tio);
    }

    g_open = true;
    if (drive_lines(false) != 0)   /* park un-keyed before anything else */
    {
        serial_ptt_close();
        return -1;
    }
    return 0;
}

int serial_ptt_set(bool on)
{
    if (!g_open)
        return -1;
    return drive_lines(on);
}

int serial_ptt_line_state(bool *rts_high, bool *dtr_high)
{
    if (g_serial_fd < 0)
        return -1;
    int flags;
    if (ioctl(g_serial_fd, TIOCMGET, &flags) < 0)
        return -1;
    if (rts_high) *rts_high = (flags & TIOCM_RTS) != 0;
    if (dtr_high) *dtr_high = (flags & TIOCM_DTR) != 0;
    return 0;
}

void serial_ptt_close(void)
{
    if (g_serial_fd < 0)
        return;
    (void)drive_lines(false);
    close(g_serial_fd);
    g_serial_fd = -1;
    g_open = false;
}

#endif
