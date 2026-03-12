/* Mercury Modem — broadcast diagnostic receiver
 *
 * Copyright (C) 2025-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#if !defined(_WIN32)
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "../common/os_interop.h"
#include "framer.h"
#include "kiss.h"

#define DEFAULT_IP "127.0.0.1"
#define DEFAULT_PORT 8100
#define CONFIG_PACKET_SIZE 9

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

static int create_tcp_socket(const char *ip, int port)
{
    int tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_socket < 0)
    {
        perror("Failed to create TCP socket");
        return -1;
    }

    struct sockaddr_in modem_addr;
    memset(&modem_addr, 0, sizeof(modem_addr));
    modem_addr.sin_family = AF_INET;
    modem_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &modem_addr.sin_addr) <= 0)
    {
        perror("Invalid modem IP address");
        SOCK_CLOSE(tcp_socket);
        return -1;
    }

    if (connect(tcp_socket, (struct sockaddr *)&modem_addr, sizeof(modem_addr)) < 0)
    {
        perror("Failed to connect to modem");
        SOCK_CLOSE(tcp_socket);
        return -1;
    }

    return tcp_socket;
}

static const char *packet_type_name(uint8_t packet_type)
{
    switch (packet_type)
    {
    case PACKET_TYPE_BROADCAST_CONTROL:
        return "BCAST_CTRL";
    case PACKET_TYPE_BROADCAST_DATA:
        return "BCAST_DATA";
    default:
        return "OTHER";
    }
}

static void print_frame_debug(uint64_t frame_no, const uint8_t *frame, size_t frame_size)
{
    if (frame_size == 0)
    {
        printf("[RX] frame=%llu EMPTY\n", (unsigned long long)frame_no);
        return;
    }

    uint8_t packet_type = frame_header_packet_type(frame[0]);
    uint8_t extension = frame_header_extension(frame[0]);

    printf("[RX] frame=%llu len=%zu type=0x%02x(%s) ext=0x%02x first16=",
           (unsigned long long)frame_no, frame_size, packet_type,
           packet_type_name(packet_type), extension);
    for (size_t i = 0; i < frame_size && i < 16; i++)
    {
        printf("%02x ", frame[i]);
    }
    printf("\n");
}

int main(int argc, char *argv[])
{
    const char *ip = (argc > 1) ? argv[1] : DEFAULT_IP;
    int port = (argc > 2) ? atoi(argv[2]) : DEFAULT_PORT;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    int tcp_socket = create_tcp_socket(ip, port);
    if (tcp_socket < 0)
        return EXIT_FAILURE;

    printf("Connected to %s:%d\n", ip, port);

    uint8_t rx_buf[4096];
    uint8_t frame_buf[MAX_PAYLOAD];
    uint64_t frame_no = 0;
    uint64_t ctrl_frames = 0, data_frames = 0, other_frames = 0;

    while (running)
    {
        ssize_t n = recv(tcp_socket, (char *)rx_buf, sizeof(rx_buf), 0);
        if (n == 0)
        {
            printf("Server disconnected\n");
            break;
        }
        if (n < 0)
        {
            if (sock_errno() == SOCK_EINTR)
                continue;
            perror("recv failed");
            break;
        }

        printf("[RX] raw_bytes=%zd\n", n);
        for (ssize_t i = 0; i < n; i++)
        {
            int frame_len = kiss_read(rx_buf[i], frame_buf);
            if (frame_len <= 0)
                continue;

            frame_no++;
            print_frame_debug(frame_no, frame_buf, (size_t)frame_len);

            uint8_t packet_type = frame_header_packet_type(frame_buf[0]);
            if (packet_type == PACKET_TYPE_BROADCAST_CONTROL)
                ctrl_frames++;
            else if (packet_type == PACKET_TYPE_BROADCAST_DATA)
                data_frames++;
            else
                other_frames++;

            if ((frame_no % 20) == 0)
            {
                printf("[RX-SUM] frames=%llu ctrl=%llu data=%llu other=%llu\n",
                       (unsigned long long)frame_no,
                       (unsigned long long)ctrl_frames,
                       (unsigned long long)data_frames,
                       (unsigned long long)other_frames);
            }
        }
    }

    SOCK_CLOSE(tcp_socket);
    printf("broadcast_diag_rx terminated\n");
    return EXIT_SUCCESS;
}
