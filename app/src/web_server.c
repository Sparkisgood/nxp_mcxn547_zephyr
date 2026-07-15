/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "web_server.h"

#include <inttypes.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/app_version.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#define HTTP_PORT 80

static uint16_t http_port = HTTP_PORT;

HTTP_SERVICE_DEFINE(edison_http_service, NULL, &http_port,
		    1, 1, NULL, NULL, NULL);

static int status_get_handler(struct http_client_ctx *client,
			      enum http_data_status status,
			      const struct http_request_ctx *request_ctx,
			      struct http_response_ctx *response_ctx,
			      void *user_data)
{
	static uint8_t response[96];
	int len;

	ARG_UNUSED(client);
	ARG_UNUSED(request_ctx);
	ARG_UNUSED(user_data);

	if (status != HTTP_SERVER_DATA_FINAL) {
		return 0;
	}

	len = snprintf(response, sizeof(response),
		       "{\"status\":\"ok\",\"version\":\"%s\","
		       "\"uptime_ms\":%" PRId64 "}\n",
		       APP_VERSION_STRING, k_uptime_get());
	if (len < 0) {
		return len;
	}

	response_ctx->status = HTTP_200_OK;
	response_ctx->body = response;
	response_ctx->body_len = MIN((size_t)len, sizeof(response) - 1U);
	response_ctx->final_chunk = true;

	return 0;
}

static struct http_resource_detail_dynamic status_resource_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_type = "application/json",
	},
	.cb = status_get_handler,
};

HTTP_RESOURCE_DEFINE(status_resource, edison_http_service, "/api/status",
		     &status_resource_detail);

static int echo_post_handler(struct http_client_ctx *client,
			     enum http_data_status status,
			     const struct http_request_ctx *request_ctx,
			     struct http_response_ctx *response_ctx,
			     void *user_data)
{
	ARG_UNUSED(client);
	ARG_UNUSED(user_data);

	if (status == HTTP_SERVER_DATA_ABORTED) {
		return 0;
	}

	response_ctx->status = HTTP_200_OK;
	response_ctx->body = request_ctx->data;
	response_ctx->body_len = request_ctx->data_len;
	response_ctx->final_chunk = (status == HTTP_SERVER_DATA_FINAL);

	return 0;
}

static struct http_resource_detail_dynamic echo_resource_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_POST),
		.content_type = "text/plain",
	},
	.cb = echo_post_handler,
};

HTTP_RESOURCE_DEFINE(echo_resource, edison_http_service, "/api/echo",
		     &echo_resource_detail);

int web_server_service_init(void)
{
	int ret = http_server_start();

	if (ret < 0) {
		printk("Failed to start HTTP server (%d)\n", ret);
		return ret;
	}

	printk("HTTP server listening on port %u\n", http_port);
	printk("  GET  /api/status\n");
	printk("  POST /api/echo\n");

	return 0;
}
