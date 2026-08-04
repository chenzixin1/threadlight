#!/bin/zsh

script_dir=${0:A:h}
rgb_tool="${script_dir}/feker-rgb"

echo 'Testing only the number 1 LED in green for 60 seconds.'
sudo -v || exit $?
sudo -n "${rgb_tool}" key 1 00FF4C
test_status=$?

echo
if [[ ${test_status} -eq 0 ]]; then
  echo 'Single-number test completed.'
else
  echo "Single-number test failed with status ${test_status}."
fi
read -k 1 '?Press any key to close...'
