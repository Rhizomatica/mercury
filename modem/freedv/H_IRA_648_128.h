/*
  FILE....: H_IRA_648_128.h

  Static arrays for LDPC codec H_IRA_648_128: an irregular repeat-accumulate code,
  648 bits, 128 information bits, information columns of weight 6, placed
  by progressive edge growth (utils/ldpc_ira_peg.py 648 128 6 1).
*/

#define H_IRA_648_128_NUMBERPARITYBITS 520
#define H_IRA_648_128_MAX_ROW_WEIGHT 3
#define H_IRA_648_128_CODELENGTH 648
#define H_IRA_648_128_NUMBERROWSHCOLS 128
#define H_IRA_648_128_MAX_COL_WEIGHT 6
#define H_IRA_648_128_DEC_TYPE 0
#define H_IRA_648_128_MAX_ITER 100

#include <stdint.h>

extern const uint16_t H_IRA_648_128_H_rows[];
extern const uint16_t H_IRA_648_128_H_cols[];
