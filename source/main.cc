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

#include <iostream>
#include <complex>
#include <fstream>
#include <math.h>
#include <unistd.h>
#include <iostream>
#include <complex>
#include "physical_layer/telecom_system.h"
#include "datalink_layer/arq.h"

int main(int argc, char *argv[])
{
    // defaults to CPU 3
    int cpu_nr = 3;
    cl_telecom_system telecom_system;

    // seed the random number generator
    srand(time(0));

    // default mode
    telecom_system.operation_mode=ARQ_MODE;

    if (argc < 2)
    {
    manual:
        printf("Usage modes: \n%s -c [cpu_nr] -m [mode]\n", argv[0], argv[0]);
        printf("%s -h\n", argv[0]);
        printf("\nOptions:\n");
        printf(" -c [cpu_nr]                Run on CPU [cpu_br]. Defaults to CPU 3. Use -1 to disable CPU selection\n");
        printf(" -m [mode]                  Available modes are: ARQ, TX, RX, TX_TEST, RX_TEST, PLOT_BASEBAND, PLOT_PASSBAND\n");
        printf(" -l                         List all modulator/coding modes\n");
        printf(" -h                         Prints this help.\n");
        return EXIT_FAILURE;
    }

    int opt;
    while ((opt = getopt(argc, argv, "hc:m:l")) != -1)
    {
        switch (opt)
        {
        case 'c':
            if(optarg)
                cpu_nr = atoi(optarg);
            break;
        case 'm':
            if (!strcmp(optarg, "ARQ"))
                telecom_system.operation_mode=ARQ_MODE;
            if (!strcmp(optarg, "TX_TEST"))
                telecom_system.operation_mode=TX_TEST;
            if (!strcmp(optarg, "RX_TEST"))
                telecom_system.operation_mode=RX_TEST;
            if (!strcmp(optarg, "TX"))
                telecom_system.operation_mode=TX_BROADCAST;
            if (!strcmp(optarg, "RX"))
                telecom_system.operation_mode=RX_BROADCAST;
            if (!strcmp(optarg, "PLOT_BASEBAND"))
                telecom_system.operation_mode=BER_PLOT_baseband;
            if (!strcmp(optarg, "PLOT_PASSBAND"))
                telecom_system.operation_mode=BER_PLOT_passband;
            break;
        case 'l':
            for (int i = 0; i < NUMBER_OF_CONFIGS; i++)
            {
                telecom_system.load_configuration(i);
                printf("CONFIG_%d (%f bps)\n", i, telecom_system.rbc);
            }
            exit(EXIT_SUCCESS);
            break;

        case 'h':
        default:
            goto manual;
        }
    }

   if (cpu_nr != -1)
   {
       cpu_set_t mask;
       CPU_ZERO(&mask);
       CPU_SET(cpu_nr, &mask);
       sched_setaffinity(0, sizeof(mask), &mask);
       printf("RUNNING ON CPU Nr %d\n", sched_getcpu());
   }

   if(telecom_system.operation_mode == ARQ_MODE)
   {
       printf("Mode selected: ARQ\n");
       cl_arq_controller ARQ;
       ARQ.telecom_system=&telecom_system;
       ARQ.init();
       ARQ.print_stats();
       while(1)
       {
           ARQ.process_main();
       }
   }

    if(telecom_system.operation_mode == RX_TEST)
    {
        printf("Mode selected: RX_TEST\n");
        telecom_system.load_configuration();
        telecom_system.constellation_plot.open("PLOT");
        telecom_system.constellation_plot.reset("PLOT");

        while(1)
        {
            telecom_system.RX_TEST_process_main();
        }
        telecom_system.constellation_plot.close();
    }

    if (telecom_system.operation_mode == TX_TEST)
    {
        printf("Mode selected: TX_TEST\n");
        telecom_system.load_configuration();
        while(1)
        {
            telecom_system.TX_TEST_process_main();
        }
    }

    if (telecom_system.operation_mode == BER_PLOT_baseband)
    {
        printf("Mode selected: PLOT_BASEBAND\n");
        telecom_system.load_configuration();
        telecom_system.constellation_plot.open("PLOT");
        telecom_system.constellation_plot.reset("PLOT");
        telecom_system.BER_PLOT_baseband_process_main();
        telecom_system.constellation_plot.close();
    }

    if(telecom_system.operation_mode == BER_PLOT_passband)
    {
        printf("Mode selected: PLOT_PASSBAND\n");
        telecom_system.load_configuration();
        telecom_system.constellation_plot.open("PLOT");
        telecom_system.constellation_plot.reset("PLOT");
        telecom_system.BER_PLOT_passband_process_main();
        telecom_system.constellation_plot.close();
    }

    if(telecom_system.operation_mode == RX_BROADCAST)
    {
        printf("Mode selected: RX_BROADCAST\n");
        telecom_system.load_configuration();
        telecom_system.constellation_plot.open("PLOT");
        telecom_system.constellation_plot.reset("PLOT");

        while(1)
        {
            telecom_system.RX_BROADCAST_process_main();
        }
        telecom_system.constellation_plot.close();

    }

    if(telecom_system.operation_mode == TX_BROADCAST)
    {
        printf("Mode selected: TX_BROADCAST\n");
        telecom_system.load_configuration();

        while(1)
        {
            telecom_system.TX_BROADCAST_process_main();
        }

    }

    return 0;
}

