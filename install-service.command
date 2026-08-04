#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
user_home=$HOME
user_id=$(id -u)
observer_label=com.chenzixin.feker-codex-bridge.observer
legacy_lights_label=com.chenzixin.feker-codex-bridge.lights
legacy_daemon_label=com.chenzixin.feker-codex-bridge.daemon
legacy_bridge_label=com.czx.feker-codex-bridge
legacy_daemon_plist="/Library/LaunchDaemons/${legacy_daemon_label}.plist"
legacy_user_daemon_plist="${user_home}/Library/LaunchAgents/${legacy_daemon_label}.plist"
legacy_lights_plist="${user_home}/Library/LaunchAgents/${legacy_lights_label}.plist"
legacy_bridge_plist="${user_home}/Library/LaunchAgents/${legacy_bridge_label}.plist"
observer_plist="${user_home}/Library/LaunchAgents/${observer_label}.plist"
helper_path=/Library/PrivilegedHelperTools/com.chenzixin.feker-codex-bridge
sudoers_path=/etc/sudoers.d/feker-codex-bridge
app_path="/Applications/Feker Codex Bridge.app"
support_dir="${user_home}/Library/Application Support/Feker Codex Bridge"
temporary_dir=$(mktemp -d)
trap 'rm -rf "$temporary_dir"' EXIT

echo "Building Feker Codex Bridge…"
"${script_dir}/build.sh"
mkdir -p "${user_home}/Library/Logs" "$support_dir"
print -r -- "$(id -un) ALL=(root) NOPASSWD: /usr/bin/env HOME=${user_home} FEKER_USER_UID=${user_id} ${helper_path} --daemon" \
  > "${temporary_dir}/sudoers"

echo
echo "macOS will ask for the administrator password once."
sudo -v
pkill -TERM -f '^/Applications/Feker Codex Bridge.app/Contents/MacOS/FekerCodexBridge($| )' 2>/dev/null || true
launchctl bootout "gui/${user_id}" "$observer_plist" 2>/dev/null || true
launchctl bootout "gui/${user_id}" "$legacy_lights_plist" 2>/dev/null || true
launchctl bootout "gui/${user_id}" "$legacy_user_daemon_plist" 2>/dev/null || true
launchctl bootout "gui/${user_id}" "$legacy_bridge_plist" 2>/dev/null || true
sudo -n mkdir -p /Library/PrivilegedHelperTools
sudo -n ditto "${script_dir}/Feker Codex Bridge.app" "$app_path"
sudo -n xattr -cr "$app_path"
sudo -n codesign --force --deep --sign - "$app_path"
sudo -n install -o root -g wheel -m 0755 \
  "${app_path}/Contents/MacOS/FekerCodexBridge" \
  "$helper_path"
sudo -n /usr/sbin/visudo -cf "${temporary_dir}/sudoers"
sudo -n install -o root -g wheel -m 0440 "${temporary_dir}/sudoers" "$sudoers_path"

sudo -n launchctl bootout system "$legacy_daemon_plist" 2>/dev/null || true
sudo -n rm -f "$legacy_daemon_plist"
rm -f "$legacy_lights_plist" "$legacy_user_daemon_plist" \
  "$legacy_bridge_plist" "$observer_plist"

lsregister=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
"$lsregister" -u "${script_dir}/Feker Codex Bridge.app" >/dev/null 2>&1 || true
"$lsregister" -u "$app_path" >/dev/null 2>&1 || true
"$lsregister" -f "$app_path"
tccutil reset ListenEvent io.github.chenzixin1.feker-codex-bridge >/dev/null 2>&1 || true
open -g "$app_path"

echo
echo "Installed. Future starts, mode changes, and tests no longer need sudo."
echo "Add Feker Codex Bridge in:"
echo "System Settings > General > Login Items & Extensions > Open at Login"
echo "Allow Feker Codex Bridge in:"
echo "System Settings > Privacy & Security > Input Monitoring"
echo "Then use the keyboard icon in the menu bar to choose a lighting mode or test a color."
echo
read "?Press Return to close…"
