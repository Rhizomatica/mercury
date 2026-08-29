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

#endif /* RADIO_PORT_H */
