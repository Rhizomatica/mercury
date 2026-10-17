/* Mercury linked into a host program
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The daemon build (`make libmercury_host.a MERCURY_HOST=daemon`) leaves out
 * Mercury's own audio, PTT and websocket server; the program that links it in
 * (hermes-radio-daemon) owns the radio and supplies them through this header.
 * Lifecycle is the bridge's (gui_interface/fyne-ui/engine/mercury_bridge.h),
 * declared below:
 *
 *     mercury_host_set_radio(&ops);              before mercury_init
 *     mercury_host_set_ui(&ui_ops);              optional, any time
 *     mercury_init(argc, argv, "mercury.ini", NULL);
 *     ...  mercury_host_audio_exchange() on every audio block  ...
 *     mercury_shutdown();
 *
 * mercury_init returns non-zero on failure (a busy TCP port, a bad config)
 * and stops only Mercury.  mercury.ini's audio and PTT settings are ignored
 * in this build: audio is the exchange below, PTT is ops->key.
 */

#ifndef MERCURY_HOST_H_
#define MERCURY_HOST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- lifecycle -----------------------------------------------------------
 *
 * The bridge's calls (mercury_bridge.c), declared here too so a host need not
 * include mercury_bridge.h and the engine headers behind it.
 *
 * mercury_init: argv as on the command line, e.g. {"mercury", "-C",
 * "/etc/mercury/mercury.ini"}; default_config is used when there is no -C,
 * log_path when there is no -L (NULL: no log file; the log always goes to
 * stderr).  Returns once the engine is up: 0, MERCURY_INIT_CONFIG_ERROR when
 * the configuration file cannot be read in full, or -1.  In a TX/RX test (-t,
 * -r) it returns only after mercury_request_shutdown(), so call it on a
 * thread of its own there.
 *
 * mercury_request_shutdown: sets Mercury's run flag; safe from any thread.
 * mercury_shutdown: stops Mercury and joins its threads, PTT released. */
#ifndef MERCURY_INIT_CONFIG_ERROR
#define MERCURY_INIT_CONFIG_ERROR (-2)
#endif
int  mercury_init(int argc, char **argv, const char *default_config, const char *log_path);
void mercury_request_shutdown(void);
void mercury_shutdown(void);

/* ---- radio ---------------------------------------------------------------
 *
 * key(true) at the start of each transmission, key(false) at its end, from
 * Mercury's TX thread (and once more at shutdown); it may block as long as
 * the radio takes.  Mercury keys, waits its configured TX delay, writes the
 * frame, waits the frame's airtime plus a 100 ms tail, then unkeys: so unkey
 * at once, with no drain of your own on top.  Returns 0 when the radio did
 * what was asked.
 *
 * get_frequency is polled by the UI publisher: return the cached dial
 * frequency without blocking, false if there is none. */
typedef struct {
    int  (*key)(bool tx, void *user);
    bool (*get_frequency)(uint64_t *hz, void *user);
    void *user;
} mercury_host_radio_ops;

/* Copied.  Call before mercury_init; NULL clears (no PTT). */
void mercury_host_set_radio(const mercury_host_radio_ops *ops);

/* ---- audio ---------------------------------------------------------------
 *
 * 8 kHz mono S16, the rtp_wire.h contract.  Call once per audio block on the
 * host's audio clock, keyed or not: TX drains only through this call.
 *   rx8k  n samples heard, put in Mercury's capture ring; NULL when the
 *         host has nothing (e.g. RX muted while keyed).
 *   tx8k  receives n samples: Mercury's TX audio, then silence.
 * Returns how many of the n were Mercury's audio.  Never waits: it takes the
 * rings' mutex briefly, and gives silence while Mercury is not running. */
size_t mercury_host_audio_exchange(const int16_t *rx8k, int16_t *tx8k, size_t n);

/* ---- UI ------------------------------------------------------------------
 *
 * What Mercury's websocket server would send to its clients: status JSON
 * (~1 Hz and on events) and the binary spectrum frames.  Called from
 * Mercury's publisher threads.  With no ops set they are dropped. */
typedef struct {
    void (*broadcast_json)(const char *json, void *user);
    void (*broadcast_binary)(const void *data, size_t len, void *user);
    void *user;
} mercury_host_ui_ops;

/* Copied.  NULL clears. */
void mercury_host_set_ui(const mercury_host_ui_ops *ops);

/* A UI client's message, the websocket API's command JSON
 * (docs/WEBSOCKET_API.md).  Returns 0 when the command was carried out,
 * -1 when it was malformed or Mercury is not running, or the handler's
 * error code. */
int mercury_host_ui_command_json(const char *json, size_t len);

/* A UI client connected: Mercury sends it, through broadcast_json, the state
 * a client gets once (device lists, radio list, history). */
void mercury_host_ui_client_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* MERCURY_HOST_H_ */
