/* The control plane's codec: the control mode, and the short PSK control
 * frame on receive (modem_ctl.c).
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MERCURY_MODEM_CTL_H
#define MERCURY_MODEM_CTL_H

#include "modem_backend.h"

extern const modem_backend_t modem_backend_ctl;

#endif
