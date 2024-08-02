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

// some globals to workaround C++ hell
double carrier_frequency_offset; // set 0 to stock HF, or to the radio passband, eg., 15k for sBitx
int radio_type;

int main(int argc, char *argv[])
{
    // defaults to CPU 3
    int cpu_nr = 3;
    bool list_modes = false;
    int mod_config = 0;
    int operation_mode = ARQ_MODE;

    // seed the random number generator
    srand(time(0));

    if (argc < 2)
    {
 manual:
        printf("Usage modes: \n%s -c [cpu_nr] -m [mode]\n", argv[0], argv[0]);
        printf("%s -h\n", argv[0]);
        printf("\nOptions:\n");
        printf(" -c [cpu_nr]                Run on CPU [cpu_br]. Defaults to CPU 3. Use -1 to disable CPU selection\n");
        printf(" -m [mode]                  Available operating modes are: ARQ, TX, RX, TX_TEST, RX_TEST, PLOT_BASEBAND, PLOT_PASSBAND\n");
        printf(" -s [modulation_config]     Sets modulation configuration for non-ARQ setups (0 to 16). Use \"-l\" for listing all available modulations\n");
        printf(" -r [radio_type]            Available radio types are: stockhf, sbitx");
        printf(" -l                         List all modulator/coding modes\n");
        printf(" -h                         Prints this help.\n");
        return EXIT_FAILURE;
    }

    int opt;
    while ((opt = getopt(argc, argv, "hc:m:s:lr:")) != -1)
    {
        switch (opt)
        {
        case 'r':
            if (!strcmp(optarg, "stockhf"))
            {
                printf("Stock HF Radio Selected.\n");
                carrier_frequency_offset = 0;
                radio_type = RADIO_STOCKHF;
            }
            if (!strcmp(optarg, "sbitx"))
            {
                printf("sBitx HF Radio Selected.\n");
                carrier_frequency_offset = 15000.0;
                radio_type = RADIO_SBITX;
            }
            if (strcmp(optarg, "sbitx") && strcmp(optarg, "stockhf"))
            {
                printf("Wrong radio.\n");
                goto manual;
            }
            break;
        case 'c':
            if (optarg)
                cpu_nr = atoi(optarg);
            break;
        case 'm':
            if (!strcmp(optarg, "ARQ"))
                operation_mode = ARQ_MODE;
            if (!strcmp(optarg, "TX_TEST"))
                operation_mode = TX_TEST;
            if (!strcmp(optarg, "RX_TEST"))
                operation_mode = RX_TEST;
            if (!strcmp(optarg, "TX"))
                operation_mode = TX_BROADCAST;
            if (!strcmp(optarg, "RX"))
                operation_mode = RX_BROADCAST;
            if (!strcmp(optarg, "PLOT_BASEBAND"))
                operation_mode = BER_PLOT_baseband;
            if (!strcmp(optarg, "PLOT_PASSBAND"))
                operation_mode = BER_PLOT_passband;
            break;
        case 's':
            if (optarg)
                mod_config = atoi(optarg);
            break;
        case 'l':
            list_modes = true;
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

    cl_telecom_system telecom_system;
    telecom_system.operation_mode = operation_mode;

    if (list_modes)
    {
        for (int i = 0; i < NUMBER_OF_CONFIGS; i++)
        {
            telecom_system.load_configuration(i);
            printf("CONFIG_%d (%f bps), frame_size: %d Bytes / %d bits\n", i,
                   telecom_system.rbc, telecom_system.get_frame_size_bytes(),
                   telecom_system.get_frame_size_bits());
        }
        return EXIT_SUCCESS;
    }


    if ((mod_config >= NUMBER_OF_CONFIGS) || (mod_config < 0))
    {
        printf("Wrong modulation config %d\n", mod_config);
        exit(EXIT_FAILURE);
    }

    if (telecom_system.operation_mode == ARQ_MODE)
    {
        printf("Mode selected: ARQ\n");
        cl_arq_controller ARQ;
        ARQ.telecom_system = &telecom_system;
        ARQ.init();
        ARQ.print_stats();
        while (1)
        {
            ARQ.process_main();
        }
    }

    if (telecom_system.operation_mode == RX_TEST)
    {
        printf("Mode selected: RX_TEST\n");
        telecom_system.load_configuration(mod_config);
        printf("CONFIG_%d (%f bps)\n", mod_config, telecom_system.rbc);
        telecom_system.constellation_plot.open("PLOT");
        telecom_system.constellation_plot.reset("PLOT");

        while (1)
        {
            telecom_system.RX_TEST_process_main();
        }
        telecom_system.constellation_plot.close();
    }

    if (telecom_system.operation_mode == TX_TEST)
    {
        printf("Mode selected: TX_TEST\n");
        telecom_system.load_configuration(mod_config);
        printf("CONFIG_%d (%f bps)\n", mod_config, telecom_system.rbc);

        while (1)
        {
            telecom_system.TX_TEST_process_main();
        }
    }

    if (telecom_system.operation_mode == BER_PLOT_baseband)
    {
        printf("Mode selected: PLOT_BASEBAND\n");
        telecom_system.load_configuration(mod_config);
        printf("CONFIG_%d (%f bps)\n", mod_config, telecom_system.rbc);
        telecom_system.constellation_plot.open("PLOT");
        telecom_system.constellation_plot.reset("PLOT");

        telecom_system.BER_PLOT_baseband_process_main();

        telecom_system.constellation_plot.close();
    }

    if (telecom_system.operation_mode == BER_PLOT_passband)
    {
        printf("Mode selected: PLOT_PASSBAND\n");
        telecom_system.load_configuration(mod_config);
        printf("CONFIG_%d (%f bps)\n", mod_config, telecom_system.rbc);
        telecom_system.constellation_plot.open("PLOT");
        telecom_system.constellation_plot.reset("PLOT");

        telecom_system.BER_PLOT_passband_process_main();

        telecom_system.constellation_plot.close();
    }

    if (telecom_system.operation_mode == RX_BROADCAST)
    {
        printf("Mode selected: RX_BROADCAST\n");
        telecom_system.load_configuration(mod_config);
        printf("CONFIG_%d (%f bps)\n", mod_config, telecom_system.rbc);
        telecom_system.constellation_plot.open("PLOT");
        telecom_system.constellation_plot.reset("PLOT");

        while (1)
        {
            telecom_system.RX_BROADCAST_process_main();
        }

        telecom_system.constellation_plot.close();      // uff...

    }

    if (telecom_system.operation_mode == TX_BROADCAST)
    {
        printf("Mode selected: TX_BROADCAST\n");
        telecom_system.load_configuration(mod_config);
        printf("CONFIG_%d (%f bps)\n", mod_config, telecom_system.rbc);

        while (1)
        {
            telecom_system.TX_BROADCAST_process_main();
        }

    }

    return EXIT_SUCCESS;
}
