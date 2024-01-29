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

#include "physical_layer/plot.h"


cl_plot::cl_plot()
{
	gnuplotPipe=NULL;
	plot_active=NO;
	folder="/mnt/ramDisk/";
	//	system("mount -t tmpfs -o size=512m tmpfs /mnt/ramDisk");

}
cl_plot::~cl_plot(){

}


void cl_plot::open(std::string main_title)
{
	if(plot_active==YES)
	{
		gnuplotPipe = popen ("gnuplot-x11 -persist", "w");
		fprintf(gnuplotPipe, "set terminal x11\n");
		std::string title1="set title \""+main_title+"\"\n";
		fprintf(gnuplotPipe, "%s", title1.c_str());
	}
}

void cl_plot::close()
{
	if(plot_active==YES)
	{
		pclose(gnuplotPipe);
	}
}

void cl_plot::reset(std::string main_title)
{
	if(plot_active==YES)
	{
		fprintf(gnuplotPipe, "reset\n");
		std::string title1="set title \""+main_title+"\"\n";
		fprintf(gnuplotPipe, "%s", title1.c_str());
	}
}

void cl_plot::plot(std::string curve,float* data, int nItems)
{

	if(plot_active==YES)
	{
		curve=folder+curve;
		FILE * data_file = fopen(curve.c_str(), "w");

		for(int i=0;i<nItems;i++)
		{
			fprintf(data_file,"%f %f  \n",*(data+i*2),*(data+i*2+1));
		}
		fclose(data_file);

		fprintf(gnuplotPipe, "set xlabel \"Es/N0\" offset 0 textcolor lt -1\n");
		fprintf(gnuplotPipe, "set ylabel \"BER\" offset 0 rotate by -270 textcolor lt -1\n");
		fprintf(gnuplotPipe, "show xlabel \n");
		fprintf(gnuplotPipe, "show ylabel \n");
		fprintf(gnuplotPipe, "set format y \"%.0s*10^{%T}\"\n");


		fprintf(gnuplotPipe, "set logscale y 10\n");

		fprintf(gnuplotPipe, "set style func linespoints\n");

		std::string text="plot \'"+curve+"\' with linespoints ls 3 \n";
		fprintf(gnuplotPipe, "%s", text.c_str());
		fflush(gnuplotPipe);
	}

}

void cl_plot::plot(std::string curve1,float* data1, int nItems1,std::string curve2,float* data2, int nItems2)
{
	if(plot_active==YES)
	{
		fprintf(gnuplotPipe, "set xlabel \"Es/N0\" offset 0 textcolor lt -1\n");
		fprintf(gnuplotPipe, "set ylabel \"BER\" offset 0 rotate by -270 textcolor lt -1\n");
		fprintf(gnuplotPipe, "show xlabel \n");
		fprintf(gnuplotPipe, "show ylabel \n");
		fprintf(gnuplotPipe, "set format y \"%.0s*10^{%T}\"\n");


		fprintf(gnuplotPipe, "set logscale y 10\n");

		fprintf(gnuplotPipe, "set style func linespoints\n");

		curve1=folder+curve1;
		curve2=folder+curve2;
		FILE * data_file1 = fopen(curve1.c_str(), "w");
		FILE * data_file2 = fopen(curve2.c_str(), "w");

		for(int i=0;i<nItems1;i++)
		{
			fprintf(data_file1,"%f %f  \n",*(data1+i*2),*(data1+i*2+1));
		}
		fclose(data_file1);



		for(int i=0;i<nItems2;i++)
		{
			fprintf(data_file2,"%f %f  \n",*(data2+i*2),*(data2+i*2+1));
		}
		fclose(data_file2);



		std::string text="plot \'"+curve1+"\' with linespoints ls 3, \'"+curve2+"\' with linespoints ls 4 \n";
		fprintf(gnuplotPipe, "%s", text.c_str());

		fflush(gnuplotPipe);

	}

}

void cl_plot::plot_constellation(float* data, int nPoints)
{
	if(plot_active==YES)
	{
		FILE * constellation_data = fopen((folder+"constellation_data.temp").c_str(), "w");
		for (int i=0;i<nPoints;i++)
		{
			fprintf(constellation_data, "%f %f  \n", *(data+2*i),*(data+2*i+1));
		}
		fclose(constellation_data);

		fprintf(gnuplotPipe, "reset\n");
		fprintf(gnuplotPipe, "set terminal x11\n");
		fprintf(gnuplotPipe, "set title \"Constellation\"\n");
		fprintf(gnuplotPipe, "set xrange [-2:2]\n");
		fprintf(gnuplotPipe, "set yrange [-2:2]\n");
		fprintf(gnuplotPipe, "set xzeroaxis\n");
		fprintf(gnuplotPipe, "set yzeroaxis\n");
		fprintf(gnuplotPipe, ("plot '"+folder+"constellation_data.temp' using 1:2 pt 7 ps 1.0 \n").c_str());
		fflush(gnuplotPipe);
	}
}




