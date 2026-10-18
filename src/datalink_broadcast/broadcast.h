/* HERMES Modem
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef BROADCAST_H_
#define BROADCAST_H_

#include <stdint.h>
#include <stdbool.h>
#include "../modem/modem.h"

/* Function declarations */

/**
 * Run the broadcast subsystem.
 *
 * @param freedv Pointer to the modem structure.
 */
void broadcast_run(generic_modem_t *g_modem);


#include "bcast_status.h"

#endif // BROADCAST_H_
