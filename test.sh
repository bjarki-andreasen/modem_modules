#!/bin/bash

set -e # Fail immediately if any command exits with a non-zero status

APPS=("modem_cmux" "modem_ppp" "modem_chat" "modem_backend_tty")
BUILD_APPS=("modem_e2e")
ZEPHYR_EXE="./build/zephyr/zephyr.exe"

for APP in "${APPS[@]}"
do
  echo "Building test: $APP"
  west build -p -b native_posix "modem_modules/tests/subsys/modem/$APP" > /dev/null 2>&1
  echo "Running test: $APP"
  $ZEPHYR_EXE
done

for APP in "${BUILD_APPS[@]}"
do
  echo "Building test: $APP"
  west build -p -b native_posix "modem_modules/tests/subsys/modem/$APP" > /dev/null 2>&1
  echo "Success"
done

# Samples
echo "Building sample: cmux_ppp"
west build -p -b b_u585i_iot02a "modem_modules/samples/subsys/modem/cmux_ppp" > /dev/null 2>&1
echo "Success"

echo "Building sample: cmux_ppp_tty"
west build -p -b native_posix "modem_modules/samples/subsys/modem/cmux_ppp_tty" > /dev/null 2>&1
echo "Success"