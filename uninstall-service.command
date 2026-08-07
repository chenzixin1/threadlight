#!/bin/zsh
set -euo pipefail

user_id=$(id -u)
legacy_lights_label=com.chenzixin.feker-codex-bridge.lights
legacy_daemon_label=com.chenzixin.feker-codex-bridge.daemon
legacy_bridge_label=com.czx.feker-codex-bridge
legacy_daemon_plist="/Library/LaunchDaemons/${legacy_daemon_label}.plist"
legacy_user_daemon_plist="$HOME/Library/LaunchAgents/${legacy_daemon_label}.plist"
legacy_lights_plist="$HOME/Library/LaunchAgents/${legacy_lights_label}.plist"
legacy_bridge_plist="$HOME/Library/LaunchAgents/${legacy_bridge_label}.plist"
helper_path=/Library/PrivilegedHelperTools/com.chenzixin.feker-codex-bridge
sudoers_path=/etc/sudoers.d/feker-codex-bridge
app_path="/Applications/Threadlight.app"
legacy_app_path="/Applications/Feker Codex Bridge.app"

echo "Removing Threadlight and legacy background services…"
sudo -v
pkill -TERM -f '^/Applications/Threadlight.app/Contents/MacOS/Threadlight($| )' 2>/dev/null || true
pkill -TERM -f '^/Applications/Feker Codex Bridge.app/Contents/MacOS/FekerCodexBridge($| )' 2>/dev/null || true
sudo -n launchctl bootout system "$legacy_daemon_plist" 2>/dev/null || true
launchctl bootout "gui/${user_id}" "$legacy_lights_plist" 2>/dev/null || true
launchctl bootout "gui/${user_id}" "$legacy_user_daemon_plist" 2>/dev/null || true
launchctl bootout "gui/${user_id}" "$legacy_bridge_plist" 2>/dev/null || true
sudo -n rm -f "$legacy_daemon_plist" "$helper_path" "$sudoers_path"
sudo -n rm -rf "$app_path" "$legacy_app_path"
rm -f "$legacy_lights_plist" "$legacy_user_daemon_plist" \
  "$legacy_bridge_plist"
rm -f "$HOME/Library/Application Support/Feker Codex Bridge/FekerServiceLauncher"
rm -f "$HOME/Library/Application Support/Feker Codex Bridge/run-light-service.zsh"

echo "App, helper, sudoers rule, and legacy background services removed."
echo "The task-light preference and log were kept."
echo "If macOS still shows a stale Login Item, remove it in System Settings."
read "?Press Return to close…"
