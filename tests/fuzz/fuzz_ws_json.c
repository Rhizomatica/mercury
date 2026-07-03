/* libFuzzer target: WebSocket JSON command parser.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * parse_ws_command() and json_find_key() are static in mercury_websocket.c,
 * so ideally we include the translation unit directly (same technique as
 * tests/data_interfaces/test_tcp_interfaces.c). However, mercury_websocket.c
 * includes mongoose.h (a ~990 KB single-file HTTP/WS amalgamation) whose
 * functions are referenced throughout the file. Including the whole .c pulls
 * in those link dependencies without touching production code to split the
 * two JSON helpers out.
 *
 * This harness is DEFERRED until the JSON helpers are extracted to a separate
 * translation unit (e.g. gui_interface/websocket/ws_json.c) that can be
 * included without the mongoose link surface. See tests/fuzz/README.md for
 * the full deferral rationale.
 *
 * DO NOT add fuzz_ws_json to FUZZ_BINS in the Makefile until that split lands.
 */

/* Placeholder: nothing to compile yet. */
