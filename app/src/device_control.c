/* SPDX-License-Identifier: Apache-2.0 */

#include "device_control.h"

#include <zephyr/kernel.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#define REBOOT_DELAY K_MSEC(500)

static struct k_work_delayable reboot_work;

static void reboot_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	sys_reboot(SYS_REBOOT_COLD);
}

static void schedule_reboot(void)
{
	k_work_reschedule(&reboot_work, REBOOT_DELAY);
}

static int cmd_device_reboot(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Cold reboot scheduled in 500 ms");
	schedule_reboot();
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(pmi_commands,
	SHELL_CMD(reboot, NULL, "Schedule a cold device reboot.",
		  cmd_device_reboot),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(pmi, &pmi_commands, "PMI device-control commands.", NULL);

static int reboot_post_handler(struct http_client_ctx *client,
			       enum http_data_status status,
			       const struct http_request_ctx *request,
			       struct http_response_ctx *response, void *user_data)
{
	static const char body[] =
		"{\"status\":\"ok\",\"action\":\"reboot\",\"delay_ms\":500}\n";

	ARG_UNUSED(client);
	ARG_UNUSED(request);
	ARG_UNUSED(user_data);

	if (status == HTTP_SERVER_DATA_ABORTED) {
		return 0;
	}
	if (status != HTTP_SERVER_DATA_FINAL) {
		return 0;
	}

	response->status = HTTP_200_OK;
	response->body = body;
	response->body_len = sizeof(body) - 1U;
	response->final_chunk = true;
	schedule_reboot();

	return 0;
}

static struct http_resource_detail_dynamic reboot_resource_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_POST),
		.content_type = "application/json",
	},
	.cb = reboot_post_handler,
};

HTTP_RESOURCE_DEFINE(reboot_resource, edison_http_service, "/pmi/reboot",
		     &reboot_resource_detail);

void device_control_service_init(void)
{
	k_work_init_delayable(&reboot_work, reboot_work_handler);
}
