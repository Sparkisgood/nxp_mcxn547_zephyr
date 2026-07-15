/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include <fsl_flexspi.h>
#include "memc_mcux_flexspi.h"

#include "psram.h"

LOG_MODULE_REGISTER(psram, LOG_LEVEL_INF);

#define PSRAM_CONTROLLER DEVICE_DT_GET(DT_NODELABEL(flexspi))
#define PSRAM_PORT kFLEXSPI_PortB1
#define PSRAM_SIZE (8U * 1024U * 1024U)
#define PSRAM_CLOCK_HZ 100000000U
#define PSRAM_TEST_OFFSET 0x1000U
#define PSRAM_BUFFER_SIZE 1024U

enum psram_lut_index {
	PSRAM_READ_ID,
	PSRAM_QUAD_READ,
	PSRAM_QUAD_WRITE,
};

static const uint32_t psram_lut[][4] = {
	[PSRAM_READ_ID] = {
		FLEXSPI_LUT_SEQ(kFLEXSPI_Command_SDR, kFLEXSPI_1PAD, 0x9f,
				kFLEXSPI_Command_DUMMY_SDR, kFLEXSPI_1PAD, 0x18),
		FLEXSPI_LUT_SEQ(kFLEXSPI_Command_READ_SDR, kFLEXSPI_1PAD, 0x04,
				kFLEXSPI_Command_STOP, kFLEXSPI_1PAD, 0),
	},
	[PSRAM_QUAD_READ] = {
		FLEXSPI_LUT_SEQ(kFLEXSPI_Command_SDR, kFLEXSPI_1PAD, 0xeb,
				kFLEXSPI_Command_RADDR_SDR, kFLEXSPI_4PAD, 24),
		FLEXSPI_LUT_SEQ(kFLEXSPI_Command_DUMMY_SDR, kFLEXSPI_4PAD, 6,
				kFLEXSPI_Command_READ_SDR, kFLEXSPI_4PAD, 4),
	},
	[PSRAM_QUAD_WRITE] = {
		FLEXSPI_LUT_SEQ(kFLEXSPI_Command_SDR, kFLEXSPI_1PAD, 0x38,
				kFLEXSPI_Command_RADDR_SDR, kFLEXSPI_4PAD, 24),
		FLEXSPI_LUT_SEQ(kFLEXSPI_Command_WRITE_SDR, kFLEXSPI_4PAD, 0,
				kFLEXSPI_Command_STOP, kFLEXSPI_1PAD, 0),
	},
};

static uint32_t write_buffer[PSRAM_BUFFER_SIZE / sizeof(uint32_t)];
static uint32_t read_buffer[PSRAM_BUFFER_SIZE / sizeof(uint32_t)];
static struct k_mutex psram_lock;
static bool psram_ready;
static uint8_t psram_id[3];

static int psram_transfer(uint32_t address, void *buffer, size_t length,
			  flexspi_command_type_t type, uint8_t sequence)
{
	flexspi_transfer_t transfer = {
		.deviceAddress = address,
		.port = PSRAM_PORT,
		.cmdType = type,
		.SeqNumber = 1,
		.seqIndex = sequence,
		.data = buffer,
		.dataSize = length,
	};

	return memc_flexspi_transfer(PSRAM_CONTROLLER, &transfer);
}

static int psram_read_id(void)
{
	uint32_t id = 0;
	int ret = psram_transfer(0, &id, sizeof(id), kFLEXSPI_Read,
				 PSRAM_READ_ID);

	memcpy(psram_id, &id, sizeof(psram_id));
	psram_id[2] &= 0xe0;
	return ret;
}

static int psram_check_range(uint32_t offset, size_t length)
{
	if (length == 0 || length > PSRAM_BUFFER_SIZE ||
	    offset >= PSRAM_SIZE || length > PSRAM_SIZE - offset) {
		return -EINVAL;
	}

	return 0;
}

static int psram_test_block(uint32_t offset, size_t length, uint8_t pattern,
			    bool incrementing)
{
	uint8_t *write_bytes = (uint8_t *)write_buffer;
	int ret = psram_check_range(offset, length);

	if (ret != 0) {
		return ret;
	}

	for (size_t i = 0; i < length; i++) {
		write_bytes[i] = incrementing ? (uint8_t)(offset + i) : pattern;
	}
	memset(read_buffer, 0, length);

	ret = psram_transfer(offset, write_buffer, length, kFLEXSPI_Write,
			     PSRAM_QUAD_WRITE);
	if (ret == 0) {
		ret = psram_transfer(offset, read_buffer, length, kFLEXSPI_Read,
				     PSRAM_QUAD_READ);
	}
	if (ret == 0 && memcmp(write_buffer, read_buffer, length) != 0) {
		ret = -EIO;
	}

	return ret;
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

static int cmd_psram_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "PSRAM: %s", psram_ready ? "ready" : "not ready");
	shell_print(sh, "Port: FlexSPI0 B1, size: %u bytes", PSRAM_SIZE);
	shell_print(sh, "ID: %02x %02x %02x", psram_id[0], psram_id[1], psram_id[2]);
	return psram_ready ? 0 : -ENODEV;
}

static int cmd_psram_test(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t pattern = 0xa5;
	uint32_t offset = PSRAM_TEST_OFFSET;
	uint32_t length = 256;
	int ret;

	if (!psram_ready) {
		shell_error(sh, "PSRAM is not ready");
		return -ENODEV;
	}
	if ((argc > 1 && parse_u32(argv[1], &pattern)) || pattern > UINT8_MAX ||
	    (argc > 2 && parse_u32(argv[2], &offset)) ||
	    (argc > 3 && parse_u32(argv[3], &length))) {
		shell_error(sh, "invalid argument");
		return -EINVAL;
	}

	k_mutex_lock(&psram_lock, K_FOREVER);
	ret = psram_test_block(offset, length, (uint8_t)pattern, false);
	k_mutex_unlock(&psram_lock);

	if (ret == 0) {
		shell_print(sh, "PASS: %u bytes at 0x%08x, pattern 0x%02x",
			    length, offset, pattern);
	} else {
		shell_error(sh, "FAIL: PSRAM test returned %d", ret);
	}
	return ret;
}

static int cmd_psram_stress(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t length = KB(64);
	uint32_t offset = 0;
	int ret = 0;

	if (!psram_ready) {
		return -ENODEV;
	}
	if ((argc > 1 && parse_u32(argv[1], &length)) ||
	    (argc > 2 && parse_u32(argv[2], &offset)) ||
	    length == 0 || offset >= PSRAM_SIZE || length > PSRAM_SIZE - offset) {
		shell_error(sh, "range must fit inside the 8 MiB PSRAM");
		return -EINVAL;
	}

	shell_warn(sh, "destructive test: offset 0x%08x, length %u", offset, length);
	k_mutex_lock(&psram_lock, K_FOREVER);
	for (uint32_t done = 0; done < length; done += PSRAM_BUFFER_SIZE) {
		size_t chunk = MIN((uint32_t)PSRAM_BUFFER_SIZE, length - done);

		ret = psram_test_block(offset + done, chunk, 0, true);
		if (ret != 0) {
			shell_error(sh, "FAIL at offset 0x%08x (%d)", offset + done, ret);
			break;
		}
	}
	k_mutex_unlock(&psram_lock);

	if (ret == 0) {
		shell_print(sh, "PASS: verified %u bytes", length);
	}
	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(psram_commands,
	SHELL_CMD(info, NULL, "Show PSRAM state, size, and device ID.", cmd_psram_info),
	SHELL_CMD_ARG(test, NULL, "Test [pattern=0xa5] [offset=0x1000] [length=256].",
		      cmd_psram_test, 1, 3),
	SHELL_CMD_ARG(stress, NULL, "Destructive stress [length=65536] [offset=0].",
		      cmd_psram_stress, 1, 2),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(psram, &psram_commands, "FlexSPI PSRAM commands.", NULL);

void psram_service_init(void)
{
	flexspi_device_config_t config = {
		.flexspiRootClk = PSRAM_CLOCK_HZ,
		.flashSize = PSRAM_SIZE / KB(1),
		.CSIntervalUnit = kFLEXSPI_CsIntervalUnit1SckCycle,
		.CSInterval = 2,
		.CSHoldTime = 3,
		.CSSetupTime = 3,
		.dataValidTime = 2,
		.columnspace = 0,
		.enableWordAddress = false,
		.AWRSeqIndex = PSRAM_QUAD_WRITE,
		.AWRSeqNumber = 1,
		.ARDSeqIndex = PSRAM_QUAD_READ,
		.ARDSeqNumber = 1,
		.AHBWriteWaitUnit = kFLEXSPI_AhbWriteWaitUnit2AhbCycle,
		.AHBWriteWaitInterval = 0,
		.enableWriteMask = false,
	};
	int ret;

	k_mutex_init(&psram_lock);
	if (!device_is_ready(PSRAM_CONTROLLER)) {
		LOG_ERR("FlexSPI controller is not ready");
		return;
	}

	ret = memc_flexspi_set_device_config(PSRAM_CONTROLLER, &config,
			(const uint32_t *)psram_lut, ARRAY_SIZE(psram_lut) * 4,
			PSRAM_PORT);
	if (ret == 0) {
		ret = psram_read_id();
	}
	if (ret != 0) {
		LOG_ERR("PSRAM initialization failed (%d)", ret);
		return;
	}

	psram_ready = true;
	LOG_INF("PSRAM ready: 8 MiB on FlexSPI0 B1, ID %02x %02x %02x",
		psram_id[0], psram_id[1], psram_id[2]);
}
