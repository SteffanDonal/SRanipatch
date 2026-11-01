#!/bin/sh

# Start a manually owned tracking server on the performance CPU cluster.
set -u

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
server="$root/build/linux-release/sranipatch-server"
if [ ! -x "$server" ]; then
    echo "Build sranipatch-server in build/linux-release first" >&2
    exit 1
fi
if ! command -v taskset >/dev/null 2>&1; then
    echo "taskset is required to pin inference to the performance CPU cluster" >&2
    exit 1
fi

exec taskset -c 2-4 "$server" "$@"
