#ifndef BENCHKIT_INTERNAL_H
#define BENCHKIT_INTERNAL_H

#include "benchkit.h"

#include <stdint.h>

#define BK_CLASS_LOSS_TOLERANT 0
#define BK_CLASS_MUST_DELIVER 1
#define BK_N_CLASSES 2

static inline int bk_class_index_from_flags(uint8_t flags) {
  return (flags & BK_FLAG_MUST_DELIVER) ? BK_CLASS_MUST_DELIVER
                                        : BK_CLASS_LOSS_TOLERANT;
}

static inline uint64_t bk_saturating_sub_u64(uint64_t a, uint64_t b) {
  return a >= b ? a - b : 0;
}

#endif  // BENCHKIT_INTERNAL_H
