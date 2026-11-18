/*
  FILE....: H_IRA_3072_2048.h

  Static arrays for LDPC codec H_IRA_3072_2048: an irregular repeat-accumulate code,
  3072 bits, 2048 information bits, information columns of weight 3, placed
  by progressive edge growth (utils/ldpc_ira_peg.py 3072 2048 3 1).
*/

#define H_IRA_3072_2048_NUMBERPARITYBITS 1024
#define H_IRA_3072_2048_MAX_ROW_WEIGHT 7
#define H_IRA_3072_2048_CODELENGTH 3072
#define H_IRA_3072_2048_NUMBERROWSHCOLS 2048
#define H_IRA_3072_2048_MAX_COL_WEIGHT 3
#define H_IRA_3072_2048_DEC_TYPE 0
#define H_IRA_3072_2048_MAX_ITER 100

#include <stdint.h>

extern const uint16_t H_IRA_3072_2048_H_rows[];
extern const uint16_t H_IRA_3072_2048_H_cols[];
