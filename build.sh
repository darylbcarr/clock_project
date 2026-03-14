#!/usr/bin/env bash
# Convenience wrapper — sets up ESP-IDF environment then forwards all args to idf.py
# Usage:  ./build.sh build
#         ./build.sh -p /dev/ttyUSB0 flash monitor
#         ./build.sh menuconfig

set -e

IDF_PATH=/home/daryl/esp/esp-idf
TOOLCHAIN=/home/daryl/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20241119/xtensa-esp-elf/bin
PYTHON=/home/daryl/.espressif/python_env/idf5.4_py3.12_env/bin/python3
ROM_ELFS=/home/daryl/.espressif/tools/esp-rom-elfs/20241011

export IDF_PATH
export ESP_ROM_ELF_DIR=$ROM_ELFS
export PATH="$TOOLCHAIN:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

exec "$PYTHON" "$IDF_PATH/tools/idf.py" "$@"
