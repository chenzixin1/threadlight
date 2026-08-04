#!/bin/zsh

script_dir=${0:A:h}
rgb_tool="${script_dir}/feker-rgb"

echo 'Running the 48-second camera-assisted LED map calibration.'
sudo -v || exit $?
sudo -n "${rgb_tool}" scan
test_status=$?

echo
if [[ ${test_status} -eq 0 ]]; then
  echo 'LED map calibration frames completed.'
else
  echo "LED map calibration failed with status ${test_status}."
fi
read -k 1 '?Press any key to close...'
