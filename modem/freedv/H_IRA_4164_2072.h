/*
  FILE....: H_IRA_4164_2072.h

  Static arrays for LDPC codec H_IRA_4164_2072: an irregular repeat-accumulate code,
  4164 bits, 2072 information bits, information columns of weight 3, placed
  by progressive edge growth (utils/ldpc_ira_peg.py 4164 2072 3 1).
*/

#define H_IRA_4164_2072_NUMBERPARITYBITS 2092
#define H_IRA_4164_2072_MAX_ROW_WEIGHT 4
#define H_IRA_4164_2072_CODELENGTH 4164
#define H_IRA_4164_2072_NUMBERROWSHCOLS 2072
#define H_IRA_4164_2072_MAX_COL_WEIGHT 3
#define H_IRA_4164_2072_DEC_TYPE 0
#define H_IRA_4164_2072_MAX_ITER 100

#include <stdint.h>

extern const uint16_t H_IRA_4164_2072_H_rows[];
extern const uint16_t H_IRA_4164_2072_H_cols[];
