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

echo "Built ${script_dir}/feker-rgb"
echo "Built ${script_dir}/Feker Codex Bridge.app"
