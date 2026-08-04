#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
cc -std=c11 -Wall -Wextra -Werror \
  "${script_dir}/feker_rgb.c" \
  $(pkg-config --cflags --libs hidapi) \
  -o "${script_dir}/feker-rgb"

app_binary="${script_dir}/Feker Codex Bridge.app/Contents/MacOS/FekerCodexBridge"
mkdir -p "${app_binary:h}"
cc -x objective-c -std=c11 -Wall -Wextra -Werror \
  "${script_dir}/feker_codex_bridge.c" \
  $(pkg-config --cflags --libs hidapi sqlite3) \
  -framework AppKit \
  -framework ApplicationServices \
  -o "${app_binary}"

icon_source="${script_dir}/assets/FekerTaskLights.svg"
resources_dir="${script_dir}/Feker Codex Bridge.app/Contents/Resources"
icon_work_dir=$(mktemp -d)
trap 'rm -rf "$icon_work_dir"' EXIT
iconset_dir="${icon_work_dir}/AppIcon.iconset"
mkdir -p "$resources_dir" "$iconset_dir"
sips -s format png "$icon_source" --out "${icon_work_dir}/master.png" >/dev/null
for size in 16 32 128 256 512; do
  sips -z "$size" "$size" "${icon_work_dir}/master.png" \
    --out "${iconset_dir}/icon_${size}x${size}.png" >/dev/null
  double_size=$((size * 2))
  sips -z "$double_size" "$double_size" "${icon_work_dir}/master.png" \
    --out "${iconset_dir}/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$iconset_dir" -o "${resources_dir}/AppIcon.icns"

echo "Built ${script_dir}/feker-rgb"
echo "Built ${script_dir}/Feker Codex Bridge.app"
