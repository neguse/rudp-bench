#ifndef BENCHKIT_TEST_COMMON_H
#define BENCHKIT_TEST_COMMON_H

#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,       \
              #cond);                                                          \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

#endif  // BENCHKIT_TEST_COMMON_H
