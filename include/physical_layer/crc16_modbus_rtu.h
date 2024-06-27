/*
 * Mercury: A configurable open-source software-defined modem.
 * Copyright (C) 2022-2024 Fadi Jerji
 * Author: Fadi Jerji
 * Email: fadi.jerji@  <gmail.com, caisresearch.com, ieee.org>
 * ORCID: 0000-0002-2076-5831
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */


#ifndef INC_CRC16_MODBUS_RTU_H_
#define INC_CRC16_MODBUS_RTU_H_

#include <cmath>

#define POLY_CRC16_MODBUS_RTU 0xA001 // POLY = (constant)calculation polynomial of the CRC 16 = 1010 0000 0000 0001 (Generating polynomial = 1 + x2 + x 15 + x 16)

int CRC16_MODBUS_RTU_calc(int* data_byte, int nItems);


#endif
