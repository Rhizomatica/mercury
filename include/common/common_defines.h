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

#ifndef INC_COMMON_DEFINES_H_
#define INC_COMMON_DEFINES_H_


#define BER_PLOT_baseband 0
#define BER_PLOT_passband 1
#define TX_TEST 2
#define RX_TEST 3
#define ARQ_MODE 4
#define TX_BROADCAST 5
#define RX_BROADCAST 6

#define NUMBER_OF_CONFIGS 17
#define CONFIG_NONE -1
#define CONFIG_0 0
#define CONFIG_1 1
#define CONFIG_2 2
#define CONFIG_3 3
#define CONFIG_4 4
#define CONFIG_5 5
#define CONFIG_6 6
#define CONFIG_7 7
#define CONFIG_8 8
#define CONFIG_9 9
#define CONFIG_10 10
#define CONFIG_11 11
#define CONFIG_12 12
#define CONFIG_13 13
#define CONFIG_14 14
#define CONFIG_15 15
#define CONFIG_16 16

/*
 * Config	CODE	Mode	EsN0(FER<0,1)
0	BPSK 	1/16	BPSK 1/16	-10
1	BPSK 	2/16	BPSK 2/16	-7,5
2	BPSK 	3/16	BPSK 3/16	-6
3	BPSK 	4/16	BPSK 4/16	-4,5
4	BPSK 	5/16	BPSK 5/16	-3,5
5	BPSK 	6/16	BPSK 6/16	-2,5
6	BPSK 	8/16	BPSK 8/16	-1,5
7	QPSK 	5/16	QPSK 5/16	-0,5
8	QPSK 	6/16	QPSK 6/16	0,5
9	QPSK 	8/16	QPSK 8/16	1,5
10	8PSK 	6/16	8PSK 6/16	3
11	8PSK 	8/16	8PSK 8/16	4
12	QPSK 	14/16	QPSK 14/16	6,5
13	16QAM 	8/16	16QAM 8/16	7,5
14	8PSK 	14/16	8PSK 14/16	9
15	16QAM 	14/16	16QAM 14/16	12,5
16	32QAM 	14/16	32QAM 14/16	13,5


HIGH_DENSITY PILOTS

CONFIG_0 (71.3 bps).
CONFIG_1 (156.1 bps).
CONFIG_2 (241.0 bps).
CONFIG_3 (325.8 bps).
CONFIG_4 (410.6 bps).
CONFIG_5 (495.5 bps).
CONFIG_6 (665.2 bps).
CONFIG_7 (762.6 bps).
CONFIG_8 (920.2 bps).
CONFIG_9 (1235.3 bps).
CONFIG_10 (1353.7 bps).
CONFIG_11 (1818.1 bps).
CONFIG_12 (2261.4 bps).
CONFIG_13 (2470.6 bps).
CONFIG_14 (3389.7 bps).
CONFIG_15 (4361.3 bps).
CONFIG_16 (5664.7 bps).


LOW DENSITY PILOTS

CONFIG_0 (84.2 bps).
CONFIG_1 (184.5 bps).
CONFIG_2 (284.8 bps).
CONFIG_3 (385.0 bps).
CONFIG_4 (485.3 bps).
CONFIG_5 (585.6 bps).
CONFIG_6 (786.1 bps).
CONFIG_7 (889.7 bps).
CONFIG_8 (1073.5 bps).
CONFIG_9 (1441.2 bps).
CONFIG_10 (1353.7 bps).
CONFIG_11 (1818.1 bps).
CONFIG_12 (2654.7 bps).
CONFIG_13 (2882.4 bps).
CONFIG_14 (3389.7 bps).
CONFIG_15 (5088.2 bps).
CONFIG_16 (5664.7 bps).


 *
 */


#define FIRST_MESSAGE 0
#define MIDDLE_MESSAGE 1
#define FLUSH_MESSAGE 2
#define SINGLE_MESSAGE 3
#define NO_FILTER_MESSAGE 4

#define YES 1
#define NO 0



#endif
