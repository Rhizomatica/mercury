/*
  FILE....: H_16200_12600.h

  Static arrays for LDPC codec H_16200_12600: the DVB-S2 short-frame code of rate
  4/5 (ETSI EN 302 307, 16200 bits, 12600 information bits), read off
  GNU Radio's DVB-S2 LDPC encoder by utils/ldpc_dvbs2_gen.py.
*/

#define H_16200_12600_NUMBERPARITYBITS 3600
#define H_16200_12600_MAX_ROW_WEIGHT 11
#define H_16200_12600_CODELENGTH 16200
#define H_16200_12600_NUMBERROWSHCOLS 12600
#define H_16200_12600_MAX_COL_WEIGHT 3
#define H_16200_12600_DEC_TYPE 0
#define H_16200_12600_MAX_ITER 100

#include <stdint.h>

extern const uint16_t H_16200_12600_H_rows[];
extern const uint16_t H_16200_12600_H_cols[];
