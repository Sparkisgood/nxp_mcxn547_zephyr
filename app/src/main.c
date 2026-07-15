/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "ethernet.h"

#define SLEEP_TIME_MS 500

int main(void)
{
	ethernet_service_init();

	while (1) {
		k_msleep(SLEEP_TIME_MS);
	}

	return 0;
}
