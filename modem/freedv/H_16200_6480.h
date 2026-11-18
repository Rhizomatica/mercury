/*
  FILE....: H_16200_6480.h

  Static arrays for LDPC codec H_16200_6480: the DVB-S2 short-frame code of rate
  2/5 (ETSI EN 302 307, 16200 bits, 6480 information bits), read off
  GNU Radio's DVB-S2 LDPC encoder by utils/ldpc_dvbs2_gen.py.
*/

#define H_16200_6480_NUMBERPARITYBITS 9720
#define H_16200_6480_MAX_ROW_WEIGHT 4
#define H_16200_6480_CODELENGTH 16200
#define H_16200_6480_NUMBERROWSHCOLS 6480
#define H_16200_6480_MAX_COL_WEIGHT 12
#define H_16200_6480_DEC_TYPE 0
#define H_16200_6480_MAX_ITER 100

#include <stdint.h>

extern const uint16_t H_16200_6480_H_rows[];
extern const uint16_t H_16200_6480_H_cols[];
