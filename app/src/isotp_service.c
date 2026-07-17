/* SPDX-License-Identifier: Apache-2.0 */

#include "isotp_service.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/canbus/isotp.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(isotp_service, LOG_LEVEL_INF);

#define ISOTP_DATA_MAX 512U
#define ISOTP_HTTP_BODY_MAX 1800U
#define ISOTP_HTTP_RESPONSE_MAX 3200U
#define ISOTP_DEFAULT_TIMEOUT_MS 5000U

static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
static const struct gpio_dt_spec can_active =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), can_active_gpios);
static bool service_ready;
K_MUTEX_DEFINE(transaction_lock);
K_SEM_DEFINE(raw_rx_sem, 0, 1);
static uint8_t raw_rx_data[CAN_MAX_DLEN];
static size_t raw_rx_len;

static int parse_u32(const char *text, uint32_t *value)
{
	char *end;
	unsigned long parsed = strtoul(text, &end, 0);

	if (*text == '\0' || *end != '\0' || parsed > UINT32_MAX) {
		return -EINVAL;
	}
	*value = (uint32_t)parsed;
	return 0;
}

static int hex_nibble(char value)
{
	if (value >= '0' && value <= '9') {
		return value - '0';
	}
	value = (char)tolower((unsigned char)value);
	if (value >= 'a' && value <= 'f') {
		return value - 'a' + 10;
	}
	return -EINVAL;
}

static int parse_payload(const char *text, uint8_t *data, size_t capacity,
			 size_t *data_len)
{
	int high = -1;
	size_t length = 0;

	for (; *text != '\0'; text++) {
		int nibble;

		if (isspace((unsigned char)*text) || *text == ':' || *text == '-') {
			continue;
		}
		nibble = hex_nibble(*text);
		if (nibble < 0) {
			return -EINVAL;
		}
		if (high < 0) {
			high = nibble;
			continue;
		}
		if (length >= capacity) {
			return -E2BIG;
		}
		data[length++] = (uint8_t)((high << 4) | nibble);
		high = -1;
	}

	if (high >= 0 || length == 0U) {
		return -EINVAL;
	}
	*data_len = length;
	return 0;
}

static void strip_single_frame_pci(uint8_t *data, size_t *length)
{
	if (*length > 1U && (data[0] & 0xf0U) == 0U &&
	    (data[0] & 0x0fU) == *length - 1U) {
		*length -= 1U;
		memmove(data, data + 1U, *length);
	}
}

static struct isotp_msg_id make_address(uint32_t id)
{
	struct isotp_msg_id address = {.dl = CAN_MAX_DLEN};

	if (id > CAN_STD_ID_MASK) {
		address.ext_id = id;
		address.flags = ISOTP_MSG_IDE;
	} else {
		address.std_id = id;
	}
	return address;
}

static void raw_rx_callback(const struct device *dev, struct can_frame *frame,
			    void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	raw_rx_len = MIN(can_dlc_to_bytes(frame->dlc), sizeof(raw_rx_data));
	memcpy(raw_rx_data, frame->data, raw_rx_len);
	k_sem_give(&raw_rx_sem);
}

static int raw_can_loop(uint32_t can_id, const uint8_t *tx_data,
			uint8_t *rx_data, uint32_t timeout_ms)
{
	struct can_filter filter = {
		.id = can_id,
		.mask = can_id > CAN_STD_ID_MASK ? CAN_EXT_ID_MASK : CAN_STD_ID_MASK,
	};
	struct can_frame frame = {
		.id = can_id,
		.dlc = can_bytes_to_dlc(8U),
	};
	int filter_id;
	int ret;

	if (!service_ready) {
		return -ENODEV;
	}
	if (can_id > CAN_EXT_ID_MASK || timeout_ms == 0U) {
		return -EINVAL;
	}
	if (can_id > CAN_STD_ID_MASK) {
		filter.flags = CAN_FILTER_IDE;
		frame.flags = CAN_FRAME_IDE;
	}
	memcpy(frame.data, tx_data, 8U);

	k_mutex_lock(&transaction_lock, K_FOREVER);
	raw_rx_len = 0;
	k_sem_reset(&raw_rx_sem);
	filter_id = can_add_rx_filter(can_dev, raw_rx_callback, NULL, &filter);
	if (filter_id < 0) {
		ret = filter_id;
		goto out;
	}

	ret = can_send(can_dev, &frame, K_MSEC(timeout_ms), NULL, NULL);
	if (ret == 0) {
		ret = k_sem_take(&raw_rx_sem, K_MSEC(timeout_ms));
	}
	can_remove_rx_filter(can_dev, filter_id);
	if (ret == 0) {
		memcpy(rx_data, raw_rx_data, raw_rx_len);
		if (raw_rx_len != 8U || memcmp(tx_data, raw_rx_data, 8U) != 0) {
			ret = -EIO;
		}
	}

out:
	k_mutex_unlock(&transaction_lock);
	return ret;
}

static int isotp_request(uint32_t tx_id, uint32_t rx_id,
			 const uint8_t *request, size_t request_len,
			 uint8_t *response, size_t response_size,
			 uint32_t timeout_ms)
{
	static struct isotp_recv_ctx recv_ctx;
	static struct isotp_send_ctx send_ctx;
	const struct isotp_fc_opts fc_opts = {.bs = 0, .stmin = 0};
	struct isotp_msg_id tx_addr;
	struct isotp_msg_id rx_addr;
	int ret;

	if (!service_ready) {
		return -ENODEV;
	}
	if (tx_id > CAN_EXT_ID_MASK || rx_id > CAN_EXT_ID_MASK || tx_id == rx_id ||
	    request_len == 0U || request_len > ISOTP_DATA_MAX) {
		return -EINVAL;
	}

	tx_addr = make_address(tx_id);
	rx_addr = make_address(rx_id);

	k_mutex_lock(&transaction_lock, K_FOREVER);
	memset(&recv_ctx, 0, sizeof(recv_ctx));
	memset(&send_ctx, 0, sizeof(send_ctx));
	ret = isotp_bind(&recv_ctx, can_dev, &rx_addr, &tx_addr, &fc_opts,
			 K_MSEC(timeout_ms));
	if (ret == ISOTP_N_OK) {
		ret = isotp_send(&send_ctx, can_dev, request, request_len,
				 &tx_addr, &rx_addr, NULL, NULL);
		if (ret == ISOTP_N_OK) {
			ret = isotp_recv(&recv_ctx, response, response_size,
					 K_MSEC(timeout_ms));
		}
		isotp_unbind(&recv_ctx);
	}
	k_mutex_unlock(&transaction_lock);

	return ret;
}

static void print_hex(const struct shell *sh, const char *label,
		      const uint8_t *data, size_t length)
{
	shell_fprintf(sh, SHELL_NORMAL, "%s", label);
	for (size_t i = 0; i < length; i++) {
		shell_fprintf(sh, SHELL_NORMAL, "%02X%s", data[i],
			      i + 1U == length ? "\n" : " ");
	}
}

static int cmd_isotp_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "ISO-TP: %s", service_ready ? "ready" : "not ready");
	shell_print(sh, "CAN0: PIO1_10 TX, PIO1_11 RX, 250 kbit/s");
	return service_ready ? 0 : -ENODEV;
}

static int cmd_isotp_test(const struct shell *sh, size_t argc, char **argv)
{
	static uint8_t request[ISOTP_DATA_MAX];
	static uint8_t response[ISOTP_DATA_MAX];
	uint32_t tx_id;
	uint32_t rx_id;
	uint32_t timeout_ms = ISOTP_DEFAULT_TIMEOUT_MS;
	size_t request_len;
	int ret;

	if (parse_u32(argv[1], &tx_id) != 0 || parse_u32(argv[2], &rx_id) != 0 ||
	    parse_payload(argv[3], request, sizeof(request), &request_len) != 0 ||
	    (argc > 4 && parse_u32(argv[4], &timeout_ms) != 0) || timeout_ms == 0U) {
		shell_error(sh, "invalid CAN ID, hex payload, or timeout");
		return -EINVAL;
	}
	strip_single_frame_pci(request, &request_len);

	print_hex(sh, "TX: ", request, request_len);
	ret = isotp_request(tx_id, rx_id, request, request_len, response,
			    sizeof(response), timeout_ms);
	if (ret < 0) {
		shell_error(sh, "ISO-TP request failed (%d)", ret);
		return ret;
	}
	print_hex(sh, "RX: ", response, (size_t)ret);
	shell_print(sh, "PASS: received %d-byte UDS response", ret);
	return 0;
}

static int cmd_can_loop_test(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t tx_data[8];
	uint8_t rx_data[8];
	uint32_t can_id;
	uint32_t timeout_ms = ISOTP_DEFAULT_TIMEOUT_MS;
	size_t tx_len;
	int ret;

	if (parse_u32(argv[1], &can_id) != 0 ||
	    parse_payload(argv[2], tx_data, sizeof(tx_data), &tx_len) != 0 ||
	    tx_len != sizeof(tx_data) ||
	    (argc > 3 && parse_u32(argv[3], &timeout_ms) != 0) || timeout_ms == 0U) {
		shell_error(sh, "provide a CAN ID and exactly 8 data bytes");
		return -EINVAL;
	}

	print_hex(sh, "TX: ", tx_data, sizeof(tx_data));
	ret = raw_can_loop(can_id, tx_data, rx_data, timeout_ms);
	if (ret != 0) {
		shell_error(sh, "raw CAN loop test failed (%d)", ret);
		return ret;
	}
	print_hex(sh, "RX: ", rx_data, sizeof(rx_data));
	shell_print(sh, "PASS: raw CAN frame matched");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(isotp_commands,
	SHELL_CMD(info, NULL, "Show ISO-TP and CAN state.", cmd_isotp_info),
	SHELL_CMD_ARG(test, NULL,
		      "Send <tx_id> <rx_id> <hex_payload> [timeout_ms=5000].",
		      cmd_isotp_test, 4, 1),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(isotp, &isotp_commands, "ISO-TP UDS test commands.", NULL);

SHELL_STATIC_SUBCMD_SET_CREATE(canloop_commands,
	SHELL_CMD_ARG(test, NULL,
		      "Send and verify <can_id> <8-byte_hex_payload> [timeout_ms=5000].",
		      cmd_can_loop_test, 3, 1),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(canloop, &canloop_commands, "Raw CAN loop test commands.", NULL);

static int url_decode(char *text)
{
	char *read = text;
	char *write = text;

	while (*read != '\0') {
		if (*read == '+') {
			*write++ = ' ';
			read++;
		} else if (*read == '%' && isxdigit((unsigned char)read[1]) &&
			   isxdigit((unsigned char)read[2])) {
			int high = hex_nibble(read[1]);
			int low = hex_nibble(read[2]);

			*write++ = (char)((high << 4) | low);
			read += 3;
		} else {
			*write++ = *read++;
		}
	}
	*write = '\0';
	return 0;
}

static int form_value(const char *body, const char *name, char *value,
		      size_t value_size)
{
	size_t name_len = strlen(name);
	const char *field = body;

	while (*field != '\0') {
		const char *next = strchr(field, '&');
		size_t field_len = next == NULL ? strlen(field) : (size_t)(next - field);

		if (field_len > name_len && strncmp(field, name, name_len) == 0 &&
		    field[name_len] == '=') {
			size_t length = field_len - name_len - 1U;

			if (length >= value_size) {
				return -ENOSPC;
			}
			memcpy(value, field + name_len + 1U, length);
			value[length] = '\0';
			url_decode(value);
			return 0;
		}
		if (next == NULL) {
			break;
		}
		field = next + 1U;
	}
	return -ENOENT;
}

static int json_response(char *buffer, size_t capacity, enum http_status *status,
			 uint32_t tx_id, uint32_t rx_id, const uint8_t *tx,
			 size_t tx_len, const uint8_t *rx, int rx_len)
{
	size_t offset = 0;
	int length;

	length = snprintf(buffer, capacity,
			  "{\"status\":\"%s\",\"tx_id\":\"0x%" PRIX32
			  "\",\"rx_id\":\"0x%" PRIX32 "\",\"tx\":\"",
			  rx_len >= 0 ? "pass" : "fail", tx_id, rx_id);
	if (length < 0 || (size_t)length >= capacity) {
		return -ENOSPC;
	}
	offset = (size_t)length;
	for (size_t i = 0; i < tx_len && offset + 3U < capacity; i++) {
		offset += (size_t)snprintf(buffer + offset, capacity - offset,
					   "%02X%s", tx[i], i + 1U == tx_len ? "" : " ");
	}
	offset += (size_t)snprintf(buffer + offset, capacity - offset, "\",\"rx\":\"");
	if (rx_len >= 0) {
		for (int i = 0; i < rx_len && offset + 3U < capacity; i++) {
			offset += (size_t)snprintf(buffer + offset, capacity - offset,
						   "%02X%s", rx[i], i + 1 == rx_len ? "" : " ");
		}
		*status = HTTP_200_OK;
	} else {
		*status = rx_len == -EINVAL ? HTTP_400_BAD_REQUEST : HTTP_504_GATEWAY_TIMEOUT;
	}
	length = snprintf(buffer + offset, capacity - offset,
			  "\",\"result\":%d}\n", rx_len);
	if (length < 0 || (size_t)length >= capacity - offset) {
		return -ENOSPC;
	}
	return (int)(offset + (size_t)length);
}

static int raw_json_response(char *buffer, size_t capacity,
			     enum http_status *status, uint32_t can_id,
			     const uint8_t *tx, const uint8_t *rx, int result)
{
	int length;

	length = snprintf(buffer, capacity,
			  "{\"status\":\"%s\",\"can_id\":\"0x%" PRIX32
			  "\",\"tx\":\"%02X %02X %02X %02X %02X %02X %02X %02X\","
			  "\"rx\":\"%02X %02X %02X %02X %02X %02X %02X %02X\","
			  "\"result\":%d}\n",
			  result == 0 ? "pass" : "fail", can_id,
			  tx[0], tx[1], tx[2], tx[3], tx[4], tx[5], tx[6], tx[7],
			  rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[6], rx[7],
			  result);
	if (length < 0 || (size_t)length >= capacity) {
		return -ENOSPC;
	}
	*status = result == 0 ? HTTP_200_OK :
		  (result == -EINVAL ? HTTP_400_BAD_REQUEST :
		   (result == -EIO ? HTTP_409_CONFLICT : HTTP_504_GATEWAY_TIMEOUT));
	return length;
}

static int isotp_post_handler(struct http_client_ctx *client,
			      enum http_data_status data_status,
			      const struct http_request_ctx *request,
			      struct http_response_ctx *response, void *user_data)
{
	static char body[ISOTP_HTTP_BODY_MAX + 1U];
	static size_t body_len;
	static char response_body[ISOTP_HTTP_RESPONSE_MAX];
	static uint8_t tx_data[ISOTP_DATA_MAX];
	static uint8_t rx_data[ISOTP_DATA_MAX];
	static char payload_text[ISOTP_DATA_MAX * 3U + 1U];
	uint32_t tx_id = 0;
	uint32_t rx_id = 0;
	char tx_id_text[16];
	char rx_id_text[16];
	char mode[8];
	size_t tx_len = 0;
	int result = -EINVAL;
	int response_len;

	ARG_UNUSED(client);
	ARG_UNUSED(user_data);

	if (data_status == HTTP_SERVER_DATA_ABORTED) {
		body_len = 0;
		return 0;
	}
	if (request->data_len > ISOTP_HTTP_BODY_MAX - body_len) {
		body_len = 0;
		response->status = HTTP_413_PAYLOAD_TOO_LARGE;
		response->final_chunk = true;
		return 0;
	}
	memcpy(body + body_len, request->data, request->data_len);
	body_len += request->data_len;
	if (data_status != HTTP_SERVER_DATA_FINAL) {
		return 0;
	}
	body[body_len] = '\0';
	body_len = 0;

	memset(tx_data, 0, sizeof(tx_data));
	memset(rx_data, 0, sizeof(rx_data));
	if (form_value(body, "CAN_loop_test", mode, sizeof(mode)) == 0 &&
	    strcmp(mode, "on") == 0 &&
	    form_value(body, "CAND_ID", tx_id_text, sizeof(tx_id_text)) == 0 &&
	    form_value(body, "TX", payload_text, sizeof(payload_text)) == 0 &&
	    parse_u32(tx_id_text, &tx_id) == 0 &&
	    parse_payload(payload_text, tx_data, 8U, &tx_len) == 0 && tx_len == 8U) {
		result = raw_can_loop(tx_id, tx_data, rx_data, ISOTP_DEFAULT_TIMEOUT_MS);
		response_len = raw_json_response(response_body, sizeof(response_body),
					     &response->status, tx_id, tx_data,
					     rx_data, result);
	} else if (form_value(body, "CAN_TX_ID", tx_id_text, sizeof(tx_id_text)) == 0 &&
	    form_value(body, "CAN_RX_ID", rx_id_text, sizeof(rx_id_text)) == 0 &&
	    form_value(body, "TX", payload_text, sizeof(payload_text)) == 0 &&
	    parse_u32(tx_id_text, &tx_id) == 0 && parse_u32(rx_id_text, &rx_id) == 0 &&
	    parse_payload(payload_text, tx_data, sizeof(tx_data), &tx_len) == 0) {
		strip_single_frame_pci(tx_data, &tx_len);
		result = isotp_request(tx_id, rx_id, tx_data, tx_len, rx_data,
				       sizeof(rx_data), ISOTP_DEFAULT_TIMEOUT_MS);
		response_len = json_response(response_body, sizeof(response_body),
					     &response->status, tx_id, rx_id,
					     tx_data, tx_len, rx_data, result);
	} else {
		response_len = json_response(response_body, sizeof(response_body),
					     &response->status, tx_id, rx_id,
					     tx_data, tx_len, rx_data, result);
	}
	if (response_len < 0) {
		return response_len;
	}
	response->body = response_body;
	response->body_len = (size_t)response_len;
	response->final_chunk = true;
	return 0;
}

static struct http_resource_detail_dynamic isotp_resource_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_POST),
		.content_type = "application/json",
	},
	.cb = isotp_post_handler,
};

HTTP_RESOURCE_DEFINE(isotp_resource, edison_http_service, "/pmi/test",
		     &isotp_resource_detail);

int isotp_service_init(void)
{
	int ret;

	if (!device_is_ready(can_dev)) {
		LOG_ERR("CAN0 device is not ready");
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&can_active)) {
		LOG_ERR("CAN transceiver control GPIO is not ready");
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&can_active, GPIO_OUTPUT_ACTIVE);
	if (ret != 0) {
		LOG_ERR("Could not enable CAN transceiver (%d)", ret);
		return ret;
	}
	ret = can_start(can_dev);
	if (ret != 0 && ret != -EALREADY) {
		LOG_ERR("Could not start CAN0 (%d)", ret);
		return ret;
	}

	service_ready = true;
	LOG_INF("ISO-TP ready on CAN0 at 250 kbit/s");
	return 0;
}
