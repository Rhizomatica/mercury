/* FreeDV modem backend — codec2/FreeDV behind the modem_backend_t vtable.
 *
 * A thin 1:1 adapter over the freedv_api: the instance context is the
 * struct freedv *.  This is backend #1; behaviour is identical to the former
 * direct freedv_* calls in modem.c.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MERCURY_MODEM_FREEDV_H
#define MERCURY_MODEM_FREEDV_H

#include "modem_backend.h"

extern const modem_backend_t modem_backend_freedv;

#endif /* MERCURY_MODEM_FREEDV_H */
