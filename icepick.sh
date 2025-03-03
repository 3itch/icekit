#!/bin/bash

# ICEPICK kernel module manager
# Usage: ./icepick.sh [build|load|test|unload|clean]

BUILD_DIR="./build"
MODULE="$BUILD_DIR/icepick.ko"
TEST="$BUILD_DIR/icepick_test"
DEVICE="/dev/cachemem"
KDIR="${KDIR:-/usr/src/linux-source-6.11.0}"

check_root() {
  [ "$EUID" -ne 0 ] && { echo "[ICEPICK] [!] root required"; exit 1; }
}

build() {
  [ ! -d "$KDIR" ] && { echo "[ICEPICK] [!]     no kernel source at $KDIR"; exit 1; }
  make clean >/dev/null 2>&1
  make >/dev/null 2>&1 || make >/dev/null 2>&1 #      use bear -- make instead if available
  [ -f "$MODULE" ] && [ -f "$TEST" ] || { echo "[ICEPICK] [!]     build failed"; exit 1; }
}

load() {
  check_root
  [ -f "$MODULE" ] || { echo "[ICEPICK] [!]     no module. run build"; exit 1; }
  insmod "$MODULE" && chmod 666 "$DEVICE" || { echo "[ICEPICK] [!]      load failed"; exit 1; }
}

test() {
  [ -f "$TEST" ] || { echo "[ICEPICK] [!]     no test. run build"; exit 1; }
  [ -c "$DEVICE" ] || { echo "[ICEPICK] [!]     no device. run load"; exit 1; }
  sudo "$TEST" || { echo "[ICEPICK] [!]     test failed"; exit 1; }
}

unload() {
  check_root
  lsmod | grep -q icepick && rmmod icepick || true
}

clean() {
  make clean >/dev/null 2>&1
  rm -f ./compile_commands.json
}

case "$1" in
  build) build ;;
  load) load ;;
  test) test ;;
  unload) unload ;;
  clean) clean ;;
  *)
    echo "[ICEPICK] usage: $0 [build|load|test|unload|clean]"
    exit 1
    ;;
esac

exit 0
