# MCXN547 Blinky App

This is a Zephyr application named `edison` targeting
`mcx_n5xx_evk/mcxn547/cpu0`. Sysbuild builds the application and MCUboot
together. Zephyr, MCUboot, modules, and the SDK remain in the external west
workspace under `~/Music/rtos/zephyr`.

## Build

```bash
cd ~/Music/rtos/nxp_mcxn547_zephyr
source ./flex_env.sh
flex_build_n547
```

## Flash

```bash
flex_flash_image
```

The board devicetree provides an 80 KiB MCUboot partition and two 984 KiB
image slots. Product-specific MCUboot settings are in
`sysbuild/mcuboot.conf`.
