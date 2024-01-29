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

#ifndef INC_FIFO_BUFFER_H_
#define INC_FIFO_BUFFER_H_

#include <unistd.h>

#define ERROR -1
#define SUCCESSFUL 0

class cl_fifo_buffer
{

public:
	cl_fifo_buffer();
  ~cl_fifo_buffer();

  void flush();
  int set_size(int size);
  int get_free_size();
  int get_size();


  int push(char* data, int length);
  int pop(char* data, int length);

private:
  char* data;
  int size;
  int read_location;
  int write_location;


};


#endif
