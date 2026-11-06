#ifndef WAV_H
#define WAV_H

#include <stddef.h>
#include <stdint.h>

/* Returns 0 on success, 1 when the WAV is absent, or -1 on invalid/oversized
 * input or I/O failure. The returned CWAV is allocated with malloc. */
int wav_to_cwav_file(const char *path, uint8_t **cwav, size_t *cwav_size,
                     char *err, size_t errlen);

#endif
