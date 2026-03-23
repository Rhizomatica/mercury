# Conventions — Mercury (HERMES HF Modem)

## Code Style

- **Language standard**: GNU C11 (`-std=gnu11`)
- **Indentation**: 4 spaces (no tabs in source; Makefile uses tabs per convention)
- **Brace style**: K&R / Allman hybrid — opening brace on same line for functions, `if`, `while`, etc.
- **Line length**: No strict limit, but generally kept under ~120 characters
- **Comments**: C-style `/* ... */` for block/file headers, `//` for inline comments

## Naming Patterns

```c
// Functions: module_verb_noun
int arq_init(size_t frame_size, int mode);
void radio_io_key_on(void);
int ui_comm_init(ui_ctx_t *ctx, ...);

// Types: snake_case_t
typedef struct { ... } arq_info;
typedef struct generic_modem { ... } generic_modem_t;
typedef enum { ... } arq_action_type_t;

// Constants/Macros: UPPER_SNAKE_CASE
#define DEFAULT_ARQ_PORT 8300
#define FREEDV_MODE_DATAC3 ...

// Global mutable state: snake_case (extern in headers)
extern volatile bool shutdown_;
extern arq_info arq_conn;
```

## Header Guards

Standard `#ifndef` / `#define` / `#endif` pattern:
```c
#ifndef MODULE_H_
#define MODULE_H_
// ...
#endif
```

Some newer headers use `#pragma once` (e.g., `audioio.h`).

## Error Handling

- Functions return `int` (0 = success, non-zero = failure) or `-1` for errors
- `fprintf(stderr, ...)` for fatal errors before exit
- `HLOGE()` / `HLOGW()` macros for runtime errors/warnings via async logger
- No exceptions (pure C)
- Graceful shutdown via global `shutdown_` flag checked in polling loops

## Logging

Centralized async logging via `hermes_log.h`:

```c
HLOGD("module", "format %d", value);  // DEBUG
HLOGT("module", "format %d", value);  // TIMING
HLOGI("module", "format %d", value);  // INFO
HLOGW("module", "format %d", value);  // WARN
HLOGE("module", "format %d", value);  // ERROR
```

- First argument is always a component tag string (e.g., `"arq"`, `"main"`, `"bcast"`)
- Supports file output with JSONL format
- Lock-free ring buffer internally

## Threading Pattern

- Each subsystem creates its own threads via `pthread_create`
- Synchronization via `pthread_mutex_t` and typed message channels (`chan.h`, `queue.h`)
- Global shutdown: `volatile bool shutdown_` + signal handlers
- Thread-per-connection model for TCP servers

## Memory Management

- Manual `malloc`/`free` — no reference counting or GC
- Static allocation preferred (fixed-size buffers, compile-time sizes)
- Ring buffers for inter-thread communication (lock-free)
- `strncpy` used for safe string copies

## Build Pattern

Each subdirectory has its own `Makefile` that:
1. Includes `../config.mk` for shared compiler settings
2. Compiles `.c` → `.o` files
3. Top-level `Makefile` links all `.o` files into final binary

## Documentation

- Doxygen-style comments (`/** @brief ... */`) in public headers (especially `datalink_arq/`)
- Standalone docs in `docs/ARQ.md` and `docs/TNC.md`
- `make doxygen` generates HTML API documentation

## License Headers

All source files include GPLv3 copyright header:
```c
/* HERMES Modem
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
```
