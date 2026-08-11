#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_dir=${script_dir:h}
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

cc -x objective-c -std=c11 -Wall -Wextra -Werror \
  "${script_dir}/thread_mapping_test.m" \
  $(pkg-config --cflags --libs hidapi sqlite3) \
  -framework AppKit \
  -framework QuartzCore \
  -framework ServiceManagement \
  -o "${work_dir}/thread-mapping-test"

"${work_dir}/thread-mapping-test"
