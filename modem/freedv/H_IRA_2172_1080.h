/*
  FILE....: H_IRA_2172_1080.h

  Static arrays for LDPC codec H_IRA_2172_1080: an irregular repeat-accumulate code,
  2172 bits, 1080 information bits, information columns of weight 3, placed
  by progressive edge growth (utils/ldpc_ira_peg.py 2172 1080 3 1).
*/

#define H_IRA_2172_1080_NUMBERPARITYBITS 1092
#define H_IRA_2172_1080_MAX_ROW_WEIGHT 4
#define H_IRA_2172_1080_CODELENGTH 2172
#define H_IRA_2172_1080_NUMBERROWSHCOLS 1080
#define H_IRA_2172_1080_MAX_COL_WEIGHT 3
#define H_IRA_2172_1080_DEC_TYPE 0
#define H_IRA_2172_1080_MAX_ITER 100

#include <stdint.h>

extern const uint16_t H_IRA_2172_1080_H_rows[];
extern const uint16_t H_IRA_2172_1080_H_cols[];
