#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .pio/native-tests
c++ -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -Iinclude -I.pio/libdeps/esp32dev/ArduinoJson/src \
  test/native/test_main.cpp src/config_codec.cpp src/macro_runner.cpp \
  -o .pio/native-tests/test_main
ASAN_OPTIONS=detect_leaks=1 .pio/native-tests/test_main "$@"
