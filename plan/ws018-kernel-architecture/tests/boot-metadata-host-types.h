/* Host sys/types.h lacks zedBSD's thread identifier; retain its target width. */
#include <stdint.h>
typedef int32_t tid_t;
