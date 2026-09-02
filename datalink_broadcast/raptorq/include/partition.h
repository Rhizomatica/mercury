#ifndef NANORQ_PARTITION_H
#define NANORQ_PARTITION_H

#include <stddef.h>
#include <stdint.h>

struct partition {
  size_t IL; /* size of long blocks */
  size_t IS; /* size of short blocks*/
  size_t JL; /* number of long blocks */
  size_t JS; /* number of short blocks */
};

struct partition partition_fill(size_t I, uint16_t J);

#endif /* NANORQ_PARTITION_H */
