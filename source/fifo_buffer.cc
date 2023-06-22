/*
 * Mercury: A configurable open-source software-defined modem.
 * Copyright (C) 2023 Fadi Jerji
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

#include "fifo_buffer.h"




cl_fifo_buffer::cl_fifo_buffer()
{
	data=NULL;
	size=0;
	read_location=0;
	write_location=0;
}

cl_fifo_buffer::~cl_fifo_buffer()
{
	if(data!=NULL)
	{
		delete[] data;
	}
}

void cl_fifo_buffer::flush()
{
	read_location=0;
	write_location=0;
}

int cl_fifo_buffer::set_size(int size)
{
	int success=ERROR;
	data= new char[size];
	if(data!=NULL)
	{
		this->size=size;
		for(int i=0;i<this->size;i++)
		{
			this->data[i]=0;
		}
		success=SUCCESSFUL;
	}
	return success;
}

int cl_fifo_buffer::get_size()
{
	return this->size-1;
}

int cl_fifo_buffer::get_free_size()
{
	int free_size=0;
	if(write_location>=read_location)
	{
		free_size= this->size-(write_location-read_location);
	}
	else
	{
		free_size= read_location-write_location;
	}
	return free_size-1;
}

int cl_fifo_buffer::push(char* data, int length)
{
	int pushed_data=0;
	if(length<=get_free_size()&& length>0)
	{
		for(int i= 0;i<length;i++)
		{
			this->data[write_location]=data[i];
			write_location++;
			if(write_location==size)
			{
				write_location=0;
			}
		}
		pushed_data=length;
	}
	return pushed_data;
}

int cl_fifo_buffer::pop(char* data, int length)
{
	int popped_data=0;
	if(length>0 && get_free_size()!=get_size())
	{
		popped_data=length;
		for(int i= 0;i<length;i++)
		{
			data[i]=this->data[read_location];
			this->data[read_location]=0;
			read_location++;
			if(read_location==write_location)
			{
				popped_data=i+1;
				break;
			}
			if(read_location==size)
			{
				read_location=0;
			}
		}
	}
	return popped_data;
}
