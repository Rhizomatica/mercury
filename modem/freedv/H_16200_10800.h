/*
  FILE....: H_16200_10800.h

  Static arrays for LDPC codec H_16200_10800: the DVB-S2 short-frame code of rate
  2/3 (ETSI EN 302 307, 16200 bits, 10800 information bits), read off
  GNU Radio's DVB-S2 LDPC encoder by utils/ldpc_dvbs2_gen.py.
*/

#define H_16200_10800_NUMBERPARITYBITS 5400
#define H_16200_10800_MAX_ROW_WEIGHT 8
#define H_16200_10800_CODELENGTH 16200
#define H_16200_10800_NUMBERROWSHCOLS 10800
#define H_16200_10800_MAX_COL_WEIGHT 13
#define H_16200_10800_DEC_TYPE 0
#define H_16200_10800_MAX_ITER 100

#include <stdint.h>

extern const uint16_t H_16200_10800_H_rows[];
extern const uint16_t H_16200_10800_H_cols[];
