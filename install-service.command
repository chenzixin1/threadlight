#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
user_home=$HOME
app_path="/Applications/Threadlight.app"
legacy_app_path="/Applications/Feker Codex Bridge.app"
legacy_app_trash="${user_home}/.Trash/Feker Codex Bridge.app"
support_dir="${user_home}/Library/Application Support/Threadlight"
legacy_support_dir="${user_home}/Library/Application Support/Feker Codex Bridge"

echo "Building Threadlight…"
"${script_dir}/build.sh"
if [[ ! -e "$support_dir" && -d "$legacy_support_dir" ]]; then
  mv "$legacy_support_dir" "$support_dir"
fi
mkdir -p "${user_home}/Library/Logs" "$support_dir"
rm -f "${support_dir}/lighting-mode.txt" \
  "${support_dir}/FekerServiceLauncher" \
  "${support_dir}/run-light-service.zsh"
pkill -TERM -f '^/Applications/Threadlight.app/Contents/MacOS/Threadlight($| )' 2>/dev/null || true
pkill -TERM -f '^/Applications/Feker Codex Bridge.app/Contents/MacOS/FekerCodexBridge($| )' 2>/dev/null || true
ditto "${script_dir}/Threadlight.app" "$app_path"
xattr -cr "$app_path"
codesign --force --deep --sign - "$app_path"

lsregister=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
"$lsregister" -u "${script_dir}/Threadlight.app" >/dev/null 2>&1 || true
"$lsregister" -u "$legacy_app_path" >/dev/null 2>&1 || true
"$lsregister" -u "$app_path" >/dev/null 2>&1 || true
"$lsregister" -f "$app_path"
if [[ -d "$legacy_app_path" ]]; then
  if [[ ! -e "$legacy_app_trash" ]]; then
    mv "$legacy_app_path" "$legacy_app_trash"
  else
    echo "Legacy app kept at $legacy_app_path because a recoverable copy already exists in Trash."
  fi
fi
open -g "$app_path"

echo
echo "Installed Threadlight. FEKER QMK/VIA lighting runs without a privileged helper."
echo "Right-click the menu bar icon, open Settings, and enable Start at Login."
echo "Open Light Settings from the keyboard icon to choose Whole Keyboard or Number Keys 1–9."
echo
read "?Press Return to close…"
