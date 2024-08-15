/*
 * Mercury: A configurable open-source software-defined modem.
 * Copyright (C) 2022-2024 Fadi Jerji
 *               2024 Rhizomatica
 * Authors: Fadi Jerji
 *          Rafael Diniz
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

// some globals
double carrier_frequency_offset; // set 0 to stock HF, or to the radio passband, eg., 15k for sBitx
int radio_type;
char alsa_input_dev[ALSA_MAX_PATH];
char alsa_output_dev[ALSA_MAX_PATH];

int main(int argc, char *argv[])
{
    // defaults to CPU 3
    int cpu_nr = 3;
    bool list_modes = false;
    int mod_config = 0;
    int operation_mode = ARQ_MODE;

    strcpy(alsa_input_dev, "plughw:0,0");
    strcpy(alsa_output_dev, "plughw:0,0");

    // seed the random number generator
    srand(time(0));

    if (argc < 2)
    {
 manual:
        printf("Usage modes: \n%s -c [cpu_nr] -m [mode] -i [device] -o [device] -r [radio_type]\n", argv[0], argv[0]);
        printf("%s -c [cpu_nr] -m ARQ -i [device] -o [device] -r [radio_type]\n", argv[0], argv[0]);
        printf("%s -h\n", argv[0]);
        printf("\nOptions:\n");
        printf(" -c [cpu_nr]                Run on CPU [cpu_br]. Defaults to CPU 3. Use -1 to disable CPU selection\n");
        printf(" -m [mode]                  Available operating modes are: ARQ, TX_SHM, RX_SHM, TX_TEST, RX_TEST, TX_RAND, RX_RAND, PLOT_BASEBAND, PLOT_PASSBAND\n");
        printf(" -s [modulation_config]     Sets modulation configuration for non-ARQ setups (0 to 16). Use \"-l\" for listing all available modulations\n");
        printf(" -r [radio_type]            Available radio types are: stockhf, sbitx\n");
        printf(" -i [device]                Radio INPUT (capture) ALSA device (default: \"plughw:0,0\")\n");
        printf(" -o [device]                Radio OUPUT (playback) ALSA device (default: \"plughw:0,0\")\n");
        printf(" -l                         List all modulator/coding modes\n");
        printf(" -h                         Prints this help.\n");
        return EXIT_FAILURE;
    }

    int opt;
    while ((opt = getopt(argc, argv, "hc:m:s:lr:i:o:")) != -1)
    {
        switch (opt)
        {
        case 'i':
            if (optarg)
                strncpy(alsa_input_dev, optarg, ALSA_MAX_PATH-1);
            break;
        case 'o':
            if (optarg)
                strncpy(alsa_output_dev, optarg, ALSA_MAX_PATH-1);
            break;
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
            if (!strcmp(optarg, "TX_RAND"))
                operation_mode = TX_RAND;
            if (!strcmp(optarg, "RX_RAND"))
                operation_mode = RX_RAND;
            if (!strcmp(optarg, "TX_TEST"))
                operation_mode = TX_TEST;
            if (!strcmp(optarg, "RX_TEST"))
                operation_mode = RX_TEST;
            if (!strcmp(optarg, "TX_SHM"))
                operation_mode = TX_SHM;
            if (!strcmp(optarg, "RX_SHM"))
                operation_mode = RX_SHM;
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
            printf("CONFIG_%d (%f bps), frame_size: %d Bytes / %d bits / %d non-byte-aligned bits\n", i,
                   telecom_system.rbc, telecom_system.get_frame_size_bytes(),
                   telecom_system.get_frame_size_bits(), telecom_system.get_frame_size_bits() - (telecom_system.get_frame_size_bytes() * 8));
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

    if (telecom_system.operation_mode == RX_RAND)
    {
        printf("Mode selected: RX_RAND\n");
        telecom_system.load_configuration(mod_config);
        printf("CONFIG_%d (%f bps) Shannon_limit: %f\n", mod_config, telecom_system.rbc, telecom_system.Shannon_limit);

        telecom_system.constellation_plot.open("PLOT");
        telecom_system.constellation_plot.reset("PLOT");

        while (1)
        {
            telecom_system.RX_RAND_process_main();
        }
        telecom_system.constellation_plot.close(); // o_O
    }

    if (telecom_system.operation_mode == TX_RAND)
    {
        printf("Mode selected: TX_RAND\n");
        telecom_system.load_configuration(mod_config);
        printf("CONFIG_%d (%f bps) Shannon_limit: %f\n", mod_config, telecom_system.rbc, telecom_system.Shannon_limit);

        while (1)
        {
            telecom_system.TX_RAND_process_main();
        }
    }

    if (telecom_system.operation_mode == BER_PLOT_baseband)
    {
        printf("Mode selected: PLOT_BASEBAND\n");
        telecom_system.load_configuration(mod_config);
        printf("CONFIG_%d (%f bps) Shannon_limit: %f\n", mod_config, telecom_system.rbc, telecom_system.Shannon_limit);

        telecom_system.constellation_plot.open("PLOT");
        telecom_system.constellation_plot.reset("PLOT");

        telecom_system.BER_PLOT_baseband_process_main();

        telecom_system.constellation_plot.close();
    }

    if (telecom_system.operation_mode == BER_PLOT_passband)
    {
        printf("Mode selected: PLOT_PASSBAND\n");
        telecom_system.load_configuration(mod_config);
        printf("CONFIG_%d (%f bps) Shannon_limit: %f\n", mod_config, telecom_system.rbc, telecom_system.Shannon_limit);

        telecom_system.constellation_plot.open("PLOT");
        telecom_system.constellation_plot.reset("PLOT");

        telecom_system.BER_PLOT_passband_process_main();

        telecom_system.constellation_plot.close();
    }

    if (telecom_system.operation_mode == RX_TEST)
    {
        printf("Mode selected: RX_TEST\n");
        telecom_system.load_configuration(mod_config);
        printf("CONFIG_%d (%f bps) Shannon_limit: %f\n", mod_config, telecom_system.rbc, telecom_system.Shannon_limit);

        while (1)
        {
            telecom_system.RX_TEST_process_main();
        }

    }

    if (telecom_system.operation_mode == TX_TEST)
    {
        printf("Mode selected: TX_TEST\n");
        telecom_system.load_configuration(mod_config);
        printf("CONFIG_%d (%f bps) Shannon_limit: %.2f db\n", mod_config, telecom_system.rbc, telecom_system.Shannon_limit);

        while (1)
        {
            telecom_system.TX_TEST_process_main();
        }

    }


    if (telecom_system.operation_mode == RX_SHM)
    {

        telecom_system.load_configuration(mod_config);
        printf("Mode selected: RX_SHM\n");
        printf("Modulation: %d  Bitrate: %.2f bps  Shannon_limit: %.2f db\n",  mod_config, telecom_system.rbc, telecom_system.Shannon_limit);

        cbuf_handle_t buffer;

        buffer = circular_buf_init_shm(SHM_PAYLOAD_BUFFER_SIZE, (char *) SHM_PAYLOAD_NAME);

        while (1)
        {
            telecom_system.RX_SHM_process_main(buffer);
        }

        // for the future...
        circular_buf_destroy_shm(buffer, SHM_PAYLOAD_BUFFER_SIZE, (char *) SHM_PAYLOAD_NAME);
        circular_buf_free_shm(buffer);
    }

    if (telecom_system.operation_mode == TX_SHM)
    {
        printf("Mode: TX_SHM  Modulation config: %d\n", mod_config);
        telecom_system.load_configuration(mod_config);
        printf("Bitrate: %.2f bps  Shannon_limit: %.2f db TX: ",  mod_config, telecom_system.rbc, telecom_system.Shannon_limit);

        cbuf_handle_t buffer;

        buffer = circular_buf_init_shm(SHM_PAYLOAD_BUFFER_SIZE, (char *) SHM_PAYLOAD_NAME);

        while (1)
        {
            telecom_system.TX_SHM_process_main(buffer);
        }

        // for the future...
        circular_buf_destroy_shm(buffer, SHM_PAYLOAD_BUFFER_SIZE, (char *) SHM_PAYLOAD_NAME);
        circular_buf_free_shm(buffer);
    }


    return EXIT_SUCCESS;
}
