#!/bin/zsh

script_dir=${0:A:h}
rgb_tool="${script_dir}/feker-rgb"

echo 'Test 1/3: FEKER live RGB command 12, all LED slots RED.'
sudo "${rgb_tool}" all FF0000
test_status=$?
echo 'Observe the keyboard, then press any key for test 2.'
read -k 1
echo

echo 'Test 2/3: only number key 1 GREEN; all other LED slots OFF.'
sudo "${rgb_tool}" key 1 00FF00
(( test_status |= $? ))
echo 'Observe the keyboard, then press any key for test 3.'
read -k 1
echo

echo 'Test 3/3: number keys 1/2/3 GREEN, AMBER, RED.'
sudo "${rgb_tool}" slots 1=00FF00 2=FFBF00 3=FF0000
(( test_status |= $? ))

echo
if [[ ${test_status} -eq 0 ]]; then
  echo 'All three FEKER live-RGB command sequences were acknowledged.'
else
  echo "FEKER RGB root test failed with status ${test_status}."
fi
echo 'You can close this window after reporting the result.'
read -k 1 '?Press any key to close...'
