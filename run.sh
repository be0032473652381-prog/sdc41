#!/usr/bin/env bash
# run.sh — build, load, and reset the sdc41 firmware over SWD.
#
# This board's flash write path does not work (diagnosed: every erase/program
# operation times out; reads and RAM loading are fine). There is no true
# "flash" on this board — nothing persists, and a bare SWD reset without
# reloading will leave the target running nothing at all (this happened
# once already: a plain "reset run" outside this script wiped the firmware,
# because it sent the CPU back to a boot ROM with no valid flash image to
# find). Every command below that touches hardware always does the FULL
# build+load+resume sequence — there is deliberately no standalone "just
# reset" option, to prevent repeating that mistake.
#
# "flash" and "reboot" below are aliases for the same thing as "all" —
# they exist so you can use the vocabulary that matches a normal board,
# even though what actually happens is a RAM load, not a flash write.
#
# Usage:
#   ./run.sh            build + load + reset   (default)
#   ./run.sh flash       same as above — alias, for normal-board vocabulary
#   ./run.sh reboot       same as above — alias
#   ./run.sh build        build only, no hardware touched
#   ./run.sh load         load the existing build, no rebuild
#   ./run.sh console      just open picocom
#   ./run.sh status       show git state and whether the build is stale
#   ./run.sh kill          kill any stuck openocd/picocom holding the port

set -uo pipefail
cd "$(dirname "$0")"

PORT=/dev/ttyACM0
BAUD=115200
ADAPTER_SPEED=100
ELF=build/sdc41_ram.elf
ENTRY=0x20000001

# ---- helpers --------------------------------------------------------------

git_state() {
    local hash dirty
    hash=$(git rev-parse --short HEAD 2>/dev/null || echo "no-git")
    if [ -n "$(git status --short 2>/dev/null)" ]; then
        dirty=" (uncommitted changes present)"
    else
        dirty=""
    fi
    echo "commit ${hash}${dirty}"
}

check_probe() {
    if [ ! -e "$PORT" ]; then
        echo "!! ${PORT} not found. Is the Debug Probe attached in UTM's USB menu?"
        return 1
    fi
    if ! lsusb 2>/dev/null | grep -q "2e8a:000c"; then
        echo "!! Debug Probe not seen on USB (2e8a:000c). Reattach it and retry."
        return 1
    fi
    return 0
}

kill_stale() {
    pkill -f "openocd.*rp2040" 2>/dev/null && echo "-- killed a stale openocd" || true
    pkill picocom 2>/dev/null && echo "-- killed a stale picocom" || true
    sleep 1
}

do_build() {
    echo "== building — $(git_state) =="
    cmake -S . -B build >/dev/null
    if ! cmake --build build -j4; then
        echo "!! build failed — nothing was reloaded, previous binary on the"
        echo "   board (if any) is still whatever ran last"
        return 1
    fi
    echo "== build ok =="
    ls -la "$ELF"
}

do_status() {
    echo "== git =="
    git_state
    git log --oneline -3 2>/dev/null
    echo
    echo "== build =="
    if [ -f "$ELF" ]; then
        ls -la "$ELF"
        newest_src=$(find src -name '*.c' -newer "$ELF" 2>/dev/null | head -1)
        if [ -n "$newest_src" ]; then
            echo "!! STALE: ${newest_src} is newer than the built ELF."
            echo "   Run './run.sh build' before loading, or you will test old code."
        else
            echo "-- build is at least as new as all source files"
        fi
    else
        echo "no build yet — run './run.sh build'"
    fi
    echo
    echo "== hardware =="
    check_probe && echo "-- probe visible on USB, ${PORT} present"
}

do_load() {
    if [ ! -f "$ELF" ]; then
        echo "!! ${ELF} does not exist. Run './run.sh build' first."
        return 1
    fi
    check_probe || return 1

    echo "== loading via SWD, resetting target — $(git_state) =="
    echo "   (this is the RAM-load 'flash' equivalent for this board — see"
    echo "    the note at the top of this script for why)"
    if ! openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg \
        -c "adapter speed ${ADAPTER_SPEED}" \
        -c "init" \
        -c "reset halt" \
        -c "load_image ${ELF}" \
        -c "resume ${ENTRY}" \
        -c "sleep 1000" \
        -c "shutdown"
    then
        echo "!! load failed. Common causes on this bench:"
        echo "   - a previous openocd process still holds the probe: try './run.sh kill'"
        echo "   - SWD link is marginal: retry once, it has recovered before"
        return 1
    fi
    echo "== loaded, target running =="
}

do_console() {
    check_probe || return 1
    echo "== console on ${PORT} — Ctrl-A Ctrl-X to exit =="
    picocom -b "${BAUD}" --omap crlf "${PORT}"
}

do_all() {
    do_build || exit 1
    echo
    echo ">>> If you don't already have './run.sh console' open in another"
    echo ">>> terminal, open it now to catch the boot output. Loading in 3s."
    sleep 3
    do_load
}

# ---- entry ------------------------------------------------------------

case "${1:-all}" in
    build)   do_build ;;
    load)    do_load ;;
    console) do_console ;;
    status)  do_status ;;
    kill)    kill_stale ;;
    all|flash|reboot) do_all ;;
    *)
        echo "usage: $0 [build|load|console|status|kill|flash|reboot|all]"
        exit 1
        ;;
esac
