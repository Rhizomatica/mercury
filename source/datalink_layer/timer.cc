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

#include "datalink_layer/timer.h"


cl_timer::cl_timer()
{
	seconds=0;
	miliSeconds=0;
	microseconds=0;
	nanoseconds=0;
	counting=NO;
}

cl_timer::~cl_timer()
{

}

void cl_timer::reset()
{
	seconds=0;
	miliSeconds=0;
	microseconds=0;
	nanoseconds=0;
}

void cl_timer::start()
{
	this->reset();
	clock_gettime(CLOCK_MONOTONIC_RAW, &startTime);
	counting=YES;

}

void cl_timer::stop()
{
	clock_gettime(CLOCK_MONOTONIC_RAW, &stopTime);
	seconds=stopTime.tv_sec-startTime.tv_sec;
	nanoseconds=stopTime.tv_nsec-startTime.tv_nsec;
	if(nanoseconds<0)
	{
		nanoseconds= 1000000000+nanoseconds;
		seconds--;
	}
	miliSeconds=nanoseconds/1000000;
	microseconds=nanoseconds/1000-miliSeconds*1000;
	nanoseconds=nanoseconds-miliSeconds*1000000-microseconds*1000;
	counting=NO;
}

void cl_timer::_continue()
{

	clock_gettime(CLOCK_MONOTONIC_RAW, &stopTime);
	seconds=stopTime.tv_sec-startTime.tv_sec;
	nanoseconds=stopTime.tv_nsec-startTime.tv_nsec;
	if(nanoseconds<0)
	{
		nanoseconds= 1000000000+nanoseconds;
		seconds--;
	}
	miliSeconds=nanoseconds/1000000;
	microseconds=nanoseconds/1000-miliSeconds*1000;
	nanoseconds=nanoseconds-miliSeconds*1000000-microseconds*1000;
	counting=YES;
}

void cl_timer::update()
{
	if(counting==YES)
	{
		clock_gettime(CLOCK_MONOTONIC_RAW, &stopTime);
		seconds=stopTime.tv_sec-startTime.tv_sec;
		nanoseconds=stopTime.tv_nsec-startTime.tv_nsec;
		if(nanoseconds<0)
		{
			nanoseconds= 1000000000+nanoseconds;
			seconds--;
		}
		miliSeconds=nanoseconds/1000000;
		microseconds=nanoseconds/1000-miliSeconds*1000;
		nanoseconds=nanoseconds-miliSeconds*1000000-microseconds*1000;
	}
}

int cl_timer::get_elapsed_time_ms()
{
	this->update();
	return this->seconds*1000+miliSeconds;
}

int cl_timer::get_counter_status()
{
	return this->counting;
}

void cl_timer::print()
{
	std::cout<< seconds << "s and "<<miliSeconds<<" ms and "<<microseconds<<" us and "<<nanoseconds<<" ns";
}
