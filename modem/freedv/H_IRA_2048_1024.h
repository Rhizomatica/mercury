/*
  FILE....: H_IRA_2048_1024.h

  Static arrays for LDPC codec H_IRA_2048_1024: an irregular repeat-accumulate code,
  2048 bits, 1024 information bits, information columns of weight 3, placed
  by progressive edge growth (utils/ldpc_ira_peg.py 2048 1024 3 1).
*/

#define H_IRA_2048_1024_NUMBERPARITYBITS 1024
#define H_IRA_2048_1024_MAX_ROW_WEIGHT 4
#define H_IRA_2048_1024_CODELENGTH 2048
#define H_IRA_2048_1024_NUMBERROWSHCOLS 1024
#define H_IRA_2048_1024_MAX_COL_WEIGHT 3
#define H_IRA_2048_1024_DEC_TYPE 0
#define H_IRA_2048_1024_MAX_ITER 100

#include <stdint.h>

extern const uint16_t H_IRA_2048_1024_H_rows[];
extern const uint16_t H_IRA_2048_1024_H_cols[];
