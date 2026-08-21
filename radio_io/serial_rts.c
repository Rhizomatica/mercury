/* HERMES Modem - Cross-platform serial RTS PTT backend
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * DigiRig Mobile and similar interfaces expose their hardware PTT switch on
 * the serial port's RTS modem-control line.  No bytes are exchanged and baud
 * rate is intentionally irrelevant.
 */

#include "serial_rts.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "../common/hermes_log.h"

#define RTS_LOG_TAG "serial-rts"

#ifdef _WIN32

#include <windows.h>

static HANDLE g_serial = INVALID_HANDLE_VALUE;

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

int serial_rts_open(const char *device_path)
{
    if (!device_path || !device_path[0])
    {
        HLOGE(RTS_LOG_TAG, "serial_rts requires a COM port");
        return -1;
    }

    char path[1024];
    windows_device_path(path, sizeof(path), device_path);
    g_serial = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_serial == INVALID_HANDLE_VALUE)
    {
        HLOGE(RTS_LOG_TAG, "Cannot open %s (Windows error %lu)",
              device_path, (unsigned long)GetLastError());
        return -1;
    }

    DCB dcb;
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(g_serial, &dcb))
    {
        HLOGE(RTS_LOG_TAG, "GetCommState(%s) failed (Windows error %lu)",
              device_path, (unsigned long)GetLastError());
        serial_rts_close();
        return -1;
    }

    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    if (!SetCommState(g_serial, &dcb))
    {
        HLOGE(RTS_LOG_TAG, "SetCommState(%s) failed (Windows error %lu)",
              device_path, (unsigned long)GetLastError());
        serial_rts_close();
        return -1;
    }

    if (!EscapeCommFunction(g_serial, CLRRTS))
    {
        HLOGE(RTS_LOG_TAG, "Could not clear RTS on %s (Windows error %lu)",
              device_path, (unsigned long)GetLastError());
        serial_rts_close();
        return -1;
    }

    HLOGI(RTS_LOG_TAG, "Serial RTS PTT ready on %s", device_path);
    return 0;
}

int serial_rts_set(bool on)
{
    if (g_serial == INVALID_HANDLE_VALUE)
        return -1;
    if (!EscapeCommFunction(g_serial, on ? SETRTS : CLRRTS))
    {
        HLOGE(RTS_LOG_TAG, "Could not %s RTS (Windows error %lu)",
              on ? "assert" : "clear", (unsigned long)GetLastError());
        return -1;
    }
    return 0;
}

void serial_rts_close(void)
{
    if (g_serial == INVALID_HANDLE_VALUE)
        return;
    (void)EscapeCommFunction(g_serial, CLRRTS);
    CloseHandle(g_serial);
    g_serial = INVALID_HANDLE_VALUE;
}

#else

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static int g_serial_fd = -1;

int serial_rts_open(const char *device_path)
{
    if (!device_path || !device_path[0])
    {
        HLOGE(RTS_LOG_TAG, "serial_rts requires a serial device path");
        return -1;
    }

    g_serial_fd = open(device_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (g_serial_fd < 0)
    {
        HLOGE(RTS_LOG_TAG, "Cannot open %s: %s", device_path, strerror(errno));
        return -1;
    }

    struct termios tio;
    if (tcgetattr(g_serial_fd, &tio) == 0)
    {
#ifdef CRTSCTS
        tio.c_cflag &= ~CRTSCTS;
#endif
        if (tcsetattr(g_serial_fd, TCSANOW, &tio) != 0)
        {
            HLOGE(RTS_LOG_TAG, "Cannot disable hardware flow control on %s: %s",
                  device_path, strerror(errno));
            serial_rts_close();
            return -1;
        }
    }
    else
    {
        HLOGE(RTS_LOG_TAG, "Cannot read serial settings for %s: %s",
              device_path, strerror(errno));
        serial_rts_close();
        return -1;
    }

    if (serial_rts_set(false) != 0)
    {
        serial_rts_close();
        return -1;
    }

    HLOGI(RTS_LOG_TAG, "Serial RTS PTT ready on %s", device_path);
    return 0;
}

int serial_rts_set(bool on)
{
    if (g_serial_fd < 0)
        return -1;

    int modem_bit = TIOCM_RTS;
    if (ioctl(g_serial_fd, on ? TIOCMBIS : TIOCMBIC, &modem_bit) != 0)
    {
        HLOGE(RTS_LOG_TAG, "Could not %s RTS: %s",
              on ? "assert" : "clear", strerror(errno));
        return -1;
    }
    return 0;
}

void serial_rts_close(void)
{
    if (g_serial_fd < 0)
        return;
    (void)serial_rts_set(false);
    close(g_serial_fd);
    g_serial_fd = -1;
}

#endif
