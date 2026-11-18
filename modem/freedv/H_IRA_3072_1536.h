/*
  FILE....: H_IRA_3072_1536.h

  Static arrays for LDPC codec H_IRA_3072_1536: an irregular repeat-accumulate code,
  3072 bits, 1536 information bits, information columns of weight 3, placed
  by progressive edge growth (utils/ldpc_ira_peg.py 3072 1536 3 1).
*/

#define H_IRA_3072_1536_NUMBERPARITYBITS 1536
#define H_IRA_3072_1536_MAX_ROW_WEIGHT 4
#define H_IRA_3072_1536_CODELENGTH 3072
#define H_IRA_3072_1536_NUMBERROWSHCOLS 1536
#define H_IRA_3072_1536_MAX_COL_WEIGHT 3
#define H_IRA_3072_1536_DEC_TYPE 0
#define H_IRA_3072_1536_MAX_ITER 100

#include <stdint.h>

extern const uint16_t H_IRA_3072_1536_H_rows[];
extern const uint16_t H_IRA_3072_1536_H_cols[];
