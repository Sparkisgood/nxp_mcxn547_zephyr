/* SPDX-License-Identifier: Apache-2.0 */

#include "pmi_update.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include "psram.h"

LOG_MODULE_REGISTER(pmi_update, LOG_LEVEL_INF);

#define PMI_IMAGE_OFFSET 0U
#define PMI_SLOT_ID FIXED_PARTITION_ID(slot1_partition)
#define PMI_SLOT_SIZE FIXED_PARTITION_SIZE(slot1_partition)
#define PMI_MAX_IMAGE_SIZE (PMI_SLOT_SIZE - (8U * 1024U))
#define PMI_TRANSFER_CHUNK 1024U
#define PMI_IMAGE_MAGIC 0x96f3b83dU
#define PMI_ACTION_MAX_SIZE 32U

enum pmi_operation_state {
	PMI_STATE_IDLE,
	PMI_STATE_UPLOADING,
	PMI_STATE_READY,
	PMI_STATE_QUEUED,
	PMI_STATE_UPDATING,
	PMI_STATE_ERROR,
};

struct pmi_image_header {
	uint32_t magic;
	uint32_t load_address;
	uint16_t header_size;
	uint16_t protected_tlv_size;
	uint32_t image_size;
	uint32_t flags;
	uint8_t version[8];
	uint32_t pad;
} __packed;

struct pmi_update_status {
	enum pmi_operation_state upload_state;
	enum pmi_operation_state update_state;
	size_t image_size;
	size_t written;
	uint32_t upload_crc;
	uint32_t percent;
	char error[40];
};

static struct pmi_update_status update_status;
static struct k_mutex update_lock;
static struct k_sem update_request;
static uint8_t transfer_buffer[PMI_TRANSFER_CHUNK];
static char action_buffer[PMI_ACTION_MAX_SIZE + 1U];
static size_t action_length;
static char image_response[256];
static char update_response[256];

static const char *state_name(enum pmi_operation_state state)
{
	switch (state) {
	case PMI_STATE_UPLOADING:
		return "uploading";
	case PMI_STATE_READY:
		return "ready";
	case PMI_STATE_QUEUED:
		return "queued";
	case PMI_STATE_UPDATING:
		return "updating";
	case PMI_STATE_ERROR:
		return "error";
	case PMI_STATE_IDLE:
	default:
		return "idle";
	}
}

static bool update_is_busy(void)
{
	return update_status.update_state == PMI_STATE_QUEUED ||
	       update_status.update_state == PMI_STATE_UPDATING;
}

static void set_error(const char *message)
{
	update_status.update_state = PMI_STATE_ERROR;
	strncpy(update_status.error, message, sizeof(update_status.error) - 1U);
	update_status.error[sizeof(update_status.error) - 1U] = '\0';
}

static int verify_staged_image(void)
{
	struct pmi_image_header header;
	uint32_t crc = 0U;
	size_t offset = 0U;
	int ret;

	if (update_status.image_size < sizeof(header)) {
		return -EINVAL;
	}

	ret = psram_read(PMI_IMAGE_OFFSET, &header, sizeof(header));
	if (ret != 0) {
		return ret;
	}
	if (header.magic != PMI_IMAGE_MAGIC || header.header_size < sizeof(header) ||
	    (size_t)header.header_size + header.image_size > update_status.image_size) {
		return -EBADMSG;
	}

	while (offset < update_status.image_size) {
		size_t chunk = MIN((size_t)PMI_TRANSFER_CHUNK,
				   update_status.image_size - offset);

		ret = psram_read(PMI_IMAGE_OFFSET + offset, transfer_buffer, chunk);
		if (ret != 0) {
			return ret;
		}
		crc = crc32_ieee_update(crc, transfer_buffer, chunk);
		offset += chunk;
	}

	return crc == update_status.upload_crc ? 0 : -EILSEQ;
}

static int verify_slot_crc(size_t image_size, uint32_t expected_crc)
{
	const struct flash_area *area;
	uint32_t crc = 0U;
	size_t offset = 0U;
	int ret = flash_area_open(PMI_SLOT_ID, &area);

	if (ret != 0) {
		return ret;
	}

	while (offset < image_size) {
		size_t chunk = MIN((size_t)PMI_TRANSFER_CHUNK, image_size - offset);

		ret = flash_area_read(area, offset, transfer_buffer, chunk);
		if (ret != 0) {
			break;
		}
		crc = crc32_ieee_update(crc, transfer_buffer, chunk);
		offset += chunk;
	}
	flash_area_close(area);

	if (ret == 0 && crc != expected_crc) {
		ret = -EILSEQ;
	}
	return ret;
}

static void perform_update(void)
{
	struct flash_img_context context;
	size_t image_size;
	uint32_t expected_crc;
	size_t offset = 0U;
	int ret;

	k_mutex_lock(&update_lock, K_FOREVER);
	update_status.update_state = PMI_STATE_UPDATING;
	update_status.written = 0U;
	update_status.percent = 0U;
	update_status.error[0] = '\0';
	image_size = update_status.image_size;
	expected_crc = update_status.upload_crc;
	k_mutex_unlock(&update_lock);

	ret = verify_staged_image();
	if (ret != 0) {
		k_mutex_lock(&update_lock, K_FOREVER);
		set_error("staged_image_invalid");
		k_mutex_unlock(&update_lock);
		return;
	}

	ret = flash_img_init_id(&context, PMI_SLOT_ID);
	while (ret == 0 && offset < image_size) {
		size_t chunk = MIN((size_t)PMI_TRANSFER_CHUNK, image_size - offset);
		bool final = offset + chunk == image_size;

		ret = psram_read(PMI_IMAGE_OFFSET + offset, transfer_buffer, chunk);
		if (ret == 0) {
			ret = flash_img_buffered_write(&context, transfer_buffer, chunk, final);
		}
		if (ret == 0) {
			offset += chunk;
			k_mutex_lock(&update_lock, K_FOREVER);
			update_status.written = offset;
			update_status.percent = (uint32_t)((offset * 100U) / image_size);
			k_mutex_unlock(&update_lock);
		}
		k_yield();
	}

	if (ret == 0) {
		ret = verify_slot_crc(image_size, expected_crc);
	}
	if (ret == 0) {
		ret = boot_request_upgrade(false);
	}

	k_mutex_lock(&update_lock, K_FOREVER);
	if (ret == 0) {
		update_status.update_state = PMI_STATE_READY;
		update_status.percent = 100U;
		LOG_INF("PMI image ready in MCUboot slot 1 (%u bytes)", image_size);
	} else {
		set_error("slot_update_failed");
		LOG_ERR("PMI image update failed (%d)", ret);
	}
	k_mutex_unlock(&update_lock);
}

static void update_thread(void)
{
	while (true) {
		k_sem_take(&update_request, K_FOREVER);
		perform_update();
	}
}

K_THREAD_DEFINE(pmi_update_thread, 4096, update_thread, NULL, NULL, NULL,
		K_PRIO_PREEMPT(5), 0, SYS_FOREVER_MS);

static void set_json_response(struct http_response_ctx *response,
			      enum http_status status, const char *body, size_t length)
{
	response->status = status;
	response->body = body;
	response->body_len = length;
	response->final_chunk = true;
}

static size_t format_image_response(void)
{
	int length = snprintf(image_response, sizeof(image_response),
		"{\"status\":\"%s\",\"size\":%u,\"percent\":%u,"
		"\"crc32\":\"0x%08" PRIX32 "\",\"error\":\"%s\"}\n",
		state_name(update_status.upload_state), update_status.image_size,
		update_status.upload_state == PMI_STATE_READY ? 100U : 0U,
		update_status.upload_crc, update_status.error);

	return length < 0 ? 0U : MIN((size_t)length, sizeof(image_response) - 1U);
}

static size_t format_update_response(void)
{
	int length = snprintf(update_response, sizeof(update_response),
		"{\"status\":\"%s\",\"size\":%u,\"written\":%u,"
		"\"percent\":%u,\"reboot_required\":%s,\"error\":\"%s\"}\n",
		state_name(update_status.update_state), update_status.image_size,
		update_status.written, update_status.percent,
		update_status.update_state == PMI_STATE_READY ? "true" : "false",
		update_status.error);

	return length < 0 ? 0U : MIN((size_t)length, sizeof(update_response) - 1U);
}

static int image_handler(struct http_client_ctx *client,
			 enum http_data_status data_status,
			 const struct http_request_ctx *request,
			 struct http_response_ctx *response, void *user_data)
{
	int ret = 0;

	ARG_UNUSED(client);
	ARG_UNUSED(user_data);

	k_mutex_lock(&update_lock, K_FOREVER);
	if (data_status == HTTP_SERVER_DATA_ABORTED) {
		update_status.upload_state = PMI_STATE_ERROR;
		strcpy(update_status.error, "upload_aborted");
		k_mutex_unlock(&update_lock);
		return 0;
	}
	if (update_is_busy()) {
		size_t length = format_image_response();

		set_json_response(response, HTTP_409_CONFLICT, image_response, length);
		k_mutex_unlock(&update_lock);
		return 0;
	}
	if (update_status.upload_state != PMI_STATE_UPLOADING) {
		update_status.upload_state = PMI_STATE_UPLOADING;
		update_status.update_state = PMI_STATE_IDLE;
		update_status.image_size = 0U;
		update_status.written = 0U;
		update_status.percent = 0U;
		update_status.upload_crc = 0U;
		update_status.error[0] = '\0';
	}

	if (update_status.image_size > PMI_MAX_IMAGE_SIZE ||
	    request->data_len > PMI_MAX_IMAGE_SIZE - update_status.image_size ||
	    update_status.image_size > psram_get_size() ||
	    request->data_len > psram_get_size() - update_status.image_size) {
		update_status.upload_state = PMI_STATE_ERROR;
		strcpy(update_status.error, "image_too_large");
		ret = -EFBIG;
	} else if (request->data_len > 0U) {
		ret = psram_write(PMI_IMAGE_OFFSET + update_status.image_size,
				  request->data, request->data_len);
		if (ret == 0) {
			update_status.upload_crc = crc32_ieee_update(
				update_status.upload_crc, request->data, request->data_len);
			update_status.image_size += request->data_len;
		} else {
			update_status.upload_state = PMI_STATE_ERROR;
			strcpy(update_status.error, "psram_write_failed");
		}
	}

	if (data_status == HTTP_SERVER_DATA_FINAL) {
		if (ret == 0 && update_status.image_size > 0U) {
			ret = verify_staged_image();
			if (ret == 0) {
				update_status.upload_state = PMI_STATE_READY;
			} else {
				update_status.upload_state = PMI_STATE_ERROR;
				strcpy(update_status.error, "invalid_mcuboot_image");
			}
		} else if (update_status.image_size == 0U) {
			update_status.upload_state = PMI_STATE_ERROR;
			strcpy(update_status.error, "empty_image");
		}

		set_json_response(response,
			update_status.upload_state == PMI_STATE_READY ? HTTP_200_OK :
			HTTP_400_BAD_REQUEST,
			image_response, format_image_response());
	}
	k_mutex_unlock(&update_lock);

	return 0;
}

static int update_handler(struct http_client_ctx *client,
			  enum http_data_status data_status,
			  const struct http_request_ctx *request,
			  struct http_response_ctx *response, void *user_data)
{
	enum http_status response_status = HTTP_200_OK;

	ARG_UNUSED(client);
	ARG_UNUSED(user_data);

	if (data_status == HTTP_SERVER_DATA_ABORTED) {
		action_length = 0U;
		return 0;
	}
	if (request->data_len > sizeof(action_buffer) - 1U - action_length) {
		static const char too_large[] =
			"{\"status\":\"error\",\"error\":\"request_too_large\"}\n";

		action_length = 0U;
		set_json_response(response, HTTP_413_PAYLOAD_TOO_LARGE,
				  too_large, sizeof(too_large) - 1U);
		return 0;
	}
	memcpy(action_buffer + action_length, request->data, request->data_len);
	action_length += request->data_len;

	if (data_status != HTTP_SERVER_DATA_FINAL) {
		return 0;
	}
	action_buffer[action_length] = '\0';

	k_mutex_lock(&update_lock, K_FOREVER);
	if (action_length > 0U) {
		if (strcmp(action_buffer, "action=update") != 0) {
			set_error("invalid_action");
			response_status = HTTP_400_BAD_REQUEST;
		} else if (update_is_busy()) {
			response_status = HTTP_409_CONFLICT;
		} else if (update_status.upload_state != PMI_STATE_READY) {
			set_error("no_image");
			response_status = HTTP_409_CONFLICT;
		} else {
			update_status.update_state = PMI_STATE_QUEUED;
			update_status.written = 0U;
			update_status.percent = 0U;
			update_status.error[0] = '\0';
			k_sem_give(&update_request);
		}
	}
	action_length = 0U;
	set_json_response(response, response_status, update_response,
			  format_update_response());
	k_mutex_unlock(&update_lock);

	return 0;
}

static struct http_resource_detail_dynamic image_resource_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_POST),
		.content_type = "application/json",
	},
	.cb = image_handler,
};

HTTP_RESOURCE_DEFINE(pmi_image_resource, edison_http_service, "/pmi/image",
		     &image_resource_detail);

static struct http_resource_detail_dynamic update_resource_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_POST),
		.content_type = "application/json",
	},
	.cb = update_handler,
};

HTTP_RESOURCE_DEFINE(pmi_update_resource, edison_http_service, "/pmi/update",
		     &update_resource_detail);

int pmi_update_service_init(void)
{
	int ret;

	k_mutex_init(&update_lock);
	k_sem_init(&update_request, 0, 1);
	k_thread_start(pmi_update_thread);
	update_status.upload_state = PMI_STATE_IDLE;
	update_status.update_state = PMI_STATE_IDLE;

	if (!psram_is_ready()) {
		set_error("psram_not_ready");
		return -ENODEV;
	}

	if (!boot_is_img_confirmed()) {
		ret = boot_write_img_confirmed();
		if (ret != 0) {
			LOG_WRN("Could not confirm the running MCUboot image (%d)", ret);
		}
	}

	LOG_INF("PMI Ethernet update staging ready (%u-byte slot)", PMI_SLOT_SIZE);
	return 0;
}
