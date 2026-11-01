#!/bin/sh
set -eu
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    printf '%s\n' 'Usage: register-steamvr.sh DRIVER_PATH [--unregister]' >&2
    exit 2
fi
driver_root=$(CDPATH= cd -- "$1" && pwd -P)
test -f "$driver_root/driver.vrdrivermanifest"
operation=adddriver
if [ "$#" -eq 2 ]; then
    test "$2" = --unregister
    operation=removedriver
fi
paths="${XDG_CONFIG_HOME:-$HOME/.config}/openvr/openvrpaths.vrpath"
runtime=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["runtime"][0])' "$paths")
case "$(uname -m)" in
    aarch64|arm64) platform=linuxarm64 ;;
    x86_64) platform=linux64 ;;
    *) printf '%s\n' 'Unsupported SteamVR platform' >&2; exit 1 ;;
esac
vrpathreg="$runtime/bin/$platform/vrpathreg"
# vrpathreg opens an optional desktop notification after registration.
if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    BROWSER=true
    export BROWSER
fi
exec "$vrpathreg" "$operation" "$driver_root"
