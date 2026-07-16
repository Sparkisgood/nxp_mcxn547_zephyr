/* SPDX-License-Identifier: Apache-2.0 */

#include "qspi_nor.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(qspi_nor, LOG_LEVEL_INF);

#define QSPI_NOR_DEVICE DEVICE_DT_GET(DT_NODELABEL(w25q64jwtbjq))
#define QSPI_NOR_SIZE DT_PROP(DT_NODELABEL(w25q64jwtbjq), size) / 8U
#define QSPI_SECTOR_SIZE DT_PROP(DT_NODELABEL(w25q64jwtbjq), erase_block_size)
#define QSPI_PAGE_SIZE 256U
#define QSPI_TEST_OFFSET (QSPI_NOR_SIZE - QSPI_SECTOR_SIZE)

static uint8_t write_buffer[QSPI_PAGE_SIZE];
static uint8_t read_buffer[QSPI_PAGE_SIZE];
static uint8_t jedec_id[3];
static bool qspi_ready;

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

static int cmd_qspi_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "QSPI NOR: %s", qspi_ready ? "ready" : "not ready");
	shell_print(sh, "Device: W25Q64JV, FlexSPI0 A1, size: %u bytes",
		    QSPI_NOR_SIZE);
	shell_print(sh, "JEDEC ID: %02x %02x %02x", jedec_id[0], jedec_id[1],
		    jedec_id[2]);
	return qspi_ready ? 0 : -ENODEV;
}

static int cmd_qspi_read(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t offset;
	uint32_t length = 64U;
	int ret;

	if (!qspi_ready) {
		return -ENODEV;
	}
	if (parse_u32(argv[1], &offset) != 0 ||
	    (argc > 2 && parse_u32(argv[2], &length) != 0) ||
	    length == 0U || length > sizeof(read_buffer) ||
	    offset >= QSPI_NOR_SIZE || length > QSPI_NOR_SIZE - offset) {
		shell_error(sh, "range must fit in NOR; maximum read length is 256");
		return -EINVAL;
	}

	ret = flash_read(QSPI_NOR_DEVICE, offset, read_buffer, length);
	if (ret == 0) {
		shell_hexdump(sh, read_buffer, length);
	}
	return ret;
}

static int cmd_qspi_test(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t pattern = 0xa5U;
	uint32_t offset = QSPI_TEST_OFFSET;
	int ret;

	if (!qspi_ready) {
		return -ENODEV;
	}
	if ((argc > 1 && parse_u32(argv[1], &pattern) != 0) || pattern > UINT8_MAX ||
	    (argc > 2 && parse_u32(argv[2], &offset) != 0) ||
	    offset % QSPI_SECTOR_SIZE != 0U || offset > QSPI_NOR_SIZE - QSPI_SECTOR_SIZE) {
		shell_error(sh, "offset must be a 4 KiB sector within the 8 MiB NOR");
		return -EINVAL;
	}

	shell_warn(sh, "destructive test: erasing sector at 0x%08x", offset);
	memset(write_buffer, (uint8_t)pattern, sizeof(write_buffer));
	memset(read_buffer, 0, sizeof(read_buffer));

	ret = flash_erase(QSPI_NOR_DEVICE, offset, QSPI_SECTOR_SIZE);
	if (ret == 0) {
		ret = flash_write(QSPI_NOR_DEVICE, offset, write_buffer,
				  sizeof(write_buffer));
	}
	if (ret == 0) {
		ret = flash_read(QSPI_NOR_DEVICE, offset, read_buffer,
				 sizeof(read_buffer));
	}
	if (ret == 0 && memcmp(write_buffer, read_buffer, sizeof(write_buffer)) != 0) {
		ret = -EIO;
	}

	if (ret == 0) {
		shell_print(sh, "PASS: verified %u bytes at 0x%08x", QSPI_PAGE_SIZE,
			    offset);
	} else {
		shell_error(sh, "FAIL: QSPI NOR test returned %d", ret);
	}
	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(qspi_commands,
	SHELL_CMD(info, NULL, "Show QSPI NOR state and JEDEC ID.", cmd_qspi_info),
	SHELL_CMD_ARG(read, NULL, "Read <offset> [length=64], maximum 256 bytes.",
		      cmd_qspi_read, 2, 1),
	SHELL_CMD_ARG(test, NULL, "Destructive test [pattern=0xa5] [sector=0x7ff000].",
		      cmd_qspi_test, 1, 2),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(qspi, &qspi_commands, "External QSPI NOR commands.", NULL);

void qspi_nor_service_init(void)
{
	int ret;

	if (!device_is_ready(QSPI_NOR_DEVICE)) {
		LOG_ERR("QSPI NOR device is not ready");
		return;
	}

	ret = flash_read_jedec_id(QSPI_NOR_DEVICE, jedec_id);
	if (ret != 0) {
		LOG_ERR("Could not read QSPI NOR JEDEC ID (%d)", ret);
		return;
	}

	qspi_ready = true;
	LOG_INF("QSPI NOR ready: W25Q64JV 8 MiB on FlexSPI0 A1, ID %02x %02x %02x",
		jedec_id[0], jedec_id[1], jedec_id[2]);
}
