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
#include "datalink_layer/fsm.h"
#include "audioio/audioio.h"

// some globals TODO: wrap this up into some struct
extern "C" {
    double carrier_frequency_offset; // set 0 to stock HF, or to the radio passband, eg., 15k for sBitx
    int radio_type;
    char *input_dev;
    char *output_dev;
    bool shutdown_;
}

uint32_t mercury_frame_size[] = { 12, 25, 37, 50, 62, 75, 100, 62, 75, 100, 74, 99, 175, 100, 174, 175, 162};

int main(int argc, char *argv[])
{
    int cpu_nr = -1;
    bool list_modes = false;
    bool list_sndcards = false;

    int mod_config = CONFIG_0;
    int operation_mode = ARQ_MODE;
    int gear_shift_mode = NO_GEAR_SHIFT;
    int base_tcp_port = DEFAULT_TCP_PORT;

    int audio_system = -1;

    input_dev = (char *) malloc(ALSA_MAX_PATH);
    output_dev = (char *) malloc(ALSA_MAX_PATH);
    input_dev[0] = 0;
    output_dev[0] = 0;

#if defined(__linux__)
    printf("\e[0;31mMercury Version %s\e[0m\n", VERSION__); // we go red
#elif defined(_WIN32)
    printf("Mercury Version %s\n", VERSION__);
#endif


    if (argc < 2)
    {
 manual:
        printf("Usage modes: \n%s -m [mode] -s [modulation_config] -i [device] -o [device] -r [radio_type] -x [sound_system]\n", argv[0], argv[0]);
        printf("%s -m ARQ -s [modulation_config] -i [device] -o [device] -r [radio_type] -x [sound_system] -p [arq_tcp_base_port]\n", argv[0], argv[0]);
        printf("%s -h\n", argv[0]);
        printf("\nOptions:\n");
        printf(" -c [cpu_nr]                Run on CPU [cpu_br]. Use -1 to disable CPU selection, which is the default.\n");
        printf(" -m [mode]                  Available operating modes are: ARQ, TX_SHM, RX_SHM, TX_TEST, RX_TEST, TX_RAND, RX_RAND, PLOT_BASEBAND, PLOT_PASSBAND.\n");
        printf(" -s [modulation_config]     Sets modulation configuration for all modes including ARQ, except when gear-shift is enabled. Modes: 0 to 16. Use \"-l\" for listing all available modulations.\n");
        printf(" -r [radio_type]            Available radio types are: stockhf, sbitx.\n");
        printf(" -i [device]                Radio Capture device id (eg: \"plughw:0,0\").\n");
        printf(" -o [device]                Radio Playback device id (eg: \"plughw:0,0\").\n");
        printf(" -x [sound_system]          Sets the sound system API to use: alsa, pulse, dsound or wasapi. Default is alsa on Linux and dsound on Windows.\n");
		printf(" -p [arq_tcp_base_port]     Sets the ARQ TCP base port (control is base_port, data is base_port + 1). Default is 7002.\n");
        printf(" -g                         Enables the adaptive modulation selection (gear-shifting). Not working yet!.\n");
        printf(" -l                         Lists all modulator/coding modes.\n");
        printf(" -z                         Lists all available sound cards.\n");
        printf(" -h                         Prints this help.\n");
        return EXIT_FAILURE;
    }

    int opt;
    while ((opt = getopt(argc, argv, "hc:m:s:lr:i:o:x:p:zg")) != -1)
    {
        switch (opt)
        {
        case 'i':
            if (optarg)
                strncpy(input_dev, optarg, ALSA_MAX_PATH-1);
            break;
        case 'o':
            if (optarg)
                strncpy(output_dev, optarg, ALSA_MAX_PATH-1);
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
        case 'p':
            if (optarg)
				base_tcp_port = atoi(optarg);
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
        case 'x':
            if (!strcmp(optarg, "alsa"))
                audio_system = AUDIO_SUBSYSTEM_ALSA;
            if (!strcmp(optarg, "pulse"))
                audio_system = AUDIO_SUBSYSTEM_PULSE;
            if (!strcmp(optarg, "dsound"))
                audio_system = AUDIO_SUBSYSTEM_DSOUND;
            if (!strcmp(optarg, "wasapi"))
                audio_system = AUDIO_SUBSYSTEM_WASAPI;
            if (!strcmp(optarg, "oss"))
                audio_system = AUDIO_SUBSYSTEM_OSS;
            if (!strcmp(optarg, "coreaudio"))
                audio_system = AUDIO_SUBSYSTEM_COREAUDIO;
            break;
        case 'g':
            gear_shift_mode = GEAR_SHIFT_ENABLED;
            break;
        case 'z':
            list_sndcards = true;
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
#if defined(__linux__)
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(cpu_nr, &mask);
        sched_setaffinity(0, sizeof(mask), &mask);
        printf("RUNNING ON CPU Nr %d\n", sched_getcpu());
#else
        cpu_nr = -1;
#endif
    }

    // set some defaults... in case the user did not select
    if (audio_system == -1)
    {
#if defined(__linux__)
        audio_system = AUDIO_SUBSYSTEM_ALSA;
#elif defined(_WIN32)
        audio_system = AUDIO_SUBSYSTEM_DSOUND;
#endif
    }

    printf("Audio System: ");
    switch(audio_system)
    {
    case AUDIO_SUBSYSTEM_ALSA:
        if(input_dev[0] == 0)
            strcpy(input_dev, "default");
        if(output_dev[0] == 0)
            strcpy(output_dev, "default");
        printf("Advanced Linux Sound Architecture (ALSA)\n");
        break;
    case AUDIO_SUBSYSTEM_PULSE:
        if (input_dev[0] == 0)
        {
            free(input_dev);
            input_dev = NULL;
        }
        if (output_dev[0] == 0)
        {
            free(output_dev);
            output_dev = NULL;
        }
        printf("PulseAudio\n");
        break;
    case AUDIO_SUBSYSTEM_WASAPI:
        if (input_dev[0] == 0)
        {
            free(input_dev);
            input_dev = NULL;
        }
        if (output_dev[0] == 0)
        {
            free(output_dev);
            output_dev = NULL;
        }
        printf("Windows Audio Session API (WASAPI)\n");
        break;
    case AUDIO_SUBSYSTEM_DSOUND:
        if (input_dev[0] == 0)
        {
            free(input_dev);
            input_dev = NULL;
        }
        if (output_dev[0] == 0)
        {
            free(output_dev);
            output_dev = NULL;
        }
        printf("Microsoft DirectSound (DSOUND)\n");
        break;
    default:
        printf("Selected audio system not supported. Trying to continue.\n");
    }

    if (list_sndcards)
    {
        list_soundcards(audio_system);
        if (input_dev)
            free(input_dev);
        if (output_dev)
            free(output_dev);
        return EXIT_SUCCESS;
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

    // initializing audio system
    pthread_t radio_capture, radio_playback, radio_capture_prep;

    if (telecom_system.operation_mode == ARQ_MODE)
    {
        printf("Mode selected: ARQ\n");

        arq_telecom_system = &telecom_system;
        printf("TNC TCP Ports: %d (CTRL) %d (DATA)\n", base_tcp_port, base_tcp_port+1); 
        arq_init(base_tcp_port, (gear_shift_mode == NO_GEAR_SHIFT)? NO : YES, mod_config);

        printf("INITIAL CONFIG_%d (%f bps), frame_size: %d Bytes / %d bits / %d non-byte-aligned bits\n", mod_config,
                   telecom_system.rbc, telecom_system.get_frame_size_bytes(),
                   telecom_system.get_frame_size_bits(), telecom_system.get_frame_size_bits() - (telecom_system.get_frame_size_bytes() * 8));

        
        audioio_init_internal(input_dev, output_dev, audio_system, &radio_capture,
                              &radio_playback, &radio_capture_prep, &telecom_system);

        // and we block here in pthread_join()
        arq_shutdown();
    }

    if (telecom_system.operation_mode == RX_RAND)
    {
        printf("Mode selected: RX_RAND\n");
        telecom_system.load_configuration(mod_config);
        printf("Modulation: %d  Bitrate: %.2f bps  Shannon_limit: %.2f db\n",  mod_config, telecom_system.rbc, telecom_system.Shannon_limit);

        telecom_system.constellation_plot.open("PLOT");
        telecom_system.constellation_plot.reset("PLOT");

        audioio_init_internal(input_dev, output_dev, audio_system, &radio_capture,
                              &radio_playback, &radio_capture_prep, &telecom_system);

        while (!shutdown_)
        {
            telecom_system.RX_RAND_process_main();
        }
        telecom_system.constellation_plot.close();
    }

    if (telecom_system.operation_mode == TX_RAND)
    {
        printf("Mode selected: TX_RAND\n");
        telecom_system.load_configuration(mod_config);
        printf("Modulation: %d  Bitrate: %.2f bps  Shannon_limit: %.2f db\n",  mod_config, telecom_system.rbc, telecom_system.Shannon_limit);

        audioio_init_internal(input_dev, output_dev, audio_system, &radio_capture,
                              &radio_playback, &radio_capture_prep, &telecom_system);

        while (!shutdown_)
        {
            telecom_system.TX_RAND_process_main();
        }
    }

    if (telecom_system.operation_mode == BER_PLOT_baseband)
    {
        printf("Mode selected: PLOT_BASEBAND\n");
        telecom_system.load_configuration(mod_config);
        printf("Modulation: %d  Bitrate: %.2f bps  Shannon_limit: %.2f db\n",  mod_config, telecom_system.rbc, telecom_system.Shannon_limit);

        telecom_system.constellation_plot.open("PLOT");
        telecom_system.constellation_plot.reset("PLOT");

        telecom_system.BER_PLOT_baseband_process_main();

        telecom_system.constellation_plot.close();
    }

    if (telecom_system.operation_mode == BER_PLOT_passband)
    {
        printf("Mode selected: PLOT_PASSBAND\n");
        telecom_system.load_configuration(mod_config);
        printf("Modulation: %d  Bitrate: %.2f bps  Shannon_limit: %.2f db\n",  mod_config, telecom_system.rbc, telecom_system.Shannon_limit);

        telecom_system.constellation_plot.open("PLOT");
        telecom_system.constellation_plot.reset("PLOT");

        telecom_system.BER_PLOT_passband_process_main();

        telecom_system.constellation_plot.close();
    }

    if (telecom_system.operation_mode == RX_TEST)
    {
        printf("Mode selected: RX_TEST\n");
        telecom_system.load_configuration(mod_config);
        printf("Modulation: %d  Bitrate: %.2f bps  Shannon_limit: %.2f db\n",  mod_config, telecom_system.rbc, telecom_system.Shannon_limit);

        audioio_init_internal(input_dev, output_dev, audio_system, &radio_capture,
                              &radio_playback, &radio_capture_prep, &telecom_system);

        while (!shutdown_)
        {
            telecom_system.RX_TEST_process_main();
        }
    }

    if (telecom_system.operation_mode == TX_TEST)
    {
        printf("Mode selected: TX_TEST\n");
        telecom_system.load_configuration(mod_config);
        printf("Modulation: %d  Bitrate: %.2f bps  Shannon_limit: %.2f db\n",  mod_config, telecom_system.rbc, telecom_system.Shannon_limit);

        audioio_init_internal(input_dev, output_dev, audio_system, &radio_capture,
                              &radio_playback, &radio_capture_prep, &telecom_system);

        while (!shutdown_)
        {
            telecom_system.TX_TEST_process_main();
        }

    }


    if (telecom_system.operation_mode == RX_SHM)
    {
        printf("Mode selected: RX_SHM\n");
        telecom_system.load_configuration(mod_config);
        printf("Modulation: %d  Bitrate: %.2f bps  Shannon_limit: %.2f db\n",  mod_config, telecom_system.rbc, telecom_system.Shannon_limit);

        cbuf_handle_t buffer;

        buffer = circular_buf_init_shm(SHM_PAYLOAD_BUFFER_SIZE, (char *) SHM_PAYLOAD_NAME);

        audioio_init_internal(input_dev, output_dev, audio_system, &radio_capture, &radio_playback, &radio_capture_prep, &telecom_system);

        while (!shutdown_)
        {
            telecom_system.RX_SHM_process_main(buffer);
        }

        circular_buf_destroy_shm(buffer, SHM_PAYLOAD_BUFFER_SIZE, (char *) SHM_PAYLOAD_NAME);
        circular_buf_free_shm(buffer);
    }

    if (telecom_system.operation_mode == TX_SHM)
    {
        printf("Mode: TX_SHM  Modulation config: %d\n", mod_config);
        telecom_system.load_configuration(mod_config);
        printf("Bitrate: %.2f bps  Shannon lim.: %.2f db  TX: ",  mod_config, telecom_system.rbc, telecom_system.Shannon_limit);

        cbuf_handle_t buffer;

        buffer = circular_buf_init_shm(SHM_PAYLOAD_BUFFER_SIZE, (char *) SHM_PAYLOAD_NAME);

        audioio_init_internal(input_dev, output_dev, audio_system, &radio_capture,
							  &radio_playback, &radio_capture_prep, &telecom_system);

        while (!shutdown_)
        {
            telecom_system.TX_SHM_process_main(buffer);
        }

        circular_buf_destroy_shm(buffer, SHM_PAYLOAD_BUFFER_SIZE, (char *) SHM_PAYLOAD_NAME);
        circular_buf_free_shm(buffer);
    }

    if (input_dev)
        free(input_dev);
    if (output_dev)
        free(output_dev);

    audioio_deinit(&radio_capture, &radio_playback, &radio_capture_prep);


    return EXIT_SUCCESS;
}
