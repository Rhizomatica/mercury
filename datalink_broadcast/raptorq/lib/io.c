#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* The mmap-backed ioctx below is POSIX-only.  Mercury ships a Windows build,
 * and mingw has no <sys/mman.h>, so that whole backend is compiled out there.
 * Nothing in Mercury uses it: broadcast files are capped small enough to hold
 * in memory, so we use ioctx_from_mem_ro().  ioctx_from_file() and the memory
 * contexts above are plain stdio and portable as-is. */
#ifndef _WIN32
#include <sys/mman.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
/* mingw's <unistd.h> defines ftruncate() as an inline that calls _chsize(),
 * declared in the SYSTEM <io.h>.  We cannot include that here: nanorq ships its
 * own include/io.h, which is on the include path and shadows it, so <io.h>
 * resolves to nanorq's header and _chsize stays undeclared.  Declaring the
 * prototype directly avoids the collision without renaming nanorq's header. */
int _chsize(int _FileHandle, long _Size);
#endif
#include <unistd.h>

#include "io.h"

struct fileioctx {
  struct ioctx io;
  FILE *fp;
};

static size_t fileio_read(struct ioctx *io, uint8_t *buf, size_t len) {
  struct fileioctx *_io = (struct fileioctx *)io;
  if (!buf)
    return 0;
  return fread(buf, 1, len, _io->fp);
}

static size_t fileio_write(struct ioctx *io, const uint8_t *buf, size_t len) {
  struct fileioctx *_io = (struct fileioctx *)io;
  if (!io->writable || !buf)
    return 0;
  return fwrite(buf, 1, len, _io->fp);
}

static bool fileio_seek(struct ioctx *io, const size_t offset) {
  struct fileioctx *_io = (struct fileioctx *)io;
  off_t target = (off_t)offset;
  if (target < 0 || (size_t)target != offset)
    return false;
  return fseeko(_io->fp, target, SEEK_SET) == 0;
}

static long fileio_tell(struct ioctx *io) {
  struct fileioctx *_io = (struct fileioctx *)io;
  off_t pos = ftello(_io->fp);
  return pos > LONG_MAX ? LONG_MAX : (long)pos;
}

static void fileio_destroy(struct ioctx *io) {
  struct fileioctx *_io = (struct fileioctx *)io;
  fclose(_io->fp);
  free(_io);
  return;
}

static size_t fileio_size(struct ioctx *io) {
  struct fileioctx *_io = (struct fileioctx *)io;
  off_t pos = ftello(_io->fp);
  if (pos < 0 || fseeko(_io->fp, 0, SEEK_END) != 0)
    return 0;
  off_t end = ftello(_io->fp);
  if (fseeko(_io->fp, pos, SEEK_SET) != 0 || end < 0 ||
      (off_t)(size_t)end != end)
    return 0;
  return (size_t)end;
}

struct ioctx *ioctx_from_file(const char *fn, int mode) {
  struct fileioctx *_io = NULL;
  FILE *fp;

  if (!fn || (mode != IOCTX_MODE_READ && mode != IOCTX_MODE_WRITE))
    return NULL;
  if (mode == IOCTX_MODE_READ) {
    fp = fopen(fn, "rb");
  } else {
    fp = fopen(fn, "w+b"); // create decoder
  }

  if (!fp)
    return NULL;

  _io = (struct fileioctx *)calloc(1, sizeof(struct fileioctx));
  if (!_io) {
    fclose(fp);
    return NULL;
  }
  _io->fp = fp;

  _io->io.read = fileio_read;
  _io->io.write = fileio_write;
  _io->io.seek = fileio_seek;
  _io->io.size = fileio_size;
  _io->io.tell = fileio_tell;
  _io->io.destroy = fileio_destroy;
  _io->io.seekable = true;
  _io->io.writable = (mode == IOCTX_MODE_WRITE);

  return (struct ioctx *)_io;
}

struct memioctx {
  struct ioctx io;
  uint8_t *ptr;
  const uint8_t *ro_ptr;
  size_t pos;
  size_t size;
};

static size_t memio_read(struct ioctx *io, uint8_t *buf, size_t len) {
  struct memioctx *_io = (struct memioctx *)io;
  if (!buf)
    return 0;
  if (_io->pos >= _io->size)
    return 0;

  size_t available = _io->size - _io->pos;
  size_t to_read = (len > available) ? available : len;

  const uint8_t *src = _io->io.writable ? _io->ptr : _io->ro_ptr;
  memcpy(buf, src + _io->pos, to_read);
  _io->pos += to_read;
  return to_read;
}

static size_t memio_write(struct ioctx *io, const uint8_t *buf, size_t len) {
  struct memioctx *_io = (struct memioctx *)io;
  if (!_io->io.writable || !buf)
    return 0;
  if (_io->pos >= _io->size)
    return 0;

  size_t available = _io->size - _io->pos;
  size_t to_write = (len > available) ? available : len;

  memcpy(_io->ptr + _io->pos, buf, to_write);
  _io->pos += to_write;
  return to_write;
}

static bool memio_seek(struct ioctx *io, const size_t offset) {
  struct memioctx *_io = (struct memioctx *)io;
  if (offset > _io->size)
    return false;
  _io->pos = offset;
  return true;
}

static long memio_tell(struct ioctx *io) {
  struct memioctx *_io = (struct memioctx *)io;
  return _io->pos > LONG_MAX ? LONG_MAX : (long)_io->pos;
}

static void memio_destroy(struct ioctx *io) {
  struct memioctx *_io = (struct memioctx *)io;
  free(_io);
  return;
}

static size_t memio_size(struct ioctx *io) {
  struct memioctx *_io = (struct memioctx *)io;
  return _io->size;
}

struct ioctx *ioctx_from_mem(uint8_t *ptr, size_t sz) {
  if (!ptr)
    return NULL;
  struct memioctx *_io = (struct memioctx *)calloc(1, sizeof(struct memioctx));
  if (!_io)
    return NULL;

  _io->ptr = ptr;
  _io->ro_ptr = ptr;
  _io->pos = 0;
  _io->size = sz;

  _io->io.read = memio_read;
  _io->io.write = memio_write;
  _io->io.seek = memio_seek;
  _io->io.size = memio_size;
  _io->io.tell = memio_tell;
  _io->io.destroy = memio_destroy;
  _io->io.seekable = true;
  _io->io.writable = true;

  return (struct ioctx *)_io;
}

struct ioctx *ioctx_from_mem_ro(const uint8_t *ptr, size_t sz) {
  if (!ptr)
    return NULL;
  struct memioctx *_io = (struct memioctx *)calloc(1, sizeof(struct memioctx));
  if (!_io)
    return NULL;

  _io->ptr = NULL;
  _io->ro_ptr = ptr;
  _io->pos = 0;
  _io->size = sz;

  _io->io.read = memio_read;
  _io->io.write = memio_write;
  _io->io.seek = memio_seek;
  _io->io.size = memio_size;
  _io->io.tell = memio_tell;
  _io->io.destroy = memio_destroy;
  _io->io.seekable = true;
  _io->io.writable = false;

  return (struct ioctx *)_io;
}

#ifndef _WIN32

struct mmapioctx {
  struct ioctx io;
  int fd;
  uint8_t *ptr;
  size_t filesize;
  size_t mapsize;
  size_t offset;
  size_t pos;
  size_t lastmap;
};

static uint8_t *mmapio_mmap(size_t mapsize, bool writable, int fd,
                            size_t offset) {
  uint8_t *ptr = NULL;

  if (writable) {
    ptr = (uint8_t *)mmap(NULL, mapsize, PROT_WRITE | PROT_READ, MAP_SHARED, fd,
                          offset);
  } else {
    ptr = (uint8_t *)mmap(NULL, mapsize, PROT_READ, MAP_SHARED, fd, offset);
  }

  if (ptr == MAP_FAILED) {
    return NULL;
  }
  return ptr;
}

static bool mmapio_seek(struct ioctx *io, const size_t offset) {
  struct mmapioctx *_io = (struct mmapioctx *)io;

  if (!_io->io.writable && offset == _io->filesize) {
    _io->pos = offset;
    return true;
  }

  if (offset >= _io->offset && offset - _io->offset < _io->mapsize) {
    _io->pos = offset;
    return true;
  }

  if (_io->io.writable) {
    if (offset < _io->offset || offset < _io->filesize) {
      size_t new_offset = (offset / _io->mapsize) * _io->mapsize;
      uint8_t *new_ptr = mmapio_mmap(_io->mapsize, true, _io->fd, new_offset);
      if (!new_ptr)
        return false;
      if (_io->ptr)
        munmap(_io->ptr, _io->mapsize);
      _io->ptr = new_ptr;
      _io->offset = new_offset;
      _io->pos = offset;
      return true;
    } else {
      if (offset > SIZE_MAX - _io->mapsize)
        return false;
      size_t new_offset = (offset / _io->mapsize) * _io->mapsize;
      size_t map_end = new_offset + _io->mapsize;
      off_t new_size = (off_t)map_end;
      if (new_size < 0 || (size_t)new_size != map_end ||
          ftruncate(_io->fd, new_size) != 0)
        return false;
      uint8_t *new_ptr = mmapio_mmap(_io->mapsize, true, _io->fd, new_offset);
      if (!new_ptr)
        return false;
      if (_io->ptr)
        munmap(_io->ptr, _io->mapsize);
      _io->ptr = new_ptr;
      _io->offset = new_offset;
      _io->pos = offset;
      return true;
    }
  }

  if (!_io->io.writable && offset >= _io->filesize)
    return false;

  if (!_io->io.writable) {
    size_t new_offset = (offset / _io->mapsize) * _io->mapsize;
    size_t tmp = _io->mapsize;
    if (tmp > _io->filesize - new_offset)
      tmp = _io->filesize - new_offset;
    uint8_t *new_ptr = mmapio_mmap(tmp, false, _io->fd, new_offset);
    if (!new_ptr)
      return false;
    if (_io->ptr)
      munmap(_io->ptr, _io->lastmap);
    _io->ptr = new_ptr;
    _io->offset = new_offset;
    _io->pos = offset;
    _io->lastmap = tmp;
    return true;
  }

  return false;
}

static size_t mmapio_read(struct ioctx *io, uint8_t *buf, size_t len) {
  struct mmapioctx *_io = (struct mmapioctx *)io;
  size_t read_bytes = 0;
  if (!buf || !_io->ptr)
    return 0;

  while (read_bytes < len) {
    if (_io->pos >= _io->filesize)
      break;

    size_t at = _io->pos % _io->mapsize;
    size_t avail = _io->lastmap - at;

    size_t to_read = (len - read_bytes < avail) ? (len - read_bytes) : avail;

    if (to_read > 0) {
      memcpy(buf + read_bytes, _io->ptr + at, to_read);
      read_bytes += to_read;
      _io->pos += to_read;
    }

    if (read_bytes < len && _io->pos < _io->filesize) {
      if (!mmapio_seek(io, _io->offset + _io->mapsize))
        break;
    }
  }
  return read_bytes;
}

static size_t mmapio_write(struct ioctx *io, const uint8_t *buf, size_t len) {
  struct mmapioctx *_io = (struct mmapioctx *)io;
  size_t written = 0;

  if (!_io->io.writable || !buf || !_io->ptr)
    return 0;

  while (written < len) {
    size_t at = _io->pos % _io->mapsize;
    size_t avail = _io->mapsize - at;

    size_t to_write = (len - written < avail) ? (len - written) : avail;

    if (to_write > 0) {
      memcpy(_io->ptr + at, buf + written, to_write);
      written += to_write;
      _io->pos += to_write;
      if (_io->pos > _io->filesize) {
        _io->filesize = _io->pos;
      }
    }

    if (written < len) {
      if (!mmapio_seek(io, _io->offset + _io->mapsize))
        break;
    }
  }

  return written;
}

static long mmapio_tell(struct ioctx *io) {
  struct mmapioctx *_io = (struct mmapioctx *)io;
  return _io->pos > LONG_MAX ? LONG_MAX : (long)_io->pos;
}

static void mmapio_destroy(struct ioctx *io) {
  struct mmapioctx *_io = (struct mmapioctx *)io;
  if (_io->ptr) {
    munmap(_io->ptr, _io->lastmap);
  }
  if (_io->io.writable) {
    ftruncate(_io->fd, _io->filesize);
  }
  close(_io->fd);
  free(_io);
  return;
}

static size_t mmapio_size(struct ioctx *io) {
  struct mmapioctx *_io = (struct mmapioctx *)io;
  return _io->filesize;
}

struct ioctx *ioctx_mmap_file(const char *fn, int mode) {
  struct mmapioctx *_io = NULL;
  int fd;
  uint8_t *ptr = NULL;
  size_t filesize = 0;
  size_t offset = 0;
  if (!fn || (mode != IOCTX_MODE_READ && mode != IOCTX_MODE_WRITE))
    return NULL;
  long page_size_result = sysconf(_SC_PAGESIZE);
  if (page_size_result <= 0)
    return NULL;
  size_t pagesize = (size_t)page_size_result;
  size_t mapsize = (65536 / pagesize) * pagesize;
  if (mapsize == 0)
    mapsize = pagesize; // safe fallback

  if (mode == IOCTX_MODE_READ) {
    fd = open(fn, O_RDONLY);
  } else {
    fd = open(fn, O_RDWR | O_CREAT | O_TRUNC, 0666); // create decoder
  }

  if (fd == -1) {
    return NULL;
  }

  if (mode == IOCTX_MODE_READ) {
    struct stat sb;
    if (fstat(fd, &sb) != 0 || sb.st_size < 0) {
      close(fd);
      return NULL;
    }
    filesize = (size_t)sb.st_size;
    if ((off_t)filesize != sb.st_size) {
      close(fd);
      return NULL;
    }
    if (filesize > 0) {
      if (filesize < mapsize)
        mapsize = filesize;
      ptr = mmapio_mmap(mapsize, false, fd, offset);
    }
  } else {
    if (ftruncate(fd, mapsize) != 0) {
      close(fd);
      return NULL;
    }
    ptr = mmapio_mmap(mapsize, true, fd, offset);
  }

  if (!ptr && (mode == IOCTX_MODE_WRITE || filesize > 0)) {
    close(fd);
    return NULL;
  }

  _io = (struct mmapioctx *)calloc(1, sizeof(struct mmapioctx));
  if (!_io) {
    if (ptr)
      munmap(ptr, mapsize);
    close(fd);
    return NULL;
  }

  _io->fd = fd;
  _io->ptr = ptr;
  _io->filesize = filesize;
  _io->mapsize = mapsize;
  _io->lastmap = mapsize;
  _io->offset = offset;
  _io->pos = offset;

  _io->io.read = mmapio_read;
  _io->io.write = mmapio_write;
  _io->io.seek = mmapio_seek;
  _io->io.size = mmapio_size;
  _io->io.tell = mmapio_tell;
  _io->io.destroy = mmapio_destroy;
  _io->io.seekable = true;
  _io->io.writable = (mode == IOCTX_MODE_WRITE);

  return (struct ioctx *)_io;
}

#else /* _WIN32 */

struct ioctx *ioctx_mmap_file(const char *fn, int mode) {
  (void)fn; (void)mode;
  return NULL;   /* no mmap on Windows; use ioctx_from_file/mem */
}

#endif /* _WIN32 */
