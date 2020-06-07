#ifndef _RADIO_UTILS_H_
#define _RADIO_UTILS_H_

#include <stdint.h>

#define SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

uint16_t convert(const char *num);

#endif  // _RADIO_UTILS_H_
