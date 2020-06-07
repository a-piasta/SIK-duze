#include "utils.h"

#include <errno.h>
#include <stdlib.h>

uint16_t convert(const char *num) {
  if (*num == '\0') {
    errno = EINVAL;
    return 0;
  }
  char *endptr;
  unsigned long ret = strtoul(num, &endptr, 10);
  if (*endptr != '\0') {
    errno = EINVAL;
    return ret;
  }
  if (ret > UINT16_MAX) {
    ret = UINT16_MAX;
    errno = ERANGE;
  }
  return ret;
}
