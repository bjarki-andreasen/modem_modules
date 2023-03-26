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
done
