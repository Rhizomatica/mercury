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

#include "physical_layer/interpolator.h"

double interpolate_linear(double a,double a_x,double b,double b_x,double x)
{
	double return_val;

	return_val=a+(b-a)*(x-a_x)/(b_x-a_x);

	return return_val;
}

std::complex <double> interpolate_linear(std::complex <double> a,double a_x,std::complex <double> b,double b_x,double x)
{
	std::complex <double> return_val;

	return_val=a+(b-a)*(x-a_x)/(b_x-a_x);

	return return_val;
}

double interpolate_bilinear(double a,double a_x,double a_y,double b,double b_x,double b_y,double c,double c_x,double c_y,double d,double d_x,double d_y,double x,double y)
{
	double e,f,return_val;

	e=interpolate_linear(a,a_x,b,b_x,x);
	f=interpolate_linear(c,c_x,d,d_x,x);

	return_val=interpolate_linear(e,a_y,f,c_y,y);

	return return_val;
}


std::complex <double> interpolate_bilinear(std::complex <double> a,double a_x,double a_y,std::complex <double> b,double b_x,double b_y,std::complex <double> c,double c_x,double c_y,std::complex <double> d,double d_x,double d_y,double x,double y)

{
	std::complex <double> e,f,return_val;

	e=interpolate_linear(a,a_x,b,b_x,x);
	f=interpolate_linear(c,c_x,d,d_x,x);

	return_val=interpolate_linear(e,a_y,f,c_y,y);


	return return_val;
}

void interpolate_linear_col(st_channel_real* estimated_channel, int max_col, int max_row, int col)
{
	int loc_start,loc_end,nLocations;

	loc_start=0;
	loc_end=max_row-1;
	nLocations=max_row-1;

	while(nLocations>0)
	{
		for(int i=loc_start;i<max_row;i++)
		{
			if(estimated_channel[i*max_col+col].status==MEASURED)
			{
				loc_start=i;
				break;
			}
		}
		for(int i=loc_start+1;i<max_row;i++)
		{
			if(estimated_channel[i*max_col+col].status==MEASURED)
			{
				loc_end=i;
				break;
			}
		}
		nLocations=loc_end-loc_start;

		for(int i=loc_start+1;i<loc_end;i++)
		{
			estimated_channel[i*max_col+col].value=interpolate_linear(estimated_channel[loc_start*max_col+col].value,loc_start,estimated_channel[loc_end*max_col+col].value,loc_end,i);
			estimated_channel[i*max_col+col].status=INTERPOLATED;
		}
		loc_start=loc_end;
	}


	loc_start=0;
	loc_end=max_row-1;
	for(int i=0;i<max_row;i++)
	{
		if(estimated_channel[i*max_col+col].status==MEASURED)
		{
			loc_start=i;
			break;
		}
	}
	for(int i=loc_start+1;i<max_row;i++)
	{
		if(estimated_channel[i*max_col+col].status==MEASURED)
		{
			loc_end=i;
			break;
		}
	}
	if(loc_start!=0)
	{
		for(int i=0;i<loc_start;i++)
		{
			estimated_channel[i*max_col+col].value=interpolate_linear(estimated_channel[loc_start*max_col+col].value,loc_start,estimated_channel[loc_end*max_col+col].value,loc_end,i);
			estimated_channel[i*max_col+col].status=INTERPOLATED;
		}
	}


	loc_end=0;
	loc_start=max_row-1;
	for(int i=max_row-1;i>=0;i--)
	{
		if(estimated_channel[i*max_col+col].status==MEASURED)
		{
			loc_end=i;
			break;
		}
	}
	for(int i=loc_end-1;i>=0;i--)
	{
		if(estimated_channel[i*max_col+col].status==MEASURED)
		{
			loc_start=i;
			break;
		}
	}
	if(loc_end!=max_row-1)
	{
		for(int i=max_row-1;i>loc_end;i--)
		{
			estimated_channel[i*max_col+col].value=interpolate_linear(estimated_channel[loc_start*max_col+col].value,loc_start,estimated_channel[loc_end*max_col+col].value,loc_end,i);
			estimated_channel[i*max_col+col].status=INTERPOLATED;
		}
	}
}

void interpolate_linear_col(st_channel_complex* estimated_channel, int max_col, int max_row, int col)
{
	int loc_start,loc_end,nLocations;

	loc_start=0;
	loc_end=max_row-1;
	nLocations=max_row-1;

	while(nLocations>0)
	{
		for(int i=loc_start;i<max_row;i++)
		{
			if(estimated_channel[i*max_col+col].status==MEASURED)
			{
				loc_start=i;
				break;
			}
		}
		for(int i=loc_start+1;i<max_row;i++)
		{
			if(estimated_channel[i*max_col+col].status==MEASURED)
			{
				loc_end=i;
				break;
			}
		}
		nLocations=loc_end-loc_start;

		for(int i=loc_start+1;i<loc_end;i++)
		{
			estimated_channel[i*max_col+col].value=interpolate_linear(estimated_channel[loc_start*max_col+col].value,loc_start,estimated_channel[loc_end*max_col+col].value,loc_end,i);
			estimated_channel[i*max_col+col].status=INTERPOLATED;
		}
		loc_start=loc_end;
	}


	loc_start=0;
	loc_end=max_row-1;
	for(int i=0;i<max_row;i++)
	{
		if(estimated_channel[i*max_col+col].status==MEASURED)
		{
			loc_start=i;
			break;
		}
	}
	for(int i=loc_start+1;i<max_row;i++)
	{
		if(estimated_channel[i*max_col+col].status==MEASURED)
		{
			loc_end=i;
			break;
		}
	}
	if(loc_start!=0)
	{
		for(int i=0;i<loc_start;i++)
		{
			estimated_channel[i*max_col+col].value=interpolate_linear(estimated_channel[loc_start*max_col+col].value,loc_start,estimated_channel[loc_end*max_col+col].value,loc_end,i);
			estimated_channel[i*max_col+col].status=INTERPOLATED;
		}
	}


	loc_end=0;
	loc_start=max_row-1;
	for(int i=max_row-1;i>=0;i--)
	{
		if(estimated_channel[i*max_col+col].status==MEASURED)
		{
			loc_end=i;
			break;
		}
	}
	for(int i=loc_end-1;i>=0;i--)
	{
		if(estimated_channel[i*max_col+col].status==MEASURED)
		{
			loc_start=i;
			break;
		}
	}
	if(loc_end!=max_row-1)
	{
		for(int i=max_row-1;i>loc_end;i--)
		{
			estimated_channel[i*max_col+col].value=interpolate_linear(estimated_channel[loc_start*max_col+col].value,loc_start,estimated_channel[loc_end*max_col+col].value,loc_end,i);
			estimated_channel[i*max_col+col].status=INTERPOLATED;
		}
	}
}

void interpolate_bilinear_matrix(st_channel_real* estimated_channel, int max_col, int max_row, int col1,int col2, int row1, int row2)
{
	double a,b,c,d;
	double a_x,a_y,b_x,b_y,c_x,c_y,d_x,d_y;


	a=estimated_channel[row1*max_col+col1].value;
	b=estimated_channel[row1*max_col+col2].value;

	for(int i=col1+1;i<col2;i++)
	{
		estimated_channel[row1*max_col+i].value=interpolate_linear(a,col1,b,col2,i);
		estimated_channel[row1*max_col+i].status=INTERPOLATED;
	}

	for(int j=row1+1;j<row2;j++)
	{
		a=estimated_channel[(j-1)*max_col+col1].value;
		a_x=col1;
		a_y=j-1;
		b=estimated_channel[(j-1)*max_col+col2].value;
		b_x=col2;
		b_y=j-1;
		c=estimated_channel[(j+1)*max_col+col1].value;
		c_x=col1;
		c_y=j+1;
		d=estimated_channel[(j+1)*max_col+col2].value;
		d_x=col2;
		d_y=j+1;

		for(int i=col1+1;i<col2;i++)
		{
			estimated_channel[j*max_col+i].value=interpolate_bilinear(a,a_x,a_y,b,b_x,b_y,c,c_x,c_y,d,d_x,d_y,i,j);
			estimated_channel[j*max_col+i].status=INTERPOLATED;
		}

	}

	c=estimated_channel[row2*max_col+col1].value;
	d=estimated_channel[row2*max_col+col2].value;

	for(int i=col1+1;i<col2;i++)
	{
		estimated_channel[row2*max_col+i].value=interpolate_linear(c,col1,d,col2,i);
		estimated_channel[row2*max_col+i].status=INTERPOLATED;
	}

}

void interpolate_bilinear_matrix(st_channel_complex* estimated_channel, int max_col, int max_row, int col1,int col2, int row1, int row2)
{
	std::complex <double> a,b,c,d;
	double a_x,a_y,b_x,b_y,c_x,c_y,d_x,d_y;


	a=estimated_channel[row1*max_col+col1].value;
	b=estimated_channel[row1*max_col+col2].value;

	for(int i=col1+1;i<col2;i++)
	{
		estimated_channel[row1*max_col+i].value=interpolate_linear(a,col1,b,col2,i);
		estimated_channel[row1*max_col+i].status=INTERPOLATED;
	}

	for(int j=row1+1;j<row2;j++)
	{
		a=estimated_channel[(j-1)*max_col+col1].value;
		a_x=col1;
		a_y=j-1;
		b=estimated_channel[(j-1)*max_col+col2].value;
		b_x=col2;
		b_y=j-1;
		c=estimated_channel[(j+1)*max_col+col1].value;
		c_x=col1;
		c_y=j+1;
		d=estimated_channel[(j+1)*max_col+col2].value;
		d_x=col2;
		d_y=j+1;

		for(int i=col1+1;i<col2;i++)
		{
			estimated_channel[j*max_col+i].value=interpolate_bilinear(a,a_x,a_y,b,b_x,b_y,c,c_x,c_y,d,d_x,d_y,i,j);
			estimated_channel[j*max_col+i].status=INTERPOLATED;
		}

	}

	c=estimated_channel[row2*max_col+col1].value;
	d=estimated_channel[row2*max_col+col2].value;

	for(int i=col1+1;i<col2;i++)
	{
		estimated_channel[row2*max_col+i].value=interpolate_linear(c,col1,d,col2,i);
		estimated_channel[row2*max_col+i].status=INTERPOLATED;
	}

}
