#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
user_home=$HOME
app_path="/Applications/Feker Codex Bridge.app"
support_dir="${user_home}/Library/Application Support/Feker Codex Bridge"

echo "Building Feker Codex Bridge…"
"${script_dir}/build.sh"
mkdir -p "${user_home}/Library/Logs" "$support_dir"
rm -f "${support_dir}/lighting-mode.txt"
pkill -TERM -f '^/Applications/Feker Codex Bridge.app/Contents/MacOS/FekerCodexBridge($| )' 2>/dev/null || true
ditto "${script_dir}/Feker Codex Bridge.app" "$app_path"
xattr -cr "$app_path"
codesign --force --deep --sign - "$app_path"

lsregister=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
"$lsregister" -u "${script_dir}/Feker Codex Bridge.app" >/dev/null 2>&1 || true
"$lsregister" -u "$app_path" >/dev/null 2>&1 || true
"$lsregister" -f "$app_path"
open -g "$app_path"

echo
echo "Installed. FEKER QMK/VIA lighting runs without a privileged helper."
echo "Right-click the menu bar icon, open Settings, and enable Start at Login."
echo "Use the keyboard icon in the menu bar to test a whole-board color."
echo
read "?Press Return to close…"
