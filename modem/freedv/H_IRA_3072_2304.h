/*
  FILE....: H_IRA_3072_2304.h

  Static arrays for LDPC codec H_IRA_3072_2304: an irregular repeat-accumulate code,
  3072 bits, 2304 information bits, information columns of weight 3, placed
  by progressive edge growth (utils/ldpc_ira_peg.py 3072 2304 3 1).
*/

#define H_IRA_3072_2304_NUMBERPARITYBITS 768
#define H_IRA_3072_2304_MAX_ROW_WEIGHT 10
#define H_IRA_3072_2304_CODELENGTH 3072
#define H_IRA_3072_2304_NUMBERROWSHCOLS 2304
#define H_IRA_3072_2304_MAX_COL_WEIGHT 3
#define H_IRA_3072_2304_DEC_TYPE 0
#define H_IRA_3072_2304_MAX_ITER 100

#include <stdint.h>

extern const uint16_t H_IRA_3072_2304_H_rows[];
extern const uint16_t H_IRA_3072_2304_H_cols[];
