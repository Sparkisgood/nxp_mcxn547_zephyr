/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

#include "ethernet.h"

#define SLEEP_TIME_MS 500
#define LED0_NODE DT_ALIAS(led0)

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

int main(void)
{
	int ret;
	bool led_on = true;

	if (!gpio_is_ready_dt(&led)) {
		printk("Error: LED device not ready\n");
		return 0;
	}

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		printk("Error: failed to configure LED pin (%d)\n", ret);
		return 0;
	}

	ethernet_service_init();

	while (1) {
		#if 0
		ret = gpio_pin_toggle_dt(&led);
		if (ret < 0) {
			printk("Error: failed to toggle LED pin (%d)\n", ret);
			return 0;
		}

		led_on = !led_on;
		printk("Hiblinky: LED is %s\n", led_on ? "ON" : "OFF");
		#endif
		k_msleep(SLEEP_TIME_MS);
	}

	return 0;
}
