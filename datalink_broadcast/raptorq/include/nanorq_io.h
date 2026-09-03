#ifndef NANORQ_IOCTX_H
#define NANORQ_IOCTX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mode flags for IO contexts
#define IOCTX_MODE_WRITE 0
#define IOCTX_MODE_READ 1

struct ioctx {
  size_t (*read)(struct ioctx *, uint8_t *, size_t);
  size_t (*write)(struct ioctx *, const uint8_t *, size_t);
  bool (*seek)(struct ioctx *, const size_t);
  size_t (*size)(struct ioctx *);
  long (*tell)(struct ioctx *);
  void (*destroy)(struct ioctx *);
  bool seekable;
  bool writable;
};

// Create an IO context from a standard file
struct ioctx *ioctx_from_file(const char *fn, int mode);

// Create an IO context using memory-mapped file access
struct ioctx *ioctx_mmap_file(const char *fn, int mode);

// Create an IO context bound to a pre-allocated memory buffer (Read/Write)
struct ioctx *ioctx_from_mem(uint8_t *ptr, size_t sz);

// Create an IO context bound to a pre-allocated memory buffer (Read-Only)
struct ioctx *ioctx_from_mem_ro(const uint8_t *ptr, size_t sz);

#ifdef __cplusplus
}
#endif

#endif
