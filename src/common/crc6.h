/* Mercury Modem — CRC-6/CRC-5 computation
 *
 * Copyright (C) 2025-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */


#include <stdint.h>

#pragma once

uint16_t crc6_0X6F(uint16_t crc, const uint8_t *data, int data_len);
uint8_t  crc5_0X15(uint8_t  crc, const uint8_t *data, int data_len);
