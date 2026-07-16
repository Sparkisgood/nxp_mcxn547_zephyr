/* SPDX-License-Identifier: Apache-2.0 */

#ifndef EDISON_PSRAM_H_
#define EDISON_PSRAM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void psram_service_init(void);

/** Return true after the PSRAM controller and device have initialized. */
bool psram_is_ready(void);

/** Return the usable PSRAM capacity in bytes. */
size_t psram_get_size(void);

/** Read from PSRAM using a byte offset from the beginning of the device. */
int psram_read(uint32_t offset, void *data, size_t length);

/** Write to PSRAM using a byte offset from the beginning of the device. */
int psram_write(uint32_t offset, const void *data, size_t length);

#endif /* EDISON_PSRAM_H_ */
