#!/usr/bin/env bash
# run.sh — build, load, and reset the sdc41 firmware over SWD.
#
# This board's flash write path does not work (diagnosed: every erase/program
# operation times out, reads and RAM loading are fine). So "flashing" here
# means loading straight into SRAM and jumping the CPU there — it does not
# persist across power loss, and must be reloaded after every reset.
#
# Usage:
#   ./run.sh          build + load + reset
#   ./run.sh build     build only
#   ./run.sh load      load the existing build, no rebuild
#   ./run.sh console   just open picocom, nothing else

set -euo pipefail
cd "$(dirname "$0")"

PORT=/dev/ttyACM0
BAUD=115200
ADAPTER_SPEED=100
ELF=build/sdc41_ram.elf
ENTRY=0x20000001

do_build() {
    echo "== building =="
    cmake -S . -B build >/dev/null
    cmake --build build -j4
    echo "== build ok: $(ls -la "$ELF" | awk '{print $5, $NF}') =="
}

do_load() {
    echo "== loading via SWD, resetting target =="
    openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg \
        -c "adapter speed ${ADAPTER_SPEED}" \
        -c "init" \
        -c "reset halt" \
        -c "load_image ${ELF}" \
        -c "resume ${ENTRY}" \
        -c "sleep 1000" \
        -c "shutdown"
    echo "== loaded, target running =="
}

do_console() {
    echo "== opening console on ${PORT} — Ctrl-A Ctrl-X to exit =="
    picocom -b "${BAUD}" --omap crlf "${PORT}"
}

case "${1:-all}" in
    build)   do_build ;;
    load)    do_load ;;
    console) do_console ;;
    all)
        do_build
        echo
        echo ">>> Reloading in 3 seconds. Open a second terminal now and run:"
        echo ">>>   ./run.sh console"
        echo ">>> if you haven't already, to catch the boot output."
        sleep 3
        do_load
        ;;
    *)
        echo "usage: $0 [build|load|console|all]"
        exit 1
        ;;
esac
