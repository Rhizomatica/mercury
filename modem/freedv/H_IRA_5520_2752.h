/*
  FILE....: H_IRA_5520_2752.h

  Static arrays for LDPC codec H_IRA_5520_2752: an irregular repeat-accumulate code,
  5520 bits, 2752 information bits, information columns of weight 3, placed
  by progressive edge growth (utils/ldpc_ira_peg.py 5520 2752 3 1).
*/

#define H_IRA_5520_2752_NUMBERPARITYBITS 2768
#define H_IRA_5520_2752_MAX_ROW_WEIGHT 4
#define H_IRA_5520_2752_CODELENGTH 5520
#define H_IRA_5520_2752_NUMBERROWSHCOLS 2752
#define H_IRA_5520_2752_MAX_COL_WEIGHT 3
#define H_IRA_5520_2752_DEC_TYPE 0
#define H_IRA_5520_2752_MAX_ITER 100

#include <stdint.h>

extern const uint16_t H_IRA_5520_2752_H_rows[];
extern const uint16_t H_IRA_5520_2752_H_cols[];
