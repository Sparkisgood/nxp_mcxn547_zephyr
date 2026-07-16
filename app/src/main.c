/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/app_version.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include "device_control.h"
#include "ethernet.h"
#include "pmi_update.h"
#include "psram.h"
#include "web_server.h"

#define SLEEP_TIME_MS 500

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("Application version: %s", APP_VERSION_STRING);

	device_control_service_init();
	psram_service_init();
	pmi_update_service_init();
	ethernet_service_init();
	web_server_service_init();

	while (1) {
		k_msleep(SLEEP_TIME_MS);
	}

	return 0;
}
