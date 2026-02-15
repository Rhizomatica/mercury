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

#define VERSION__ "0.3.1-dev1"

// Verbose debug output (0=quiet, 1=debug prints enabled). Set via -v flag.
extern int g_verbose;

#define BER_PLOT_baseband 0
#define BER_PLOT_passband 1
#define TX_RAND 2
#define RX_RAND 3
#define TX_TEST 4
#define RX_TEST 5
#define TX_SHM 6
#define RX_SHM 7
#define ARQ_MODE 8

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

// ROBUST (MFSK) configurations - values 100+ to avoid collision with OFDM configs
#define NUMBER_OF_ROBUST_CONFIGS 3
#define ROBUST_0 100  // 32-MFSK, LDPC rate 1/16, ~14 bps (hailing mode)
#define ROBUST_1 101  // 16-MFSK x2, LDPC rate 1/16, ~22 bps
#define ROBUST_2 102  // 16-MFSK x2, LDPC rate 1/4,  ~87 bps

inline bool is_robust_config(int config) { return config >= 100 && config <= 102; }
inline bool is_ofdm_config(int config) { return config >= 0 && config <= 16; }

// Unified config ladder for gearshift (ROBUST → OFDM)
// Used when robust_enabled + gearshift: ROBUST_0 → ROBUST_1 → ROBUST_2 → CONFIG_0 → ... → CONFIG_15
// CONFIG_16 (32QAM rate 14/16, 1 preamble) excluded: poor channel estimation makes it
// strictly worse than CONFIG_15 at all tested SNRs (10k vs 19k B/min at +30 dB).
static const int FULL_CONFIG_LADDER[] = {
	ROBUST_0, ROBUST_1, ROBUST_2,
	CONFIG_0, CONFIG_1, CONFIG_2, CONFIG_3, CONFIG_4, CONFIG_5, CONFIG_6,
	CONFIG_7, CONFIG_8, CONFIG_9, CONFIG_10, CONFIG_11, CONFIG_12,
	CONFIG_13, CONFIG_14, CONFIG_15
};
static const int FULL_CONFIG_LADDER_SIZE = 19;

inline int config_ladder_index(int config) {
	for (int i = 0; i < FULL_CONFIG_LADDER_SIZE; i++) {
		if (FULL_CONFIG_LADDER[i] == config) return i;
	}
	return -1;
}

inline int config_ladder_up(int config, bool robust_enabled) {
	if (!robust_enabled) {
		// OFDM only: simple increment within CONFIG_0-16
		return (config < CONFIG_15) ? config + 1 : config;
	}
	int idx = config_ladder_index(config);
	if (idx >= 0 && idx < FULL_CONFIG_LADDER_SIZE - 1) return FULL_CONFIG_LADDER[idx + 1];
	return config;
}

inline int config_ladder_down(int config, bool robust_enabled) {
	if (!robust_enabled) {
		return (config > CONFIG_0) ? config - 1 : config;
	}
	int idx = config_ladder_index(config);
	if (idx > 0) return FULL_CONFIG_LADDER[idx - 1];
	return config;
}

inline int config_ladder_down_n(int config, int steps, bool robust_enabled) {
	if (!robust_enabled) {
		int target = config - steps;
		return (target > CONFIG_0) ? target : CONFIG_0;
	}
	int idx = config_ladder_index(config);
	idx -= steps;
	if (idx < 0) idx = 0;
	return FULL_CONFIG_LADDER[idx];
}

inline bool config_is_at_top(int config, bool robust_enabled) {
	if (!robust_enabled) return config == CONFIG_15;
	return config_ladder_index(config) == FULL_CONFIG_LADDER_SIZE - 1;
}

inline bool config_is_at_bottom(int config, bool robust_enabled) {
	if (!robust_enabled) return config == CONFIG_0;
	return config_ladder_index(config) == 0;
}

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


// messages definition
#define FIRST_MESSAGE 0
#define MIDDLE_MESSAGE 1
#define FLUSH_MESSAGE 2
#define SINGLE_MESSAGE 3
#define NO_FILTER_MESSAGE 4

// supported radios
#define RADIO_SBITX 0
#define RADIO_STOCKHF 1

// {TX,RX}_SHM shared memory interface
#define SHM_PAYLOAD_BUFFER_SIZE 131072
#define SHM_PAYLOAD_NAME "/mercury-comm"

// audio buffers shared memory interface
// 1536000 * 8
#define AUDIO_PAYLOAD_BUFFER_SIZE 12288000
#define AUDIO_CAPT_PAYLOAD_NAME "/audio-capt"
#define AUDIO_PLAY_PAYLOAD_NAME "/audio-play"


// Gear shifting modes
#define NO_GEAR_SHIFT 0
#define GEAR_SHIFT_ENABLED 1
// #define NO_GEAR_SHIFT_LADDER 2
// #define NO_GEAR_SHIFT_SNR 3

#define YES 1
#define NO 0

// Config-to-string conversion for GUI display
// Bitrates are approximate (low-density pilots, default config)
inline const char* config_to_string(int config) {
	switch (config) {
		case ROBUST_0: return "ROBUST 0 (32-MFSK, ~14 bps)";
		case ROBUST_1: return "ROBUST 1 (16-MFSK x2, ~22 bps)";
		case ROBUST_2: return "ROBUST 2 (16-MFSK x2, ~87 bps)";
		case CONFIG_0:  return "CONFIG 0 (BPSK 1/16, ~84 bps)";
		case CONFIG_1:  return "CONFIG 1 (BPSK 2/16, ~185 bps)";
		case CONFIG_2:  return "CONFIG 2 (BPSK 3/16, ~285 bps)";
		case CONFIG_3:  return "CONFIG 3 (BPSK 4/16, ~385 bps)";
		case CONFIG_4:  return "CONFIG 4 (BPSK 5/16, ~485 bps)";
		case CONFIG_5:  return "CONFIG 5 (BPSK 6/16, ~586 bps)";
		case CONFIG_6:  return "CONFIG 6 (BPSK 8/16, ~786 bps)";
		case CONFIG_7:  return "CONFIG 7 (QPSK 5/16, ~890 bps)";
		case CONFIG_8:  return "CONFIG 8 (QPSK 6/16, ~1074 bps)";
		case CONFIG_9:  return "CONFIG 9 (QPSK 8/16, ~1441 bps)";
		case CONFIG_10: return "CONFIG 10 (8PSK 6/16, ~1354 bps)";
		case CONFIG_11: return "CONFIG 11 (8PSK 8/16, ~1818 bps)";
		case CONFIG_12: return "CONFIG 12 (QPSK 14/16, ~2655 bps)";
		case CONFIG_13: return "CONFIG 13 (16QAM 8/16, ~2882 bps)";
		case CONFIG_14: return "CONFIG 14 (8PSK 14/16, ~3390 bps)";
		case CONFIG_15: return "CONFIG 15 (16QAM 14/16, ~5088 bps)";
		case CONFIG_16: return "CONFIG 16 (32QAM 14/16, ~5665 bps)";
		default: return "UNKNOWN";
	}
}

// Short config label for status bar
inline const char* config_to_short_string(int config) {
	switch (config) {
		case ROBUST_0: return "ROBUST 0";
		case ROBUST_1: return "ROBUST 1";
		case ROBUST_2: return "ROBUST 2";
		case CONFIG_0:  return "CFG 0 BPSK";
		case CONFIG_1:  return "CFG 1 BPSK";
		case CONFIG_2:  return "CFG 2 BPSK";
		case CONFIG_3:  return "CFG 3 BPSK";
		case CONFIG_4:  return "CFG 4 BPSK";
		case CONFIG_5:  return "CFG 5 BPSK";
		case CONFIG_6:  return "CFG 6 BPSK";
		case CONFIG_7:  return "CFG 7 QPSK";
		case CONFIG_8:  return "CFG 8 QPSK";
		case CONFIG_9:  return "CFG 9 QPSK";
		case CONFIG_10: return "CFG 10 8PSK";
		case CONFIG_11: return "CFG 11 8PSK";
		case CONFIG_12: return "CFG 12 QPSK";
		case CONFIG_13: return "CFG 13 16QAM";
		case CONFIG_14: return "CFG 14 8PSK";
		case CONFIG_15: return "CFG 15 16QAM";
		case CONFIG_16: return "CFG 16 32QAM";
		default: return "???";
	}
}

#endif // INC_COMMON_DEFINES_H_
