#!/bin/zsh
set -euo pipefail

app_binary="/Applications/Feker Codex Bridge.app/Contents/MacOS/FekerCodexBridge"
if [[ ! -x "$app_binary" ]]; then
  echo "Feker Codex Bridge is not installed. Run install-service.command first."
  read "?Press Return to close…"
  exit 1
fi

"$app_binary" --request-test complete
echo "The whole keyboard shows green for 30 seconds."
echo "No administrator password is needed."
read "?Press Return to close…"
