# NXP MCXN547 Zephyr firmware

This repository contains the MCXN547 application and product-specific MCUboot
configuration. Zephyr, MCUboot, modules, and the Zephyr SDK are provided by the
external west workspace at `~/Music/rtos/zephyr`.

## Flash partition layout

The `mcx_n5xx_evk/mcxn547/cpu0` board defines the following layout in its
Zephyr devicetree:

| Partition | Start | End | Size | Purpose |
| --- | ---: | ---: | ---: | --- |
| `boot_partition` | `0x00000000` | `0x00013FFF` | 80 KiB | MCUboot bootloader |
| `slot0_partition` | `0x00014000` | `0x00109FFF` | 984 KiB | Primary signed application image |
| `slot1_partition` | `0x0010A000` | `0x001FFFFF` | 984 KiB | Secondary firmware-update image |

```text
0x00000000  +-------------------------------+
            | MCUboot                       | 80 KiB
0x00014000  +-------------------------------+
            | Primary application slot      | 984 KiB
0x0010A000  +-------------------------------+
            | Secondary update slot         | 984 KiB
0x00200000  +-------------------------------+
```

The partition addresses are fixed; the application is not placed immediately
after the amount of flash actually used by MCUboot. Sysbuild links MCUboot at
the boot partition and places the signed application in slot 0. The generated
HEX files contain absolute flash addresses, while runner metadata supplies the
offset when a raw BIN file is flashed.

The upstream layout is defined in:

```text
~/Music/rtos/zephyr/zephyr/boards/nxp/mcx_nx4x_evk/mcx_nx4x_evk.dtsi
```

## Build and flash

```bash
cd ~/Music/rtos/nxp_mcxn547_zephyr
source ./flex_env.sh
flex_build_n547
flex_flash_image
```

`west flash` reads `build/app/domains.yaml` and flashes MCUboot before the
application. Do not edit generated build files or `domains.yaml` manually.

## Release package

Create a versioned release directory containing a combined full-flash image,
the signed PMI update image, and release instructions:

```bash
./tools/flex_release.sh
```

The helper reads the version from `app/VERSION` and rebuilds the sysbuild
application by default. To package existing build outputs without rebuilding:

```bash
./tools/flex_release.sh --no-build
```

Output is written under `release/edison_pmi_v<version>/`. For example, version
`0.1.0-rc1` produces `edison_pmi_full_v0.1.0-rc1.bin` and
`edison_pmi_update_v0.1.0-rc1.bin`.

## Coding style

All new or modified code must follow the Zephyr coding-style guidelines. Keep
the style of nearby code when a rule is not explicitly covered, and do not mix
unrelated formatting changes into a feature or bug-fix commit.

### C source and headers

- Indent with tabs, with a tab width of 8 characters. Use spaces only for
  alignment where appropriate.
- Keep lines at 100 columns or fewer.
- Use `snake_case` for functions and variables. Use uppercase `SNAKE_CASE` for
  macros and compile-time constants.
- Put braces around every `if`, `else`, `for`, `while`, `do`, and `switch`
  body, including one-line bodies.
- Use `/* comment */` instead of `// comment`. Use `/** ... */` only for API
  documentation that should be processed by Doxygen.
- Keep declarations close to their use, initialize variables, make internal
  functions and data `static`, and use `const` wherever possible.
- Use Zephyr types, helpers, and APIs rather than adding local equivalents.
  Prefer helpers such as `ARRAY_SIZE()`, `BIT()`, `MIN()`, and `MAX()` over
  open-coded implementations.
- Return standard negative error values such as `-EINVAL`, `-ENODEV`, and
  `-EIO`. Check and propagate errors from Zephyr and driver APIs.
- Use the logging subsystem (`LOG_INF()`, `LOG_WRN()`, and `LOG_ERR()`) for
  runtime diagnostics. Do not add unconditional `printk()` calls to production
  paths.
- Public headers require include guards. Keep private declarations in the
  module's source file or private header.

Example:

```c
static int device_read(const struct device *dev, uint8_t *buffer, size_t length)
{
	int ret;

	if (dev == NULL || buffer == NULL || length == 0U) {
		return -EINVAL;
	}

	ret = driver_read(dev, buffer, length);
	if (ret < 0) {
		LOG_ERR("Read failed (%d)", ret);
		return ret;
	}

	return 0;
}
```

### Devicetree, Kconfig, and CMake

- Describe hardware in devicetree overlays rather than hard-coding controller
  addresses or pins in C. Use lowercase node names and labels, with hyphens in
  property names.
- Add user-selectable functionality through Kconfig. Prefix application-owned
  symbols with `APP_` and provide useful help text.
- Keep `prj.conf` grouped by subsystem and explain settings whose purpose is
  not obvious.
- List sources explicitly in `CMakeLists.txt`; do not use wildcard source-file
  discovery.
- Do not edit files under `build/`, generated headers, `domains.yaml`, Zephyr,
  MCUboot, or SDK sources to implement an application feature.

### Module organization

Keep each feature in its own `.c` and `.h` files, as done for Ethernet, HTTP,
and PSRAM. `main.c` should coordinate service initialization and the main loop;
hardware access and feature logic belong in their respective modules. Protect
shared peripheral state using the appropriate Zephyr synchronization object.

### Before committing

Format changed C files using the Zephyr workspace configuration:

```bash
clang-format --style=file:"$ZEPHYR_BASE/.clang-format" -i \
  app/src/changed_file.c app/src/changed_file.h
```

Review staged C-style issues with Zephyr's `checkpatch` tool:

```bash
git diff --cached | "$ZEPHYR_BASE/scripts/checkpatch.pl" -
```

Finally, check whitespace and build the complete MCUboot/application image:

```bash
git diff --check
source ./flex_env.sh
flex_build_n547
```

Warnings must be understood and documented; new compiler errors or warnings
must not be committed. Use focused commit subjects such as:

```text
feat(psram): add FlexSPI read and write tests
fix(http): validate POST request body length
docs(readme): document Zephyr coding style
```
