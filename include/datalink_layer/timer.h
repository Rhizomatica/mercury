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

#ifndef INC_TIMER_H_
#define INC_TIMER_H_

#include <ctime>
#include <iostream>
#include "common/common_defines.h"

#define COUNTING YES
#define NOT_COUNTING NO

class cl_timer
{
private:
	struct timespec startTime, stopTime;
public:
	cl_timer();
	~cl_timer();
	void reset();
	void start();
	void _continue();
	void stop();
	void update();
	void print();
	int get_elapsed_time_ms();
	int get_counter_status();
	int seconds;
	int miliSeconds;
	int microseconds;
	long int nanoseconds;
	int counting;


};


#endif
