/* Used to transmit data to Mercury in TX_SHM mode
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

#define READ_LEN 512

int main(int argc, char *argv[])
{
    FILE *input;
    uint8_t data[SHM_PAYLOAD_BUFFER_SIZE];
    cbuf_handle_t buffer = circular_buf_connect_shm(SHM_PAYLOAD_BUFFER_SIZE, SHM_PAYLOAD_NAME);

    if (argc > 1 && !strcmp(argv[1], "-h"))
    {
        fprintf(stderr, "Usage: %s [in_file]\n", argv[0]);
        fprintf(stderr, "In case output_file is omitted, stdin is used;");
        return 0;
    }

    if (buffer == NULL)
    {
        fprintf(stderr, "Shared memory not created\n");
        return 0;
    }

    if (argv[1] != NULL)
        input = fopen(argv[1], "r");
    else
        input = stdin;

    while(!feof(input))
    {
        size_t size = fread(data, 1, READ_LEN, input);
        write_buffer(buffer, data, size);
        // fprintf(stderr, "frame of size %lu sent to modem\n", size);
    }

    fprintf(stderr, "End of file.\n");

    circular_buf_free_shm(buffer);

    return 0;
}
