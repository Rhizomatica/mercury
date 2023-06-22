/*
 * Mercury: A configurable open-source software-defined modem.
 * Copyright (C) 2022 Fadi Jerji
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
#ifndef PLOT_H_
#define PLOT_H_

#include <string>
#include "defines.h"

class cl_plot
{
private:
	FILE * gnuplotPipe;


public:
	cl_plot();
	~cl_plot();
	void open(std::string main_title);
	void close();
	void reset(std::string main_title);
	void plot(std::string curve,float* data, int nItems);
	void plot(std::string curve1,float* data1, int nItems1,std::string curve2,float* data2, int nItems2);
	void plot_constellation(float* data, int nItems);
	std::string folder;
	int plot_active;


};





#endif
