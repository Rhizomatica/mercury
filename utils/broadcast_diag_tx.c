/* Mercury Modem — broadcast diagnostic transmitter
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
#define DEFAULT_FRAME_SIZE 510
#define DEFAULT_INTERVAL_MS 200
#define CONFIG_PACKET_SIZE 9

#if defined(MSG_NOSIGNAL)
#define DIAG_SEND_FLAGS MSG_NOSIGNAL
#else
#define DIAG_SEND_FLAGS 0
#endif

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

static int send_all(int socket_fd, const uint8_t *buffer, size_t len)
{
    size_t sent_total = 0;
    while (sent_total < len)
    {
        ssize_t sent = send(socket_fd, (const char *)(buffer + sent_total), len - sent_total, DIAG_SEND_FLAGS);
        if (sent <= 0)
        {
            return -1;
        }
        sent_total += (size_t)sent;
    }
    return 0;
}

static void fill_frame(uint8_t *frame, size_t frame_size, uint8_t packet_type, uint64_t seq)
{
    memset(frame, 0, frame_size);

    for (size_t i = 1; i < frame_size; i++)
    {
        frame[i] = (uint8_t)((seq + (i * 17)) & 0xff);
    }

    if (packet_type == PACKET_TYPE_BROADCAST_CONTROL &&
        frame_size >= BROADCAST_CONFIG_PACKET_SIZE)
    {
        /* deterministic 8-byte config payload for debug */
        frame[1] = 0xAA;
        frame[2] = 0x55;
        frame[3] = 0x10;
        frame[4] = 0x20;
        frame[5] = 0x30;
        frame[6] = (uint8_t)(seq & 0xff);
        frame[7] = (uint8_t)((seq >> 8) & 0xff);
        frame[8] = 0x01;
    }

    write_frame_header(frame, packet_type, 0);
}

static void print_frame_debug(const char *tag, const uint8_t *frame, size_t frame_size, int kiss_len, uint64_t seq)
{
    uint8_t packet_type = frame_header_packet_type(frame[0]);
    uint8_t extension = frame_header_extension(frame[0]);

    printf("[%s] seq=%llu type=0x%02x ext=0x%02x frame=%zu kiss=%d first16=",
           tag, (unsigned long long)seq, packet_type, extension, frame_size, kiss_len);
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
    size_t frame_size = (argc > 3) ? (size_t)atoi(argv[3]) : DEFAULT_FRAME_SIZE;
    int interval_ms = (argc > 4) ? atoi(argv[4]) : DEFAULT_INTERVAL_MS;

    if (frame_size < CONFIG_PACKET_SIZE)
    {
        fprintf(stderr, "frame_size must be >= %d\n", CONFIG_PACKET_SIZE);
        return EXIT_FAILURE;
    }
    if (frame_size > MAX_PAYLOAD)
    {
        fprintf(stderr, "frame_size must be <= %d\n", MAX_PAYLOAD);
        return EXIT_FAILURE;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    int tcp_socket = create_tcp_socket(ip, port);
    if (tcp_socket < 0)
        return EXIT_FAILURE;

    printf("Connected to %s:%d, frame_size=%zu, interval_ms=%d\n", ip, port, frame_size, interval_ms);

    uint8_t *frame = (uint8_t *)malloc(frame_size);
    uint8_t *kiss_frame = (uint8_t *)malloc((frame_size * 2) + 3);
    if (!frame || !kiss_frame)
    {
        fprintf(stderr, "allocation failed\n");
        free(frame);
        free(kiss_frame);
        SOCK_CLOSE(tcp_socket);
        return EXIT_FAILURE;
    }

    uint64_t seq = 0;
    while (running)
    {
        fill_frame(frame, frame_size, PACKET_TYPE_BROADCAST_CONTROL, seq);
        int kiss_len = kiss_write_frame(frame, (int)frame_size, kiss_frame);
        print_frame_debug("TX", frame, frame_size, kiss_len, seq);
        if (send_all(tcp_socket, kiss_frame, (size_t)kiss_len) < 0)
        {
            perror("send config frame failed");
            break;
        }

        fill_frame(frame, frame_size, PACKET_TYPE_BROADCAST_DATA, seq);
        kiss_len = kiss_write_frame(frame, (int)frame_size, kiss_frame);
        print_frame_debug("TX", frame, frame_size, kiss_len, seq);
        if (send_all(tcp_socket, kiss_frame, (size_t)kiss_len) < 0)
        {
            perror("send payload frame failed");
            break;
        }

        seq++;
#if defined(_WIN32)
        Sleep((DWORD)interval_ms);
#else
        usleep((useconds_t)interval_ms * 1000);
#endif
    }

    free(frame);
    free(kiss_frame);
    SOCK_CLOSE(tcp_socket);
    printf("broadcast_diag_tx terminated\n");
    return EXIT_SUCCESS;
}
