#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
VERSION_FILE="$REPO_ROOT/app/VERSION"
BUILD_DIR="$REPO_ROOT/build/app"
BOOT_IMAGE="$BUILD_DIR/mcuboot/zephyr/zephyr.bin"
UPDATE_IMAGE="$BUILD_DIR/app/zephyr/zephyr.signed.bin"
SLOT0_OFFSET=$((0x14000))

version_value() {
  local key="$1"

  awk -F= -v key="$key" \
    '$1 ~ "^[[:space:]]*" key "[[:space:]]*$" {
      value = $2
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", value)
      print value
    }' "$VERSION_FILE"
}

if [[ ! -f "$VERSION_FILE" ]]; then
  echo "Error: VERSION file not found: $VERSION_FILE" >&2
  exit 1
fi

major="$(version_value VERSION_MAJOR)"
minor="$(version_value VERSION_MINOR)"
patch="$(version_value PATCHLEVEL)"
tweak="$(version_value VERSION_TWEAK)"
extra="$(version_value EXTRAVERSION)"

if [[ -z "$major" || -z "$minor" || -z "$patch" || -z "$tweak" ]]; then
  echo "Error: app/VERSION is incomplete." >&2
  exit 1
fi

version="$major.$minor.$patch"
if [[ "$tweak" != "0" ]]; then
  version="$version.$tweak"
fi
if [[ -n "$extra" ]]; then
  version="$version-$extra"
fi

if [[ "${1:-}" != "--no-build" ]]; then
  cd "$REPO_ROOT"
  source "$REPO_ROOT/flex_env.sh"
  flex_build_n547
elif [[ $# -gt 1 ]]; then
  echo "Usage: $0 [--no-build]" >&2
  exit 2
fi

if [[ ! -f "$BOOT_IMAGE" || ! -f "$UPDATE_IMAGE" ]]; then
  echo "Error: release artifacts are missing; run flex_build_n547 first." >&2
  exit 1
fi
if ! command -v objcopy >/dev/null 2>&1; then
  echo "Error: objcopy is required to create the full image." >&2
  exit 127
fi

release_name="edison_pmi_v$version"
release_dir="$REPO_ROOT/release/$release_name"
full_name="edison_pmi_full_v$version.bin"
update_name="edison_pmi_update_v$version.bin"

rm -rf -- "$release_dir"
mkdir -p -- "$release_dir"

objcopy -I binary -O binary --gap-fill 0xff --pad-to "$SLOT0_OFFSET" \
  "$BOOT_IMAGE" "$release_dir/$full_name"
cat "$UPDATE_IMAGE" >> "$release_dir/$full_name"
cp -- "$UPDATE_IMAGE" "$release_dir/$update_name"

cat > "$release_dir/README.md" <<EOF
# Edison PMI firmware v$version

## Files

- \`$full_name\`: MCUboot at \`0x00000000\` and the signed application at
  \`0x00014000\`. Use this image for initial programming at flash address 0.
- \`$update_name\`: signed application-only image for PMI Ethernet update.

## PMI Ethernet update

\`\`\`bash
curl -X POST --data-binary @$update_name http://<board-ip>/pmi/image
curl -X POST -d "action=update" http://<board-ip>/pmi/update
curl http://<board-ip>/pmi/update
curl -X POST http://<board-ip>/pmi/reboot
\`\`\`

Wait for \`"status":"ready"\`, \`"percent":100\`, and
\`"reboot_required":true\` before rebooting.
EOF

echo "Release created: $release_dir"
echo "  $full_name"
echo "  $update_name"
echo "  README.md"
