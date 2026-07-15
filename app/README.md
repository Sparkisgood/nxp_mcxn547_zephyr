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
{"status":"ok","uptime_ms":12345}
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
