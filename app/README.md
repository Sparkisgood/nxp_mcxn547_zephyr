# Edison Blinky App

This is a Zephyr application named `edison` targeting `frdm_mcxn947`.

## Build

```sh
cd /home/spark/Music/rtos/zephyr/zephyr
west build -b frdm_mcxn947 app -p always
```

## Flash

```sh
west flash
```
