/*
  FILE....: H_16200_11880.h

  Static arrays for LDPC codec H_16200_11880: the DVB-S2 short-frame code of rate
  3/4 (ETSI EN 302 307, 16200 bits, 11880 information bits), read off
  GNU Radio's DVB-S2 LDPC encoder by utils/ldpc_dvbs2_gen.py.
*/

#define H_16200_11880_NUMBERPARITYBITS 4320
#define H_16200_11880_MAX_ROW_WEIGHT 11
#define H_16200_11880_CODELENGTH 16200
#define H_16200_11880_NUMBERROWSHCOLS 11880
#define H_16200_11880_MAX_COL_WEIGHT 12
#define H_16200_11880_DEC_TYPE 0
#define H_16200_11880_MAX_ITER 100

#include <stdint.h>

extern const uint16_t H_16200_11880_H_rows[];
extern const uint16_t H_16200_11880_H_cols[];
