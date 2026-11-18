/*
  FILE....: H_16200_7200.h

  Static arrays for LDPC codec H_16200_7200: the DVB-S2 short-frame code of rate
  1/2 (ETSI EN 302 307, 16200 bits, 7200 information bits), read off
  GNU Radio's DVB-S2 LDPC encoder by utils/ldpc_dvbs2_gen.py.
*/

#define H_16200_7200_NUMBERPARITYBITS 9000
#define H_16200_7200_MAX_ROW_WEIGHT 5
#define H_16200_7200_CODELENGTH 16200
#define H_16200_7200_NUMBERROWSHCOLS 7200
#define H_16200_7200_MAX_COL_WEIGHT 8
#define H_16200_7200_DEC_TYPE 0
#define H_16200_7200_MAX_ITER 100

#include <stdint.h>

extern const uint16_t H_16200_7200_H_rows[];
extern const uint16_t H_16200_7200_H_cols[];
