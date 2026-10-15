#!/bin/sh

# Run both processes as one job. Gamescope 3.16.28 on Steam Frame currently
# routes input reliably through its XWayland display, not native xdg-shell.
set -u

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
server="$root/build/linux-release/sof-server"
client="$root/build/linux-release/sof-client"
server_pid=
client_pid=

cleanup() {
    status=$?
    trap - EXIT HUP INT TERM
    if [ -n "$client_pid" ]; then
        kill -TERM "$client_pid" 2>/dev/null || true
        wait "$client_pid" 2>/dev/null || true
    fi
    if [ -n "$server_pid" ]; then
        kill -TERM "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    exit "$status"
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if [ ! -x "$server" ] || [ ! -x "$client" ]; then
    echo "Build sof-server and sof-client in build/linux-release first" >&2
    exit 1
fi
if ! command -v taskset >/dev/null 2>&1; then
    echo "taskset is required to pin inference to the performance CPU cluster" >&2
    exit 1
fi

DISPLAY=${DISPLAY:-:0}
XDG_SESSION_TYPE=x11
export DISPLAY
export XDG_SESSION_TYPE
unset WAYLAND_DISPLAY

cd "$root"
taskset -c 2-4 "$server" --model ../assets/models/lip_model.mnn &
server_pid=$!
"$client" &
client_pid=$!
wait "$client_pid"
exit $?
