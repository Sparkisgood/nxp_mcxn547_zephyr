#!/usr/bin/env bash

# This script must be sourced:
#   source ./flex_env.sh

CWD="$PWD"
ZEPHYR_WORKSPACE="${ZEPHYR_WORKSPACE:-$HOME/Music/rtos/zephyr}"
export XDG_CACHE_HOME="${XDG_CACHE_HOME:-$CWD/.cache}"

source "$ZEPHYR_WORKSPACE/.venv/bin/activate"
source "$ZEPHYR_WORKSPACE/zephyr/zephyr-env.sh"

flex_build_n947() {
  cbb
  west build -p always -b frdm_mcxn947/mcxn947/cpu0 samples/basic/blinky
}

flex_build_n547() {
  west build -p always --sysbuild \
    -b mcx_n5xx_evk/mcxn547/cpu0 \
    -s "$CWD/app" \
    -d "$CWD/build/app"
}

flex_clean_build() {
  rm -rf -- "$CWD/build"
}

flex_flash_image() {
  west flash -d "$CWD/build/app"
}

flex_pmi_update_test() {
  local board_ip="${1:-${PMI_BOARD_IP:-192.168.10.101}}"
  local image="${2:-$CWD/build/app/app/zephyr/zephyr.signed.bin}"
  local status
  local attempt

  if ! command -v curl >/dev/null 2>&1; then
    echo "Error: curl is required for the PMI firmware update test." >&2
    return 127
  fi
  if [[ ! -f "$image" ]]; then
    echo "Error: signed MCUboot image not found: $image" >&2
    echo "Build it first with: flex_build_n547" >&2
    return 1
  fi

  echo "Uploading $image to $board_ip ..."
  curl --fail --show-error --silent \
    --request POST --data-binary "@$image" \
    "http://$board_ip/pmi/image" || return
  echo

  echo "Requesting the slot update ..."
  curl --fail --show-error --silent \
    --request POST --data 'action=update' \
    "http://$board_ip/pmi/update" || return
  echo

  echo "Waiting for the image to be ready in MCUboot slot 1 ..."
  for attempt in $(seq 1 120); do
    status="$(curl --fail --show-error --silent \
      "http://$board_ip/pmi/update")" || return
    printf '\r[%3d/120] %s' "$attempt" "$status"

    if [[ "$status" == *'"status":"ready"'* &&
          "$status" == *'"reboot_required":true'* ]]; then
      echo
      echo "PMI firmware update test passed. Reboot to install the image:"
      echo "  curl -X POST http://$board_ip/pmi/reboot"
      return 0
    fi
    if [[ "$status" == *'"status":"error"'* ]]; then
      echo
      echo "Error: PMI firmware update failed." >&2
      return 1
    fi
    sleep 1
  done

  echo
  echo "Error: timed out waiting for the PMI firmware update." >&2
  return 1
}

cbb() {
  cd "$CWD" || return
}
