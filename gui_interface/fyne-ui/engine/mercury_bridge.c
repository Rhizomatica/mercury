/* Mercury C Engine Bridge — thin CGo wrapper
 *
 * Delegates all init / teardown to mercury_engine_init() /
 * mercury_engine_shutdown() in common/mercury_engine.c so there
 * is exactly one copy of the init sequence.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cfg_utils.h"
#include "mercury_engine.h"
#include "mercury_cli.h"
#include "mercury_version.h"
#include "ui_communication.h"
#include "modem.h"
#include "modem_stats.h"   /* MODEM_STATS_NSPEC */
#include "message_store.h"
#include "bcast_file.h"

/* Print the startup version banner (same text the daemon prints), so the UI
 * announces its version on the terminal too.  Resolved here, in a unit the
 * libmercury_core.a rule recompiles every build, so the git hash stays fresh. */
void mercury_print_version(void)
{
    mercury_print_version_banner();
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/*  mercury_init(argc, argv, default_config, log_path)                 */
/*                                                                     */
/*  Runs the SAME CLI/config parser the daemon uses (Go forwards its   */
/*  os.Args here), then delegates to mercury_engine_init().            */
/*    default_config — mercury.ini path used when -C is absent (the    */
/*                     UI's per-user writable copy).                    */
/*    log_path       — engine log file (the UI's writable path); an    */
/*                     explicit -L overrides it.                        */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/*  mercury_precheck(argc, argv, default_config)                       */
/*                                                                     */
/*  Handle exit-only CLI actions (-h/-l/-z/-K/-Q) exactly as the       */
/*  daemon does — print to the terminal.  Called from Go BEFORE the    */
/*  window is created so `mercury-ui -h` shows help instead of opening */
/*  the GUI.  Returns non-zero if handled (Go should exit), 0 to run.  */
/* ------------------------------------------------------------------ */
int mercury_precheck(int argc, char **argv, const char *default_config)
{
    mercury_cli_t cli;
    if (mercury_cli_parse(argc, argv,
                          (default_config && default_config[0]) ? default_config : "mercury.ini",
                          &cli) != 0) {
        /* Go exits without running C's atexit flush, so a piped stdout/stderr
         * would otherwise lose the buffered getopt/usage text — flush now. */
        fflush(stdout);
        fflush(stderr);
        return 1;   /* parse error already reported → exit */
    }
    int handled = mercury_cli_run_info_action(&cli,
                      (argc > 0 && argv) ? argv[0] : "mercury-ui") ? 1 : 0;
    if (cli.action == MERCURY_CLI_TEST_PTT) {
        (void)mercury_cli_run_ptt_test(&cli);
        handled = 1;
    }
    fflush(stdout);
    fflush(stderr);
    return handled;
}

int mercury_init(int argc, char **argv, const char *default_config, const char *log_path)
{
    mercury_cli_t cli;
    if (mercury_cli_parse(argc, argv,
                          (default_config && default_config[0]) ? default_config : "mercury.ini",
                          &cli) != 0)
        return -1;

    /* The single-binary UI always runs the engine (the list/help actions are
     * daemon-only) and always enables the UI websocket. */
    cli.cfg.ui_enabled = true;

    const char *log = cli.log_file_path ? cli.log_file_path : log_path;
    return mercury_engine_init(&cli.cfg, cli.cfg_path, log, cli.log_file_jsonl,
                               cli.startup_mode, cli.test_mode);
}

/* ------------------------------------------------------------------ */
/*  mercury_request_shutdown()                                         */
/*                                                                     */
/*  Sets the global shutdown flag so background threads begin their    */
/*  exit sequences.  Call BEFORE mercury_shutdown().                    */
/* ------------------------------------------------------------------ */
void mercury_request_shutdown(void)
{
    shutdown_ = true;
}

/* ------------------------------------------------------------------ */
/*  mercury_shutdown()                                                 */
/*                                                                     */
/*  Delegates to mercury_engine_shutdown().                            */
/* ------------------------------------------------------------------ */
void mercury_shutdown(void)
{
    mercury_engine_shutdown();
}

/* ------------------------------------------------------------------ */
/*  mercury_ui_preload_device_lists()                                  */
/*                                                                     */
/*  Trigger radio-enumeration during engine init, on the main thread,   */
/*  so the Go goroutine that later reads the list never enters hamlib's */
/*  backend-loading path inside a CGo context.                          */
/* ------------------------------------------------------------------ */
void mercury_ui_preload_device_lists(void)
{
    ui_comm_preload_radio_list();
}

/* ------------------------------------------------------------------ */
/*  In-process UI link                                                 */
/*                                                                     */
/*  Thin forwarders: the engine owns the state and the command          */
/*  handling, so local and remote clients cannot drift apart.           */
/* ------------------------------------------------------------------ */
int mercury_ui_get_status(ui_status_t *out)
{
    return ui_comm_get_status(out) ? 1 : 0;
}

int mercury_ui_get_spectrum(float *out, int max_bins, int *sample_rate_hz,
                            unsigned long long *seq)
{
    if (!out || max_bins <= 0)
        return 0;

    int nbins = (max_bins < MODEM_STATS_NSPEC) ? max_bins : MODEM_STATS_NSPEC;
    uint64_t s = 0;
    int sr = modem_get_rx_spectrum_seq(out, nbins, &s);
    if (sr <= 0)
        return 0;
    if (sample_rate_hz)
        *sample_rate_hz = sr;
    if (seq)
        *seq = (unsigned long long)s;
    return nbins;
}

int mercury_ui_command(const char *command, const char *value,
                       const char *value2, const char *value3,
                       const char *value4, const char *value5,
                       const char *value6, const char *value7)
{
    return ui_comm_command(command, value, value2, value3, value4,
                           value5, value6, value7);
}

int mercury_ui_get_audio_devices(int kind, ui_device_t *out, int max,
                                 char *selected, int selected_len)
{
    return ui_comm_get_audio_devices((ui_device_kind_t)kind, out, max,
                                     selected, (size_t)selected_len);
}

int mercury_ui_get_radio_list(ui_device_t *out, int max,
                              char *selected, int selected_len,
                              char *device_path, int device_path_len,
                              int *serial_speed,
                              char *ptt_method, int method_len,
                              char *ptt_line, int line_len,
                              char *ptt_invert, int invert_len,
                              int *cm108_gpio)
{
    return ui_comm_get_radio_list(out, max, selected, (size_t)selected_len,
                                  device_path, (size_t)device_path_len,
                                  serial_speed, ptt_method, (size_t)method_len,
                                  ptt_line, (size_t)line_len,
                                  ptt_invert, (size_t)invert_len, cm108_gpio);
}

int mercury_ui_get_input_channel(void)
{
    return ui_comm_get_input_channel();
}

void mercury_ui_get_audio_system(char *name, int name_len)
{
    if (name && name_len > 0) {
        snprintf(name, (size_t)name_len, "%s", ui_comm_get_audio_system());
    }
}

void mercury_ui_get_audio_subsystems(char *names, int names_len)
{
    if (names && names_len > 0) {
        (void) ui_comm_get_audio_subsystems(names, (size_t)names_len);
    }
}

void mercury_ui_set_waterfall(bool enabled)
{
    ui_comm_set_waterfall(enabled);
}

void mercury_ui_get_tcp_ports(int *arq_base_port, int *broadcast_port)
{
    ui_comm_get_tcp_ports(arq_base_port, broadcast_port);
}

int mercury_ui_get_history(char *out, int out_len)
{
    size_t count = 0, len = 0;
    char *snap;

    if (!out || out_len <= 0)
        return 0;

    snap = msg_store_snapshot(&count, &len);
    if (!snap)
        return 0;

    if (len >= (size_t)out_len)
        len = (size_t)out_len - 1;
    memcpy(out, snap, len);
    out[len] = '\0';
    free(snap);
    return (int)len;
}

void mercury_ui_get_version(char *version, int version_len,
                            char *git_hash, int git_hash_len)
{
    if (version && version_len > 0) {
        snprintf(version, (size_t)version_len, "%s", MERCURY_VERSION);
    }
    if (git_hash && git_hash_len > 0) {
        snprintf(git_hash, (size_t)git_hash_len, "%s", GIT_HASH);
    }
}

/* ---- Broadcast file transmission ---------------------------------------- */

void *mercury_bcast_tx_open(const char *path, int mode, int cycles,
                            int session_id, char *err, int errlen)
{
    return bcast_file_tx_open(path, mode, cycles, session_id, err, (size_t)errlen);
}

int mercury_bcast_tx_next(void *tx, unsigned char *buf, int buflen)
{
    return bcast_file_tx_next((bcast_file_tx_t *)tx, buf, (size_t)buflen);
}

int mercury_bcast_tx_frame_size(void *tx)
{
    return bcast_file_tx_frame_size((bcast_file_tx_t *)tx);
}

void mercury_bcast_tx_stats(void *tx, int *cycle_now, int *cycles_total,
                            unsigned long long *frames_sent)
{
    uint64_t sent = 0;
    bcast_file_tx_stats((bcast_file_tx_t *)tx, cycle_now, cycles_total, &sent);
    if (frames_sent) *frames_sent = (unsigned long long)sent;
}

void mercury_bcast_tx_source(void *tx, long *file_bytes, int *blocks)
{
    size_t b = 0;
    bcast_file_tx_source((bcast_file_tx_t *)tx, &b, blocks);
    if (file_bytes) *file_bytes = (long)b;
}

void mercury_bcast_tx_close(void *tx)
{
    bcast_file_tx_close((bcast_file_tx_t *)tx);
}

int  mercury_bcast_mode_frame_size(int mode) { return bcast_file_mode_frame_size(mode); }
int  mercury_bcast_mode_usable(int mode)     { return bcast_file_mode_usable(mode); }
const char *mercury_bcast_mode_name(int mode) { return bcast_file_mode_name(mode); }
long mercury_bcast_max_file_bytes(void)      { return (long)BCAST_FILE_MAX_BYTES; }

/* Both accessors below report the LISTEN mode, not the live modem mode.
 * g_modem.mode follows whatever rung the ARQ ladder is currently on, so
 * reading it made the broadcast UI report -- and size its frames from -- a
 * mode unrelated to broadcast as soon as an ARQ session came up.  The listen
 * mode is the one broadcast actually uses: what the payload decoder sits on
 * while idle and what pure-broadcast TX is pinned to.  It also means these
 * follow a MODE control-port command instead of reporting the startup mode
 * forever.
 *
 * The hermes index they return is the same index space as -m, `mercury -l` and
 * MODE, and comes from the shared table in common/mercury_modes.c rather than
 * a copy here: it is a user-facing API, so two copies could drift apart. */
static int listen_mode_index(void)
{
    int m = modem_get_listen_mode();
    int count = mercury_cli_mode_count();
    for (int i = 0; i < count; i++)
        if (freedv_modes[i] == m)
            return i;
    return -1;
}

int mercury_bcast_engine_mode(void)
{
    int i = listen_mode_index();
    if (i < 0)
        return -1;
    /* Runnable by the engine is not the same as usable for broadcast.  A
     * broadcast frame has to carry the framing plus one whole RaptorQ symbol,
     * and FSK_LDPC and DATAC15 (30-byte frames) do not -- see
     * BCAST_SYMBOL_SIZE_MIN.  Report -1 for those, so the callers' existing
     * "this modem's mode cannot carry broadcast" paths fire BEFORE a transfer
     * is attempted, instead of the operator getting an opaque failure out of
     * tx_open. */
    return bcast_file_mode_usable(i) ? i : -1;
}

/* The mapped index whether or not broadcast can use it, so the UI can NAME the
 * mode it is refusing rather than saying only that something is wrong. */
int mercury_bcast_engine_mode_raw(void)
{
    return listen_mode_index();
}

int mercury_bcast_engine_bitrate(void)      { return mercury_engine_modem_bitrate(); }
int mercury_bcast_engine_bandwidth_hz(void) { return mercury_engine_modem_bandwidth_hz(); }

/* ---- Broadcast file receiving ------------------------------------------- */

void *mercury_bcast_rx_open(int mode, const char *dir, char *err, int errlen)
{
    return bcast_file_rx_open(mode, dir, err, (size_t)errlen);
}

int mercury_bcast_rx_frame(void *rx, const unsigned char *frame, int len)
{
    return (int)bcast_file_rx_frame((bcast_file_rx_t *)rx, frame, (size_t)len);
}

const char *mercury_bcast_rx_last_path(void *rx)
{ return bcast_file_rx_last_path((bcast_file_rx_t *)rx); }
const char *mercury_bcast_rx_last_name(void *rx)
{ return bcast_file_rx_last_name((bcast_file_rx_t *)rx); }
const char *mercury_bcast_rx_error(void *rx)
{ return bcast_file_rx_error((bcast_file_rx_t *)rx); }

void mercury_bcast_rx_stats(void *rx, unsigned long long *symbols, long *expect_bytes)
{
    uint64_t sym = 0; size_t want = 0;
    bcast_file_rx_stats((bcast_file_rx_t *)rx, &sym, &want);
    if (symbols)      *symbols      = (unsigned long long)sym;
    if (expect_bytes) *expect_bytes = (long)want;
}

void mercury_bcast_rx_close(void *rx) { bcast_file_rx_close((bcast_file_rx_t *)rx); }
