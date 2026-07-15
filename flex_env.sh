#!/usr/bin/env bash

# This script must be sourced:
#   source ./flex_env.sh

CWD="$PWD"
ZEPHYR_WORKSPACE="${ZEPHYR_WORKSPACE:-$HOME/Music/rtos/zephyr}"
ZEPHYR_USER_CACHE="${ZEPHYR_USER_CACHE:-$CWD/.cache/zephyr}"

source "$ZEPHYR_WORKSPACE/.venv/bin/activate"
source "$ZEPHYR_WORKSPACE/zephyr/zephyr-env.sh"

flex_build_n947() {
  cbb
  west build -p always -b frdm_mcxn947/mcxn947/cpu0 samples/basic/blinky
}

flex_build_n547() {
  west build -p always \
    -b mcx_n5xx_evk/mcxn547/cpu0 \
    -s "$CWD/app" \
    -d "$CWD/build/app" \
    -- -DUSER_CACHE_DIR="$ZEPHYR_USER_CACHE"
}

flex_clean_build() {
  rm -rf -- "$CWD/build"
}

flex_flash_image() {
  west flash -d "$CWD/build/app"
}

cbb() {
  cd "$CWD" || return
}


