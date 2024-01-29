/*
 * Mercury: A configurable open-source software-defined modem.
 * Copyright (C) 2022-2024 Fadi Jerji
 * Author: Fadi Jerji
 * Email: fadi.jerji@  <gmail.com, rhizomatica.org, caisresearch.com, ieee.org>
 * ORCID: 0000-0002-2076-5831
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef INC_PHYSICAL_LAYER_INTERPOLATOR_H_
#define INC_PHYSICAL_LAYER_INTERPOLATOR_H_

#include "physical_defines.h"

double interpolate_linear(double a,double a_x,double b,double b_x,double x);
std::complex <double> interpolate_linear(std::complex <double> a,double a_x,std::complex <double> b,double b_x,double x);

double interpolate_bilinear(double a,double a_x,double a_y,double b,double b_x,double b_y,double c,double c_x,double c_y,double d,double d_x,double d_y,double x,double y);
std::complex <double> interpolate_bilinear(std::complex <double> a,double a_x,double a_y,std::complex <double> b,double b_x,double b_y,std::complex <double> c,double c_x,double c_y,std::complex <double> d,double d_x,double d_y,double x,double y);

void interpolate_linear_col(st_channel_real* estimated_channel, int max_col, int max_row, int col);
void interpolate_linear_col(st_channel_complex* estimated_channel, int max_col, int max_row, int col);

void interpolate_bilinear_matrix(st_channel_real* estimated_channel, int max_col, int max_row, int col1,int col2, int row1, int row2);
void interpolate_bilinear_matrix(st_channel_complex* estimated_channel, int max_col, int max_row, int col1,int col2, int row1, int row2);



#endif
