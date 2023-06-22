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

#include <iostream>
#include <complex>
#include <fstream>
#include <math.h>
#include <iostream>
#include <complex>
#include "telecom_system.h"
#include "arq.h"



int main(int argc, char *argv[])
{
	srand(time(0));

	cl_telecom_system telecom_system;
	cl_arq_controller ARQ;

	telecom_system.operation_mode=ARQ_MODE;
	telecom_system.load_configuration();
	ARQ.telecom_system=&telecom_system;

	if(telecom_system.operation_mode==ARQ_MODE)
	{
		ARQ.init();
		ARQ.print_stats();
		while(1)
		{
			ARQ.process_main();
		}
	}
	else if(telecom_system.operation_mode==RX_TEST)
	{
		telecom_system.plot.open("PLOT");
		telecom_system.plot.reset("PLOT");

		while(1)
		{
			telecom_system.RX_TEST_process_main();
		}
		telecom_system.plot.close();
	}
	else if(telecom_system.operation_mode==RX_TCP)
	{
		if(telecom_system.tcp_socket_test.init()==SUCCESS)
		{
			while(1)
			{
				telecom_system.RX_TCP_process_main();
			}
		}
	}
	else if (telecom_system.operation_mode==TX_TEST)
	{
		while(1)
		{
			telecom_system.TX_TEST_process_main();
		}
	}
	else if (telecom_system.operation_mode==TX_TCP)
	{
		if(telecom_system.tcp_socket_test.init()==SUCCESS)
		{
			while(1)
			{
				telecom_system.TX_TCP_process_main();
			}
		}
	}
	else if (telecom_system.operation_mode==BER_PLOT_baseband)
	{
		telecom_system.BER_PLOT_baseband_process_main();
	}
	else if(telecom_system.operation_mode==BER_PLOT_passband)
	{
		telecom_system.BER_PLOT_passband_process_main();
	}

	return 0;
}

