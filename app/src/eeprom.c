/* SPDX-License-Identifier: Apache-2.0 */

#include "eeprom.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

LOG_MODULE_REGISTER(spi_eeprom, LOG_LEVEL_INF);

#define EEPROM_NODE DT_NODELABEL(spi_eeprom)
#define EEPROM_DEVICE DEVICE_DT_GET(EEPROM_NODE)
#define EEPROM_SIZE 8192U
#define EEPROM_PAGE_SIZE 32U
#define EEPROM_TEST_OFFSET 0x1000U
#define EEPROM_READ_ID 0x9fU
#define EEPROM_READ 0x03U
#define EEPROM_WRITE 0x02U
#define EEPROM_WRITE_ENABLE 0x06U
#define EEPROM_READ_STATUS 0x05U
#define EEPROM_STATUS_WIP BIT(0)
#define EEPROM_WRITE_TIMEOUT_MS 10U

static const struct spi_dt_spec eeprom_spi =
	SPI_DT_SPEC_GET(EEPROM_NODE, SPI_WORD_SET(8));
static uint8_t eeprom_id[3];
static bool eeprom_ready;

/*
 * Always provide an RX buffer, including for commands that only transmit.
 * The interrupt-driven NXP LPSPI driver stops draining its RX FIFO for a
 * TX-only transfer. A page write is longer than that FIFO and otherwise
 * stalls with -ETIMEDOUT.
 */
static int eeprom_transceive(uint8_t *tx, uint8_t *rx, size_t length)
{
	const struct spi_buf tx_buf = {.buf = tx, .len = length};
	const struct spi_buf rx_buf = {.buf = rx, .len = length};
	const struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1};
	const struct spi_buf_set rx_set = {.buffers = &rx_buf, .count = 1};

	return spi_transceive_dt(&eeprom_spi, &tx_set, &rx_set);
}

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

static int read_id(void)
{
	uint8_t tx[4] = {EEPROM_READ_ID, 0, 0, 0};
	uint8_t rx[4] = {0};
	int ret = eeprom_transceive(tx, rx, sizeof(tx));

	if (ret == 0) {
		memcpy(eeprom_id, &rx[1], sizeof(eeprom_id));
	}
	return ret;
}

static int read_status(uint8_t *status)
{
	uint8_t tx[2] = {EEPROM_READ_STATUS, 0};
	uint8_t rx[2] = {0};
	int ret = eeprom_transceive(tx, rx, sizeof(tx));

	if (ret == 0) {
		*status = rx[1];
	}
	return ret;
}

static int wait_until_ready(void)
{
	int64_t deadline = k_uptime_get() + EEPROM_WRITE_TIMEOUT_MS;
	uint8_t status;
	int ret;

	do {
		ret = read_status(&status);
		if (ret != 0 || (status & EEPROM_STATUS_WIP) == 0U) {
			return ret;
		}
		k_msleep(1);
	} while (k_uptime_get() < deadline);

	return -ETIMEDOUT;
}

static int read_data(uint32_t offset, uint8_t *data, size_t length)
{
	uint8_t tx[EEPROM_PAGE_SIZE + 3] = {
		EEPROM_READ, (uint8_t)(offset >> 8), (uint8_t)offset
	};
	uint8_t rx[sizeof(tx)] = {0};
	int ret = eeprom_transceive(tx, rx, length + 3U);

	if (ret == 0) {
		memcpy(data, &rx[3], length);
	}
	return ret;
}

static int write_page(uint32_t offset, const uint8_t *data, size_t length)
{
	uint8_t enable_tx[1] = {EEPROM_WRITE_ENABLE};
	uint8_t enable_rx[1] = {0};
	uint8_t tx[EEPROM_PAGE_SIZE + 3] = {
		EEPROM_WRITE, (uint8_t)(offset >> 8), (uint8_t)offset
	};
	uint8_t rx[sizeof(tx)] = {0};
	int ret;

	ret = eeprom_transceive(enable_tx, enable_rx, sizeof(enable_tx));
	if (ret != 0) {
		return ret;
	}

	memcpy(&tx[3], data, length);
	ret = eeprom_transceive(tx, rx, length + 3U);
	if (ret != 0) {
		return ret;
	}

	return wait_until_ready();
}

static int cmd_eeprom_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "SPI EEPROM: %s", eeprom_ready ? "ready" : "not ready");
	shell_print(sh, "Size: %u bytes, page: %u bytes, bus: LPSPI0 at 1 MHz",
		    EEPROM_SIZE, EEPROM_PAGE_SIZE);
	shell_print(sh, "ID: %02x %02x %02x", eeprom_id[0], eeprom_id[1],
		    eeprom_id[2]);
	return eeprom_ready ? 0 : -ENODEV;
}

static int cmd_eeprom_read(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t buffer[EEPROM_PAGE_SIZE];
	uint32_t offset;
	uint32_t length = EEPROM_PAGE_SIZE;
	int ret;

	if (!eeprom_ready) {
		return -ENODEV;
	}
	if (parse_u32(argv[1], &offset) != 0 ||
	    (argc > 2 && parse_u32(argv[2], &length) != 0) ||
	    length == 0U || length > sizeof(buffer) || offset >= EEPROM_SIZE ||
	    length > EEPROM_SIZE - offset) {
		shell_error(sh, "range must fit in EEPROM; maximum length is 32");
		return -EINVAL;
	}

	ret = read_data(offset, buffer, length);
	if (ret == 0) {
		shell_hexdump(sh, buffer, length);
	}
	return ret;
}

static int cmd_eeprom_test(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t write_buffer[EEPROM_PAGE_SIZE];
	uint8_t read_buffer[EEPROM_PAGE_SIZE];
	uint32_t pattern = 0xa5U;
	uint32_t offset = EEPROM_TEST_OFFSET;
	int ret;

	if (!eeprom_ready) {
		return -ENODEV;
	}
	if ((argc > 1 && parse_u32(argv[1], &pattern) != 0) || pattern > UINT8_MAX ||
	    (argc > 2 && parse_u32(argv[2], &offset) != 0) ||
	    offset % EEPROM_PAGE_SIZE != 0U || offset > EEPROM_SIZE - EEPROM_PAGE_SIZE) {
		shell_error(sh, "offset must be a 32-byte page within the 8 KiB EEPROM");
		return -EINVAL;
	}

	shell_warn(sh, "destructive test: overwriting page at 0x%04x", offset);
	memset(write_buffer, (uint8_t)pattern, sizeof(write_buffer));
	memset(read_buffer, 0, sizeof(read_buffer));
	ret = write_page(offset, write_buffer, sizeof(write_buffer));
	if (ret == 0) {
		ret = read_data(offset, read_buffer, sizeof(read_buffer));
	}
	if (ret == 0 && memcmp(write_buffer, read_buffer, sizeof(write_buffer)) != 0) {
		ret = -EIO;
	}

	if (ret == 0) {
		shell_print(sh, "PASS: verified 32 bytes at 0x%04x", offset);
	} else {
		shell_error(sh, "FAIL: EEPROM test returned %d", ret);
	}
	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(eeprom_commands,
	SHELL_CMD(info, NULL, "Show SPI EEPROM state and ID.", cmd_eeprom_info),
	SHELL_CMD_ARG(read, NULL, "Read <offset> [length=32], maximum 32 bytes.",
		      cmd_eeprom_read, 2, 1),
	SHELL_CMD_ARG(test, NULL, "Destructive test [pattern=0xa5] [page=0x1000].",
		      cmd_eeprom_test, 1, 2),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(eeprom, &eeprom_commands, "SPI EEPROM commands.", NULL);

void eeprom_service_init(void)
{
	int ret;

	if (!device_is_ready(EEPROM_DEVICE) || !spi_is_ready_dt(&eeprom_spi)) {
		LOG_ERR("SPI EEPROM is not ready");
		return;
	}

	ret = read_id();
	if (ret != 0) {
		LOG_ERR("Could not read SPI EEPROM ID (%d)", ret);
		return;
	}

	eeprom_ready = true;
	LOG_INF("SPI EEPROM ready: 8 KiB on LPSPI0, ID %02x %02x %02x",
		eeprom_id[0], eeprom_id[1], eeprom_id[2]);
}
