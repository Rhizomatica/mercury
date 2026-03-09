/* HERMES Modem
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/types.h>
#if !defined(_WIN32)
#include <unistd.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include "os_interop.h"

#include "tcp_interfaces.h"
#include "ring_buffer_posix.h"
#include "net.h"
#include "arq.h"
#include "fsm.h"
#include "chan.h"
#include "defines_modem.h"
#include "kiss.h"
#include "hermes_log.h"
#include "radio_io.h"

static pthread_t tid[7];
static bool tid_started[7];
static int arq_tcp_base_port_cfg = 0;
static int broadcast_tcp_port_cfg = 0;
static size_t broadcast_frame_size_cfg = 0;
static float last_sn_value = 0.0f;
static uint32_t last_bitrate_sl = 0;
static uint32_t last_bitrate_bps = 0;
static chan_t *tnc_tx_chan = NULL;
static atomic_ulong tnc_tx_drop_count = 0;
static atomic_int tnc_last_buffer_sent = -1;

#if defined(MSG_NOSIGNAL)
#define HERMES_SEND_FLAGS MSG_NOSIGNAL
#else
#define HERMES_SEND_FLAGS 0
#endif

extern cbuf_handle_t data_tx_buffer_arq;
extern cbuf_handle_t data_rx_buffer_arq;

extern cbuf_handle_t data_tx_buffer_broadcast;
extern cbuf_handle_t data_rx_buffer_broadcast;

extern bool shutdown_;

extern arq_info arq_conn;

static ssize_t send_all(int socket_fd, const uint8_t *buffer, size_t len)
{
    size_t total_sent = 0;

    while (total_sent < len)
    {
        ssize_t sent = send(socket_fd, (const char *)buffer + total_sent, len - total_sent, HERMES_SEND_FLAGS);
        if (sent <= 0)
        {
            return -1;
        }
        total_sent += (size_t)sent;
    }

    return (ssize_t)total_sent;
}

typedef struct
{
    size_t len;
    uint8_t data[128];
} tnc_tx_msg_t;

static int tnc_queue_line(const char *line)
{
    chan_t *send_chans[1];
    void *send_msgs[1];
    tnc_tx_msg_t *msg;
    size_t len;
    int rc;

    if (!line || !tnc_tx_chan)
        return -1;

    len = strlen(line);
    if (len == 0 || len >= sizeof(((tnc_tx_msg_t *)0)->data))
        return -1;

    msg = (tnc_tx_msg_t *)calloc(1, sizeof(*msg));
    if (!msg)
        return -1;

    msg->len = len;
    memcpy(msg->data, line, len);
    send_chans[0] = tnc_tx_chan;
    send_msgs[0] = msg;
    rc = chan_select(NULL, 0, NULL, send_chans, 1, send_msgs);
    if (rc != 0)
    {
        free(msg);
        atomic_fetch_add_explicit(&tnc_tx_drop_count, 1, memory_order_relaxed);
        return -1;
    }

    return 0;
}

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

static int set_nonblocking(int fd)
{
    if (fd < 0)
        return -1;
    return sock_set_nonblocking(fd);
}

static int open_listener_socket(int port, int port_type, const char *tag)
{
    int fd;
    int opt = 1;
    struct sockaddr_in local_addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt)) < 0)
    {
        SOCK_CLOSE(fd);
        return -1;
    }

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0)
    {
        SOCK_CLOSE(fd);
        return -1;
    }
    if (listen(fd, 1) < 0)
    {
        SOCK_CLOSE(fd);
        return -1;
    }
    if (set_nonblocking(fd) < 0)
    {
        SOCK_CLOSE(fd);
        return -1;
    }

    net_set_status(port_type, NET_LISTENING);
    HLOGI(tag, "Listening on TCP port %d", port);
    return fd;
}

static int arq_buffered_bytes_snapshot(void)
{
    arq_runtime_snapshot_t snapshot;
    int buffered = 0;

    if (arq_get_runtime_snapshot(&snapshot))
        buffered = snapshot.tx_backlog_bytes;

    if (buffered < 0)
        buffered = 0;
    return buffered;
}

static void execute_control_command(char *buffer)
{
    arq_cmd_msg_t cmd;
    char temp[16] = {0};

    if (!buffer)
        return;

    HLOGI("tcp-ctl", "Command received: %s", buffer);

    if (!memcmp(buffer, "MYCALL", strlen("MYCALL")))
    {
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = ARQ_CMD_SET_CALLSIGN;
        if (sscanf(buffer, "MYCALL %15s", cmd.arg0) == 1 &&
            arq_submit_tcp_cmd(&cmd) == 0)
            tcp_write(CTL_TCP_PORT, (uint8_t *)"OK\r", 3);
        else
            tcp_write(CTL_TCP_PORT, (uint8_t *)"WRONG\r", 6);
        return;
    }

    if (!memcmp(buffer, "LISTEN", strlen("LISTEN")))
    {
        memset(&cmd, 0, sizeof(cmd));
        sscanf(buffer, "LISTEN %15s", temp);
        if (temp[1] == 'N')
            cmd.type = ARQ_CMD_LISTEN_ON;
        if (temp[1] == 'F')
            cmd.type = ARQ_CMD_LISTEN_OFF;
        if (cmd.type != ARQ_CMD_NONE && arq_submit_tcp_cmd(&cmd) == 0)
            tcp_write(CTL_TCP_PORT, (uint8_t *)"OK\r", 3);
        else
            tcp_write(CTL_TCP_PORT, (uint8_t *)"WRONG\r", 6);
        return;
    }

    if (!memcmp(buffer, "PUBLIC", strlen("PUBLIC")))
    {
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = ARQ_CMD_SET_PUBLIC;
        sscanf(buffer, "PUBLIC %15s", temp);
        if (temp[1] == 'N')
            cmd.flag = true;
        if (temp[1] == 'F')
            cmd.flag = false;
        if (arq_submit_tcp_cmd(&cmd) == 0)
            tcp_write(CTL_TCP_PORT, (uint8_t *)"OK\r", 3);
        else
            tcp_write(CTL_TCP_PORT, (uint8_t *)"WRONG\r", 6);
        return;
    }

    if (!memcmp(buffer, "COMPRESSION", strlen("COMPRESSION")))
    {
        /* Compatibility no-op: VARA-style clients expect this command. */
        tcp_write(CTL_TCP_PORT, (uint8_t *)"OK\r", 3);
        return;
    }

    if (!memcmp(buffer, "CHAT", strlen("CHAT")))
    {
        /* CHAT ON implies LISTEN ON (VARA compatibility).
         * CHAT OFF is a no-op — Mercury does not limit idle loops. */
        sscanf(buffer, "CHAT %15s", temp);
        if (temp[1] == 'N')
        {
            memset(&cmd, 0, sizeof(cmd));
            cmd.type = ARQ_CMD_LISTEN_ON;
            if (arq_submit_tcp_cmd(&cmd) == 0)
                tcp_write(CTL_TCP_PORT, (uint8_t *)"OK\r", 3);
            else
                tcp_write(CTL_TCP_PORT, (uint8_t *)"WRONG\r", 6);
        }
        else
        {
            tcp_write(CTL_TCP_PORT, (uint8_t *)"OK\r", 3);
        }
        return;
    }

    if (!memcmp(buffer, "BW", strlen("BW")))
    {
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = ARQ_CMD_SET_BANDWIDTH;
        if (sscanf(buffer, "BW%d", &cmd.value) == 1 &&
            arq_submit_tcp_cmd(&cmd) == 0)
            tcp_write(CTL_TCP_PORT, (uint8_t *)"OK\r", 3);
        else
            tcp_write(CTL_TCP_PORT, (uint8_t *)"WRONG\r", 6);
        return;
    }

    if (!memcmp(buffer, "BUFFER", strlen("BUFFER")))
    {
        int buffered = arq_buffered_bytes_snapshot();
        tnc_send_buffer((uint32_t)buffered);
        return;
    }

    if (!memcmp(buffer, "SN", strlen("SN")))
    {
        tnc_send_sn(last_sn_value);
        return;
    }

    if (!memcmp(buffer, "BITRATE", strlen("BITRATE")))
    {
        tnc_send_bitrate(last_bitrate_sl, last_bitrate_bps);
        return;
    }

    if (!memcmp(buffer, "P2P", strlen("P2P")))
    {
        tcp_write(CTL_TCP_PORT, (uint8_t *)"OK\r", 3);
        return;
    }

    if (!memcmp(buffer, "CONNECT", strlen("CONNECT")))
    {
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = ARQ_CMD_CONNECT;
        if (sscanf(buffer, "CONNECT %15s %15s", cmd.arg0, cmd.arg1) == 2 &&
            arq_submit_tcp_cmd(&cmd) == 0)
            tcp_write(CTL_TCP_PORT, (uint8_t *)"OK\r", 3);
        else
            tcp_write(CTL_TCP_PORT, (uint8_t *)"WRONG\r", 6);
        return;
    }

    if (!memcmp(buffer, "DISCONNECT", strlen("DISCONNECT")))
    {
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = ARQ_CMD_DISCONNECT;
        if (arq_submit_tcp_cmd(&cmd) == 0)
            tcp_write(CTL_TCP_PORT, (uint8_t *)"OK\r", 3);
        else
            tcp_write(CTL_TCP_PORT, (uint8_t *)"WRONG\r", 6);
        return;
    }

    HLOGW("tcp-ctl", "Unknown command: %s", buffer);
    tcp_write(CTL_TCP_PORT, (uint8_t *)"WRONG\r", 6);
}

static void process_control_bytes(char *line_buf, int *line_len, const uint8_t *data, ssize_t len)
{
    ssize_t i;

    if (!line_buf || !line_len || !data || len <= 0)
        return;

    for (i = 0; i < len; i++)
    {
        if (data[i] == '\r')
        {
            line_buf[*line_len] = 0;
            execute_control_command(line_buf);
            *line_len = 0;
            continue;
        }

        if (*line_len >= TCP_BLOCK_SIZE)
        {
            *line_len = 0;
            HLOGW("tcp-ctl", "ERROR in command parsing: line too long");
            continue;
        }

        line_buf[*line_len] = (char)data[i];
        (*line_len)++;
    }
}

static void close_data_client(int *data_client_fd)
{
    if (!data_client_fd || *data_client_fd < 0)
        return;

    SOCK_CLOSE(*data_client_fd);
    *data_client_fd = -1;
    cli_data_sockfd = -1;
    net_set_status(DATA_TCP_PORT, NET_LISTENING);
    HLOGI("tcp-data", "Data client disconnected");
}

static void close_ctl_client(int *ctl_client_fd, int *data_client_fd, bool notify_arq)
{
    if (!ctl_client_fd || *ctl_client_fd < 0)
        return;

    SOCK_CLOSE(*ctl_client_fd);
    *ctl_client_fd = -1;
    cli_ctl_sockfd = -1;
    net_set_status(CTL_TCP_PORT, NET_LISTENING);
    atomic_store_explicit(&tnc_last_buffer_sent, -1, memory_order_relaxed);
    HLOGI("tcp-ctl", "Control client disconnected");

    if (notify_arq)
    {
        arq_cmd_msg_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = ARQ_CMD_CLIENT_DISCONNECT;
        (void)arq_submit_tcp_cmd(&cmd);
    }

    close_data_client(data_client_fd);
}

static void drain_tnc_queue_to_ctl(void)
{
    chan_t *recv_chans[1];
    void *raw = NULL;

    if (!tnc_tx_chan)
        return;

    recv_chans[0] = tnc_tx_chan;
    while (chan_select(recv_chans, 1, &raw, NULL, 0, NULL) == 0)
    {
        tnc_tx_msg_t *msg = (tnc_tx_msg_t *)raw;
        if (msg)
        {
            ssize_t sent = tcp_write(CTL_TCP_PORT, msg->data, msg->len);
            if (sent < (ssize_t)msg->len)
                atomic_fetch_add_explicit(&tnc_tx_drop_count, 1, memory_order_relaxed);
            free(msg);
        }
        raw = NULL;
    }
}

static void dispose_tnc_tx_queue(void)
{
    chan_t *recv_chans[1];
    void *raw = NULL;

    if (!tnc_tx_chan)
        return;

    chan_close(tnc_tx_chan);
    recv_chans[0] = tnc_tx_chan;
    while (chan_select(recv_chans, 1, &raw, NULL, 0, NULL) == 0)
    {
        free(raw);
        raw = NULL;
    }
    chan_dispose(tnc_tx_chan);
    tnc_tx_chan = NULL;
    atomic_store_explicit(&tnc_tx_drop_count, 0, memory_order_relaxed);
    atomic_store_explicit(&tnc_last_buffer_sent, -1, memory_order_relaxed);
}

static void *arq_reactor_thread(void *port)
{
    int tcp_base_port = *((int *)port);
    int ctl_listener = -1;
    int data_listener = -1;
    int ctl_client = -1;
    int data_client = -1;
    char ctl_line[TCP_BLOCK_SIZE + 1] = {0};
    int ctl_len = 0;
    int last_buffer_report = -1;
    uint64_t next_keepalive_ms = 0;
    uint64_t next_buffer_report_ms = 0;
    uint8_t rx_buf[TCP_BLOCK_SIZE];
    uint8_t tx_buf[DATA_TX_BUFFER_SIZE];

    ctl_listener = open_listener_socket(tcp_base_port, CTL_TCP_PORT, "tcp-ctl");
    if (ctl_listener < 0)
    {
        HLOGE("tcp-ctl", "Could not open TCP port %d", tcp_base_port);
        shutdown_ = true;
        return NULL;
    }

    data_listener = open_listener_socket(tcp_base_port + 1, DATA_TCP_PORT, "tcp-data");
    if (data_listener < 0)
    {
        HLOGE("tcp-data", "Could not open TCP port %d", tcp_base_port + 1);
        SOCK_CLOSE(ctl_listener);
        net_set_status(CTL_TCP_PORT, NET_NONE);
        shutdown_ = true;
        return NULL;
    }

    while (!shutdown_)
    {
        struct pollfd pfds[4];
        int nfds = 0;
        uint64_t now_ms;

        drain_tnc_queue_to_ctl();
        memset(pfds, 0, sizeof(pfds));
        pfds[nfds].fd = ctl_listener;
        pfds[nfds].events = POLLIN;
        nfds++;

        pfds[nfds].fd = data_listener;
        pfds[nfds].events = POLLIN;
        nfds++;

        if (ctl_client >= 0)
        {
            pfds[nfds].fd = ctl_client;
            pfds[nfds].events = POLLIN | POLLERR | POLLHUP;
            nfds++;
        }

        if (data_client >= 0)
        {
            pfds[nfds].fd = data_client;
            pfds[nfds].events = POLLIN | POLLERR | POLLHUP;
            nfds++;
        }

        (void)poll(pfds, (nfds_t)nfds, 100);
        now_ms = monotonic_ms();

        if (pfds[0].revents & POLLIN)
        {
            int fd = accept(ctl_listener, NULL, NULL);
            if (fd >= 0)
            {
                if (set_nonblocking(fd) < 0)
                {
                    SOCK_CLOSE(fd);
                }
                else
                {
                    arq_cmd_msg_t cmd;
                    if (ctl_client >= 0)
                        close_ctl_client(&ctl_client, &data_client, true);
                    ctl_client = fd;
                    cli_ctl_sockfd = fd;
                    net_set_status(CTL_TCP_PORT, NET_CONNECTED);
                    memset(&cmd, 0, sizeof(cmd));
                    cmd.type = ARQ_CMD_CLIENT_CONNECT;
                    (void)arq_submit_tcp_cmd(&cmd);
                    next_keepalive_ms = now_ms + 60000ULL;
                    next_buffer_report_ms = now_ms + 1000ULL;
                    last_buffer_report = -1;
                    atomic_store_explicit(&tnc_last_buffer_sent, -1, memory_order_relaxed);
                    ctl_len = 0;
                    HLOGI("tcp-ctl", "Control client connected");
                }
            }
        }

        if (pfds[1].revents & POLLIN)
        {
            int fd = accept(data_listener, NULL, NULL);
            if (fd >= 0)
            {
                if (set_nonblocking(fd) < 0)
                {
                    SOCK_CLOSE(fd);
                }
                else
                {
                    if (data_client >= 0)
                        close_data_client(&data_client);
                    data_client = fd;
                    cli_data_sockfd = fd;
                    net_set_status(DATA_TCP_PORT, NET_CONNECTED);
                    HLOGI("tcp-data", "Data client connected");
                }
            }
        }

        if (ctl_client >= 0)
        {
            short events = pfds[2].revents;
            if (events & (POLLERR | POLLHUP | POLLNVAL))
            {
                close_ctl_client(&ctl_client, &data_client, true);
            }
            else if (events & POLLIN)
            {
                ssize_t n = recv(ctl_client, (char *)rx_buf, sizeof(rx_buf), 0);
                if (n > 0)
                {
                    process_control_bytes(ctl_line, &ctl_len, rx_buf, n);
                }
                else if (n == 0 || (n < 0 && sock_errno() != SOCK_EAGAIN && sock_errno() != SOCK_EWOULDBLOCK && sock_errno() != SOCK_EINTR))
                {
                    close_ctl_client(&ctl_client, &data_client, true);
                }
            }
        }

        if (data_client >= 0)
        {
            short events = pfds[(ctl_client >= 0) ? 3 : 2].revents;
            if (events & (POLLERR | POLLHUP | POLLNVAL))
            {
                close_data_client(&data_client);
            }
            else if (events & POLLIN)
            {
                ssize_t n = recv(data_client, (char *)rx_buf, sizeof(rx_buf), 0);
                if (n > 0)
                {
                    int queued = arq_submit_tcp_payload(rx_buf, (size_t)n);
                    if (queued < 0)
                        HLOGW("tcp-data", "Failed to queue ARQ data frame(s)");
                    else
                        tnc_send_buffer((uint32_t)arq_buffered_bytes_snapshot());
                }
                else if (n == 0 || (n < 0 && sock_errno() != SOCK_EAGAIN && sock_errno() != SOCK_EWOULDBLOCK && sock_errno() != SOCK_EINTR))
                {
                    close_data_client(&data_client);
                }
            }
        }

        if (ctl_client >= 0)
        {
            if (net_get_status(CTL_TCP_PORT) != NET_CONNECTED)
                close_ctl_client(&ctl_client, &data_client, true);
            else
            {
                unsigned long dropped =
                    atomic_exchange_explicit(&tnc_tx_drop_count, 0, memory_order_relaxed);
                if (dropped > 0)
                    HLOGW("tcp-ctl", "Dropped %lu queued control messages", dropped);
                if (now_ms >= next_keepalive_ms)
                {
                    char imalive[] = "IAMALIVE\r";
                    (void)tcp_write(CTL_TCP_PORT, (uint8_t *)imalive, strlen(imalive));
                    next_keepalive_ms = now_ms + 60000ULL;
                }
                if (now_ms >= next_buffer_report_ms)
                {
                    int buffered = arq_buffered_bytes_snapshot();
                    if (buffered != last_buffer_report)
                    {
                        tnc_send_buffer((uint32_t)buffered);
                        last_buffer_report = buffered;
                    }
                    next_buffer_report_ms = now_ms + 1000ULL;
                }
            }
        }

        if (data_client >= 0)
        {
            if (net_get_status(DATA_TCP_PORT) != NET_CONNECTED)
                close_data_client(&data_client);
            else
            {
                size_t available = size_buffer(data_rx_buffer_arq);
                if (available > DATA_TX_BUFFER_SIZE)
                    available = DATA_TX_BUFFER_SIZE;
                if (available > 0)
                {
                    if (read_buffer(data_rx_buffer_arq, tx_buf, available) == 0)
                    {
                        ssize_t sent = tcp_write(DATA_TCP_PORT, tx_buf, available);
                        if (sent < (ssize_t)available)
                            HLOGW("tcp-data", "Partial DATA write (%zd/%zu)", sent, available);
                    }
                }
            }
        }
    }

    close_ctl_client(&ctl_client, &data_client, true);
    close_data_client(&data_client);
    drain_tnc_queue_to_ctl();
    if (ctl_listener >= 0)
        SOCK_CLOSE(ctl_listener);
    if (data_listener >= 0)
        SOCK_CLOSE(data_listener);
    net_set_status(CTL_TCP_PORT, NET_NONE);
    net_set_status(DATA_TCP_PORT, NET_NONE);
    return NULL;
}

/********** ARQ TCP ports INTERFACES **********/
void *server_worker_thread_ctl(void *port)
{
    HLOGW("tcp", "server_worker_thread_ctl now runs unified ARQ reactor");
    return arq_reactor_thread(port);
}

void *server_worker_thread_data(void *port)
{
    (void)port;
    HLOGW("tcp", "server_worker_thread_data is deprecated (reactor owns DATA socket)");
    return NULL;
}

// tx to tcp socket the received data from the modem
void *data_worker_thread_tx(void *conn)
{
    (void)conn;
    HLOGW("tcp", "data_worker_thread_tx is deprecated (reactor owns DATA TX)");
    return NULL;
}

// rx from tcp socket and send to trasmit by the modem
void *data_worker_thread_rx(void *conn)
{
    (void)conn;
    HLOGW("tcp", "data_worker_thread_rx is deprecated (reactor owns DATA RX)");
    return NULL;
}

void *control_worker_thread_tx(void *conn)
{
    (void)conn;
    HLOGW("tcp", "control_worker_thread_tx is deprecated (reactor owns CTL TX)");
    return NULL;
}

void *control_worker_thread_rx(void *conn)
{
    (void)conn;
    HLOGW("tcp", "control_worker_thread_rx is deprecated (reactor owns CTL RX)");
    return NULL;
}


/********** BROADCAST TCP port INTERFACE **********/
void *send_thread(void *client_socket_ptr)
{
    int client_socket = *((int *)client_socket_ptr);
    size_t frame_size = broadcast_frame_size_cfg;
    uint8_t *frame_buffer = NULL;
    uint8_t *kiss_buffer = NULL;

    if (frame_size == 0 || frame_size > MAX_PAYLOAD)
    {
        fprintf(stderr, "Invalid broadcast frame size: %zu\n", frame_size);
        return NULL;
    }

    frame_buffer = (uint8_t *)malloc(frame_size);
    kiss_buffer = (uint8_t *)malloc((frame_size * 2) + 3);

    if (!frame_buffer || !kiss_buffer)
    {
        fprintf(stderr, "Failed to allocate memory for send buffer.\n");
        free(frame_buffer);
        free(kiss_buffer);
        return NULL;
    }

    while (!shutdown_)
    {
        if (read_buffer(data_rx_buffer_broadcast, frame_buffer, frame_size) < 0)
            break;

        int kiss_len = kiss_write_frame(frame_buffer, (int)frame_size, kiss_buffer);
        if (send_all(client_socket, kiss_buffer, (size_t)kiss_len) < 0)
        {
            perror("Error sending KISS broadcast frame");
            break;
        }
    }

    free(frame_buffer);
    free(kiss_buffer);
    return NULL;
}

void *recv_thread(void *client_socket_ptr)
{
    int client_socket = *((int *)client_socket_ptr);
    size_t frame_size = broadcast_frame_size_cfg;
    uint8_t *buffer = (uint8_t *)malloc(DATA_TX_BUFFER_SIZE);
    uint8_t decoded_frame[MAX_PAYLOAD];

    if (!buffer)
    {
        fprintf(stderr, "Failed to allocate memory for recv buffer.\n");
        return NULL;
    }

    // Reset global KISS parser state for each new client session.
    kiss_reset_state();

    if (frame_size == 0 || frame_size > MAX_PAYLOAD)
    {
        fprintf(stderr, "Invalid broadcast frame size: %zu\n", frame_size);
        goto cleanup;
    }

    while (!shutdown_)
    {
        ssize_t received = recv(client_socket, (char *)buffer, DATA_TX_BUFFER_SIZE, 0);
        if (received > 0)
        {
            for (ssize_t i = 0; i < received; i++)
            {
                int frame_len = kiss_read(buffer[i], decoded_frame);
                if (frame_len <= 0)
                    continue;

                if ((size_t)frame_len != frame_size)
                {
                    fprintf(stderr, "Discarding broadcast frame with unexpected size %d (expected %zu)\n",
                            frame_len, frame_size);
                    continue;
                }

                write_buffer(data_tx_buffer_broadcast, decoded_frame, frame_size);
            }
        }
        else if (received == 0)
        {
            printf("Client disconnected.\n");
            break;
        }
        else if (received < 0)
        {
            perror("Error receiving TCP data");
            break;
        }
    }

cleanup:
    free(buffer);
    return NULL;
}


// Function to handle TCP server logic
void *tcp_server_thread(void *port_ptr)
{
    int tcp_port = *((int *)port_ptr);
    int tcp_socket, client_socket;
    int opt = 1;
    struct sockaddr_in local_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // Open TCP socket
    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_socket < 0)
    {
        perror("Failed to create TCP socket");
        return NULL;
    }

    if (setsockopt(tcp_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt)) < 0)
    {
        perror("Failed to set SO_REUSEADDR on broadcast TCP socket");
        SOCK_CLOSE(tcp_socket);
        return NULL;
    }

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(tcp_port);

    if (bind(tcp_socket, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0)
    {
        perror("Failed to bind TCP socket");
        SOCK_CLOSE(tcp_socket);
        return NULL;
    }

    if (listen(tcp_socket, 1) < 0)
    {
        perror("Failed to listen on TCP socket");
        SOCK_CLOSE(tcp_socket);
        return NULL;
    }

    printf("Waiting for a client to connect...\n");

    while (!shutdown_)
    {
        client_socket = accept(tcp_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket < 0)
        {
            perror("Failed to accept client connection");
            if (shutdown_)
                break; // Exit if shutdown flag is set
            continue; // Retry accepting a connection
        }

        printf("Client connected.\n");

        pthread_t recv_tid, send_tid;

        // Create threads for receiving and sending data
        pthread_create(&recv_tid, NULL, recv_thread, (void *)&client_socket);
        pthread_create(&send_tid, NULL, send_thread, (void *)&client_socket);

        // Wait for threads to finish
        pthread_join(recv_tid, NULL);
        pthread_cancel(send_tid);
        pthread_join(send_tid, NULL);

        SOCK_CLOSE(client_socket);
        printf("Waiting for a new client to connect...\n");
    }

    SOCK_CLOSE(tcp_socket);
    return NULL;
}

/*********** TNC / radio functions ***********/
void ptt_on()
{
    arq_conn.TRX = TX;
    if (radio_io_enabled())
        radio_io_key_on();
    /* Queue "PTT ON\r" asynchronously so a transient write failure on the
     * non-blocking CTL socket does NOT set NET_RESTART and kill the DATA
     * socket (which would send EOF to uucico and abort the UUCP session).
     * The PTT state change (arq_conn.TRX = TX) is already synchronous and
     * is what the modem RX thread actually polls; the TCP notification to
     * uucpd is informational only. */
    (void)tnc_queue_line("PTT ON\r");
    HLOGI("radio", "TX enabled (PTT ON)");
}

void ptt_off()
{
    arq_conn.TRX = RX;
    if (radio_io_enabled())
        radio_io_key_off();
    /* Same reasoning as ptt_on(): queue asynchronously. */
    (void)tnc_queue_line("PTT OFF\r");
    HLOGI("radio", "TX disabled (PTT OFF)");
}

void tnc_send_connected()
{
    char buffer[128];
    sprintf(buffer, "CONNECTED %s %s %d\r", arq_conn.my_call_sign, arq_conn.dst_addr, 2300);
    if (tnc_queue_line(buffer) < 0)
        HLOGW("tcp-ctl", "Error queuing connected message");
}

void tnc_send_disconnected()
{
    char buffer[128];
    sprintf(buffer, "DISCONNECTED\r");
    if (tnc_queue_line(buffer) < 0)
        HLOGW("tcp-ctl", "Error queuing disconnected message");
}

void tnc_send_buffer(uint32_t bytes)
{
    char buffer[64];
    int last = atomic_load_explicit(&tnc_last_buffer_sent, memory_order_relaxed);

    if (last >= 0 && (uint32_t)last == bytes)
        return;

    snprintf(buffer, sizeof(buffer), "BUFFER %u\r", bytes);
    if (tnc_queue_line(buffer) == 0)
        atomic_store_explicit(&tnc_last_buffer_sent, (int)bytes, memory_order_relaxed);
}

void tnc_send_sn(float snr)
{
    char buffer[64];
    last_sn_value = snr;
    snprintf(buffer, sizeof(buffer), "SN %.1f\r", snr);
    (void)tnc_queue_line(buffer);
}

void tnc_send_bitrate(uint32_t speed_level, uint32_t bps)
{
    char buffer[64];
    last_bitrate_sl = speed_level;
    last_bitrate_bps = bps;
    snprintf(buffer, sizeof(buffer), "BITRATE (%u) %u BPS\r", speed_level, bps);
    (void)tnc_queue_line(buffer);
}

int interfaces_init(int arq_tcp_base_port, int broadcast_tcp_port, size_t broadcast_frame_size)
{
    arq_tcp_base_port_cfg = arq_tcp_base_port;
    broadcast_tcp_port_cfg = broadcast_tcp_port;
    broadcast_frame_size_cfg = broadcast_frame_size;
    memset(tid_started, 0, sizeof(tid_started));

    /*************** ARQ TCP ports *******************/
    net_set_status(CTL_TCP_PORT, NET_NONE);
    net_set_status(DATA_TCP_PORT, NET_NONE);
    if (!tnc_tx_chan)
    {
        tnc_tx_chan = chan_init(256);
        if (!tnc_tx_chan)
        {
            HLOGE("tcp-ctl", "Failed to init control TX queue");
            return EXIT_FAILURE;
        }
    }
    atomic_store_explicit(&tnc_last_buffer_sent, -1, memory_order_relaxed);

    if (pthread_create(&tid[0], NULL, arq_reactor_thread, (void *)&arq_tcp_base_port_cfg) != 0)
    {
        HLOGE("tcp", "Failed to start ARQ reactor thread");
        dispose_tnc_tx_queue();
        return EXIT_FAILURE;
    }
    tid_started[0] = true;


    /*************** BROADCAST TCP ports **************/
    // Create TCP BROADCAST server thread
    if (pthread_create(&tid[6], NULL, tcp_server_thread, (void *)&broadcast_tcp_port_cfg) != 0)
    {
        HLOGE("tcp", "Failed to start broadcast TCP thread");
        shutdown_ = true;
        if (tid_started[0])
        {
            pthread_join(tid[0], NULL);
            tid_started[0] = false;
        }
        dispose_tnc_tx_queue();
        return EXIT_FAILURE;
    }
    tid_started[6] = true;

    
    return EXIT_SUCCESS;
}

void interfaces_shutdown()
{
    int i;

    for (i = 0; i < 7; i++)
    {
        if (tid_started[i])
        {
            pthread_join(tid[i], NULL);
            tid_started[i] = false;
        }
    }
    dispose_tnc_tx_queue();
}
