# MCUboot customization

MCUboot is built from the west-managed source at:

```text
~/Music/rtos/zephyr/bootloader/mcuboot
```

The source is intentionally not copied into this repository. Product-specific
MCUboot Kconfig settings live in `app/sysbuild/mcuboot.conf`, and sysbuild
settings live in `app/sysbuild.conf`.

Place source changes exported with `git format-patch` in `patches/mcuboot/`.
Each patch series should document the exact MCUboot base revision against which
it was created and tested.
