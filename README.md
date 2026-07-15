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
