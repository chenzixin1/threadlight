#!/bin/zsh
set -euo pipefail

app_binary="/Applications/Feker Codex Bridge.app/Contents/MacOS/FekerCodexBridge"
if [[ ! -x "$app_binary" ]]; then
  echo "Feker Codex Bridge is not installed. Run install-service.command first."
  read "?Press Return to close…"
  exit 1
fi

"$app_binary" --request-test-key 1 unread
echo "A green test runs for 30 seconds: number 1 in per-key mode, or the whole keyboard in traffic-light mode."
echo "No administrator password is needed."
read "?Press Return to close…"
