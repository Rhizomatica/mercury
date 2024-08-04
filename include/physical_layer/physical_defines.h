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

#ifndef INC_PHYSICAL_DEFINES_H_
#define INC_PHYSICAL_DEFINES_H_

#include <complex>
#include "common/common_defines.h"

#define ALSA_MAX_PATH 128

#define N_MAX 1600
#define C_WIDTH_MAX 200
#define V_WIDTH_MAX 50
#define D_WIDTH_MAX 2400


#define IRA 0


#define MERCURY 0

#define GBF 0
#define SPA 1


#define NOT_HEALTHY -1
#define UNKNOWN 0
#define HEALTHY 1


#define DATA 0
#define PILOT 1
#define CONFIG 2
#define ZERO 3
#define PREAMBLE 4
#define COPY_FIRST_COL 5
#define AUTO_SELLECT -1

#define DBPSK 0

#define UNKNOWN 0
#define MEASURED 1
#define INTERPOLATED 2

#define INTERPOLATION 0
#define DECIMATION 1

#define ZERO_FORCE 0
#define LEAST_SQUARE 1

#define NO_OUTER_CODE 0
#define CRC16_MODBUS_RTU 1

#define HIGH_DENSITY 0
#define LOW_DENSITY 1



struct st_carrier
{
	std::complex <double> value;
	int type;

};

struct st_channel_complex
{
	std::complex <double> value;
	int status;

};

struct st_channel_real
{
	double value;
	int status;

};

struct st_power_measurment
{
	double avg;
	double max;
	double papr_db;
};





#endif
