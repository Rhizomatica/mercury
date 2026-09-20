/* UI pages compiled into the binary
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Implemented by web_packed.c, which pack_web.sh generates at build time from
 * websocket/web/ and which is never committed.  Packed rather than installed
 * because a path relative to the working directory only worked when mercury
 * was started from inside the source tree, and `make install` never shipped
 * the pages at all -- the browser got an empty directory index with nothing
 * to explain why (issue #204).
 */

#ifndef WEB_ASSETS_H_
#define WEB_ASSETS_H_

#include <stddef.h>

/* Look up a page by request path ("/index.html").  Returns NULL when absent. */
const void *ws_asset_find(const char *path, size_t *size);

/* Enumerate packed paths: index 0, 1, ... until NULL. */
const char *ws_asset_name(size_t no);

#endif /* WEB_ASSETS_H_ */
