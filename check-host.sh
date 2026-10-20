#!/bin/sh
set -eu
cd "$(dirname "$0")"
for test in tests/test_core.py tests/test_rtc.py tests/test_state_file.py tests/test_battery_file.py tests/test_game_catalog.py tests/test_session.py; do
  python3 "$test"
done
gameboy_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/gameboy-report.XXXXXX")
trap 'rm -rf "$gameboy_test_dir"' EXIT HUP INT TERM
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror tests/controller_report.cpp -o "$gameboy_test_dir/controller"
"$gameboy_test_dir/controller"
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -Iinclude tests/controller_usb.cpp -o "$gameboy_test_dir/controller_usb"
"$gameboy_test_dir/controller_usb"
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -pthread tests/controller_usb_lifecycle.cpp -o "$gameboy_test_dir/controller_usb_lifecycle"
"$gameboy_test_dir/controller_usb_lifecycle"
