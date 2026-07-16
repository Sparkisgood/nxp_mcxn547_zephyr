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

## Ethernet and ping test

Connect the board's Ethernet port to a LAN with a DHCPv4 server, then open the
MCXN547 UART console. The application prints its assigned IPv4 address,
netmask, and gateway when DHCP completes.

At the Zephyr shell prompt, inspect the interface and ping the printed gateway:

```text
uart:~$ net iface
uart:~$ net ping -c 4 <gateway-address>
```

For example:

```text
uart:~$ net ping -c 4 192.168.1.1
```

Four echo replies confirm that the PHY link, Ethernet driver, IPv4/ARP stack,
and outbound/return network path are operating. The gateway address depends on
the connected LAN and must not be hard-coded in the firmware.

## HTTP GET and POST

The application starts an HTTP server on TCP port 80. Replace `<board-ip>`
with the IPv4 address printed after DHCP completes.

Read application status and uptime with GET:

```bash
curl http://<board-ip>/api/status
```

Example response:

```json
{"status":"ok","version":"0.1.0","uptime_ms":12345}
```

The application version is defined in `VERSION`. It is also printed on the
UART log during startup:

```text
<inf> app: Application version: 0.1.0
```

Send a request body and receive it back with POST:

```bash
curl -X POST \
  -H 'Content-Type: text/plain' \
  --data 'hello from the host' \
  http://<board-ip>/api/echo
```

The HTTP service accepts one concurrent client. It is an unencrypted
development interface; do not expose it directly to an untrusted network.

## PSRAM

The Edison hardware's 8 MiB PSRAM is configured on FlexSPI0 Port B1. The
implementation uses the same `0x9f` ID, `0xeb` quad-read, and `0x38`
quad-write command sequences as the reference `flex_spi.c`.

Use the UART shell to inspect the device and run a small read/write test:

```text
uart:~$ psram info
uart:~$ psram test
uart:~$ psram test 0x5a 0x1000 256
```

The stress command writes an incrementing pattern, reads it back, and compares
it in 1 KiB blocks. Its arguments are `[length] [offset]`; both the normal and
stress tests overwrite the selected PSRAM range.

```text
uart:~$ psram stress 65536 0
```

## PMI firmware update over Ethernet

The firmware-update service follows the reference `cgi_upload_image()` and
`cgi_pmi_update()` flow. It stages an uploaded image in PSRAM, verifies its
MCUboot header and CRC, copies it to the inactive slot, verifies the flash CRC,
and requests an MCUboot test upgrade. It never writes the bootloader or active
application partitions.

Upload the signed MCUboot binary produced by sysbuild. Do not upload the plain
`zephyr.bin` file:

```bash
curl -X POST --data-binary \
  @build/app/app/zephyr/zephyr.signed.bin \
  http://<board-ip>/pmi/image
```

A successful upload returns `"status":"ready"`. Start the background copy to
MCUboot slot 1:

```bash
curl -X POST -d "action=update" http://<board-ip>/pmi/update
```

Query progress until the response reports `"status":"ready"`,
`"percent":100`, and `"reboot_required":true`:

```bash
curl http://<board-ip>/pmi/update
```

Reset the board to let MCUboot test the new image. After the new application
reaches firmware-update service initialization, it confirms the running image
so MCUboot will not revert it on the following reset. The HTTP service is
unencrypted and unauthenticated, so it is intended only for a trusted
development network.

## Device reboot

Schedule a cold reboot from the UART shell. The short delay allows the shell
message to be transmitted before the system resets:

```text
uart:~$ pmi reboot
Cold reboot scheduled in 500 ms
```

Zephyr's standard `kernel reboot cold` shell command is also enabled.

The same operation is available through HTTP POST. The server sends its JSON
response before the delayed reboot occurs:

```bash
curl -X POST http://<board-ip>/pmi/reboot
```

Example response:

```json
{"status":"ok","action":"reboot","delay_ms":500}
```
