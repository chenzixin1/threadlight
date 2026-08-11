#!/bin/zsh
set -euo pipefail

app_binary="/Applications/Threadlight.app/Contents/MacOS/Threadlight"
if [[ ! -x "$app_binary" ]]; then
  echo "Threadlight is not installed. Run install-service.command first."
  read "?Press Return to close…"
  exit 1
fi

"$app_binary" --request-test complete
echo "The selected lighting scope shows solid blue for 30 seconds."
echo "Whole Keyboard lights the board; Number Keys lights 1–9."
echo "No administrator password is needed."
read "?Press Return to close…"
