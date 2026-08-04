#!/bin/zsh

script_dir=${0:A:h}
rgb_tool="${script_dir}/feker-rgb"

echo 'Testing the Alice80 number-row LED map for 60 seconds.'
sudo -v || exit $?
sudo -n "${rgb_tool}" slots \
  1=00FF4C 2=FF6D00 3=FF0033 \
  4=304FFE 5=FFFFFF 6=00FF4C \
  7=FF6D00 8=FF0033 9=304FFE
test_status=$?

echo
if [[ ${test_status} -eq 0 ]]; then
  echo 'Number-row map test completed.'
else
  echo "Number-row map test failed with status ${test_status}."
fi
read -k 1 '?Press any key to close...'
