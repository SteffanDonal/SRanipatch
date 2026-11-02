#!/bin/bash
set -euo pipefail

destination="${HOME}/.local/share/sranipatch"
restart=()
while (($#)); do
    case "$1" in
        --dir) destination="${2:?--dir requires a directory}"; shift 2 ;;
        --no-restart) restart=(--no-restart); shift ;;
        -h|--help) echo 'Usage: get.sh [--dir DIRECTORY] [--no-restart]'; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

case "$(uname -s):$(uname -m)" in
    Linux:aarch64|Linux:arm64) ;;
    *) echo 'Install on Frame requires Linux ARM64.' >&2; exit 1 ;;
esac
for command in curl python3 tar sha256sum; do
    command -v "$command" >/dev/null || { echo "Required command not found: $command" >&2; exit 1; }
done

mkdir -p -- "$(dirname -- "$destination")"
destination=$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$destination")
temporary=$(mktemp -d "$(dirname -- "$destination")/.sranipatch-download.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT
trap 'echo "SRanipatch installation failed. See the error above." >&2' ERR

curl -fsSL 'https://api.github.com/repos/steffandonal/sranipatch/releases/latest' -o "$temporary/release.json"
python3 - "$temporary" <<'PY'
import json
from pathlib import Path
import sys
directory = Path(sys.argv[1])
release = json.loads((directory / 'release.json').read_text())
assets = {asset['name']: asset['browser_download_url'] for asset in release['assets']}
filename = 'sranipatch-linux-arm64.tar.gz'
for name in (filename, filename + '.sha256'):
    (directory / (name + '.url')).write_text(assets[name])
PY
filename=sranipatch-linux-arm64.tar.gz
curl -fsSL "$(cat "$temporary/$filename.url")" -o "$temporary/$filename"
curl -fsSL "$(cat "$temporary/$filename.sha256.url")" -o "$temporary/$filename.sha256"
expected=$(python3 -c 'import sys; print(open(sys.argv[1]).read().split()[0])' "$temporary/$filename.sha256")
actual=$(sha256sum "$temporary/$filename")
test "$expected" = "${actual%% *}" || { echo 'Release checksum does not match' >&2; exit 1; }

if [[ -f "$destination/app/package.json" ]]; then
    "$destination/update" --archive "$temporary/$filename" --no-restart
else
    if [[ -e "$destination" ]]; then
        echo "Destination already exists and is not a SRanipatch package: $destination" >&2
        exit 1
    fi
    mkdir "$temporary/package"
    python3 - "$temporary/$filename" "$temporary/package" "$temporary/release.json" <<'PY'
import json
from pathlib import Path
import sys
import tarfile
archive, destination, release_file = map(Path, sys.argv[1:])
with tarfile.open(archive) as package:
    for member in package.getmembers():
        if not (destination / member.name).resolve().is_relative_to(destination.resolve()) or not (member.isfile() or member.isdir()):
            raise RuntimeError('Unsafe entry in release archive')
    package.extractall(destination, filter="data")
info = json.loads((destination / 'app/package.json').read_text())
release = json.loads(release_file.read_text())
if info['platform'] != 'linux-arm64' or info['version'] != release['tag_name']:
    raise RuntimeError('Release metadata does not match this platform or tag')
for name in ('start-server', 'start-client', 'install', 'uninstall', 'update', 'github', 'app/bin/sranipatch-server',
             'app/bin/sranipatch-client', 'app/bin/sranipatch-package-control', 'app/driver/driver.vrdrivermanifest'):
    if not (destination / name).is_file():
        raise RuntimeError('Release is missing ' + name)
PY
    mv -- "$temporary/package" "$destination"
fi
"$destination/install" "${restart[@]}"
