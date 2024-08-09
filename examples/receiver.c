/* Used to receive data from Mercury in RX_SHM mode
 *
 * Copyright (C) 2024 Rafael Diniz
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include "common/shm_posix.h"
#include "common/ring_buffer_posix.h"
#include "common/common_defines.h"
// #define SHM_PAYLOAD_BUFFER_SIZE 131072
// #define SHM_PAYLOAD_NAME "/mercury-comm"

#include <threads.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
    FILE *output;
    uint8_t data[SHM_PAYLOAD_BUFFER_SIZE];
    cbuf_handle_t buffer = circular_buf_connect_shm(SHM_PAYLOAD_BUFFER_SIZE, SHM_PAYLOAD_NAME);


    if (argc > 1 && !strcmp(argv[1], "-h"))
    {
        fprintf(stderr, "Usage: %s [output_file]\n", argv[0]);
        fprintf(stderr, "In case output_file is omitted, stdout is used;");
        return 0;
    }

    if (buffer == NULL)
    {
        fprintf(stderr, "Shared memory not created\n");
        return 0;
    }

    if (argv[1] != NULL)
        output = fopen(argv[1], "w");
    else
        output = stdout;

    char spinner[] = ".oOo";
    int counter = 0;
    while(true)
    {
        int size = read_buffer_all(buffer, data);
        fwrite(data, size, 1, output);
        fflush(output);
        fprintf(stderr, "%c\r", spinner[counter++ % 4]);
    }

    circular_buf_free_shm(buffer);

    return 0;
}
