/*
 * Mercury: A configurable open-source software-defined modem.
 * Copyright (C) 2022-2024 Fadi Jerji
 * Author: Fadi Jerji
 * Email: fadi.jerji@  <gmail.com, caisresearch.com, ieee.org>
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

#ifndef INC_MISC_H_
#define INC_MISC_H_

#include <complex>
#include <cmath>

#ifndef M_PI
#define M_PI          3.14159265358979323846  /* pi */
#endif

void shift_left(double* matrix, int size, int nShift);
double get_angle(std::complex <double> value);
double get_amplitude(std::complex <double> value);
std::complex <double> set_complex(double amplitude, double theta);
void matrix_multiplication(std::complex <double>* a, int a_width, int a_hight, std::complex <double>* b, int b_width, int b_hight, std::complex <double>* c);

void byte_to_bit(int* data_byte, int* data_bit, int nBytes);
void bit_to_byte(int* data_bit, int* data_byte, int nBits);

#endif
