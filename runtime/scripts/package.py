#!/usr/bin/env python3
"""Linux package entry points; invoked by the root launchers."""

import argparse
import hashlib
import json
import os
import platform
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
import uuid
from pathlib import Path

REPOSITORY = "https://github.com/steffandonal/sranipatch"
API = "https://api.github.com/repos/steffandonal/sranipatch/releases/latest"
UVC_REPOSITORY = "https://github.com/mavishtheblue/steam-frame-uvc"
ROOT = Path(__file__).resolve().parents[2]


def run(*args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def metadata(root):
    return json.loads((root / "app/package.json").read_text())


def cleanup(root):
    if (root / ".update-lock").exists():
        return
    for backup in root.glob("app.*.old"):
        if backup.is_dir() and not backup.is_symlink():
            try:
                shutil.rmtree(backup)
            except OSError:
                pass  # A loaded driver may still hold this backup open.


def openvr_paths():
    path = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config"))
    path /= "openvr/openvrpaths.vrpath"
    if not path.exists():
        return None
    return json.loads(path.read_text())


def registered(root):
    paths = openvr_paths()
    return bool(
        paths
        and any(
            Path(p).resolve() == (root / "app/driver").resolve()
            for p in paths.get("external_drivers", [])
        )
    )


def register(root, remove=False):
    if not openvr_paths():
        if remove:
            return
        raise RuntimeError("SteamVR was not found. Install and run SteamVR first.")
    run(
        "sh",
        root / "app/scripts/register-steamvr.sh",
        root / "app/driver",
        *(["--unregister"] if remove else []),
    )


def processes(root):
    found = []
    directory = root / "app/bin"
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            executable = (entry / "exe").readlink()
            if executable.parent != directory or executable.name not in (
                "sranipatch-server",
                "sranipatch-client",
            ):
                continue
            args = (entry / "cmdline").read_bytes().split(b"\0")
            args = [os.fsdecode(arg) for arg in args if arg]
            found.append(
                (int(entry.name), executable, args[1:], (entry / "cwd").readlink())
            )
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
    return found


def control(root, command, *args):
    result = run(
        root / "app/bin/sranipatch-package-control",
        command,
        *args,
        capture_output=True,
        text=True,
    )
    return (
        json.loads(
            next(
                line
                for line in reversed(result.stdout.splitlines())
                if line.startswith("{")
            )
        )
        if command == "status"
        else None
    )


def stop_processes(snapshot):
    for pid, executable, _, _ in snapshot:
        try:
            if Path(f"/proc/{pid}/exe").readlink() == executable:
                os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        except FileNotFoundError:
            pass
    deadline = time.monotonic() + 10
    while True:
        remaining = []
        for process in snapshot:
            try:
                if Path(f"/proc/{process[0]}/exe").readlink() == process[1]:
                    remaining.append(process)
            except FileNotFoundError:
                pass
        if not remaining:
            return
        if time.monotonic() >= deadline:
            raise RuntimeError("Timed out stopping SRanipatch applications")
        time.sleep(0.05)


def restore_processes(root, snapshot):
    live = {process[0] for process in processes(root)}
    for pid, executable, args, cwd in snapshot:
        if pid not in live and "--lifetime" not in args:
            subprocess.Popen(
                [str(executable), *args],
                cwd=cwd,
                start_new_session=True,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )


def confirm(question):
    # The bootstrap may have been invoked through curl | bash.
    try:
        with open("/dev/tty", "r+", encoding="utf-8") as terminal:
            terminal.write(question + " [y/N] ")
            terminal.flush()
            return terminal.readline().strip().lower() in ("y", "yes")
    except OSError:
        return False


def uvc_loaded():
    return Path("/sys/module/uvcvideo").is_dir()


def setup_uvc():
    data = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share")
    if not data.is_absolute():
        data = Path.home() / ".local/share"
    destination = data / "steam-frame-uvc"
    marker = destination / ".sranipatch-managed"

    def check_destination():
        if destination.is_symlink() or (
            destination.exists()
            and (
                not destination.is_dir()
                or marker.is_symlink()
                or not marker.is_file()
                or marker.read_text() != UVC_REPOSITORY + "\n"
            )
        ):
            raise RuntimeError(
                f"UVC setup directory is not installer-managed: {destination}"
            )

    check_destination()
    print(f"UVC source: {UVC_REPOSITORY}")
    print(f"Download location: {destination}")
    print(
        "Setup runs upstream's installer, which asks for sudo to install kernel "
        "modules and a system service. The service loads UVC at boot and may "
        "download/build modules after kernel updates."
    )
    if not confirm("Download and set up USB Video support now?"):
        raise RuntimeError(
            "USB Video setup was not approved. Install UVC support and rerun install."
        )
    if os.geteuid() == 0:
        raise RuntimeError(
            "Run install as your normal user; UVC setup asks for sudo when needed."
        )
    data.mkdir(parents=True, exist_ok=True)
    lock = data / ".sranipatch-uvc-lock"
    lock.mkdir()  # Do not replace a checkout while another installer uses it.
    try:
        check_destination()
        with tempfile.TemporaryDirectory(
            prefix=".sranipatch-uvc-", dir=data
        ) as temporary:
            temporary = Path(temporary)
            archive = temporary / "source.tar.gz"
            download(
                "https://codeload.github.com/mavishtheblue/steam-frame-uvc/tar.gz/HEAD",
                archive,
            )
            staged = temporary / "source"
            staged.mkdir()
            extract(archive, staged)
            entries = list(staged.iterdir())
            if len(entries) != 1 or not entries[0].is_dir():
                raise RuntimeError("Unexpected UVC source archive layout")
            source = entries[0]
            for name in ("install.sh", "uninstall.sh", "src/build.sh", "src/load.sh"):
                script = source / name
                if not script.is_file():
                    raise RuntimeError(f"UVC source is missing {name}")
                script.chmod(script.stat().st_mode | 0o111)
            (source / marker.name).write_text(UVC_REPOSITORY + "\n")
            if destination.exists():
                shutil.rmtree(destination)
            source.rename(destination)
        run("bash", destination / "install.sh", cwd=destination)
    except subprocess.CalledProcessError as error:
        raise RuntimeError(
            "UVC setup failed; see upstream's output above. Resolve the reported "
            "problem (including any requested SteamOS update/reboot), then rerun install."
        ) from error
    finally:
        lock.rmdir()


def ensure_uvc():
    if uvc_loaded():
        return
    print("USB Video (UVC) support is required to access the facial tracker's camera.")
    print(f"Steam Frame setup: {UVC_REPOSITORY}")
    modinfo = shutil.which("modinfo")
    if (
        modinfo
        and subprocess.run(
            [modinfo, "-k", os.uname().release, "uvcvideo"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        ).returncode
        == 0
    ):
        print("This kernel already provides the UVC driver; loading it requires sudo.")
        if not confirm("Load the existing USB Video driver now?"):
            raise RuntimeError(
                "UVC driver loading was not approved. Load uvcvideo and rerun install."
            )
        run("sudo", "modprobe", "uvcvideo")
    else:
        if (
            os.uname().machine != "aarch64"
            or platform.freedesktop_os_release().get("ID") != "steamos"
        ):
            raise RuntimeError(
                "Automatic UVC setup requires SteamOS ARM64. Install your distribution's "
                "uvcvideo kernel driver, load it with sudo modprobe uvcvideo, and rerun install."
            )
        setup_uvc()
    if not uvc_loaded():
        raise RuntimeError(
            "UVC driver is still unavailable. Resolve the setup error and rerun install."
        )


def offer_restart(no_restart):
    if no_restart or not steamvr_running():
        return
    print(
        "On Steam Frame, restarting SteamVR will make the display go black temporarily."
    )
    # curl | bash still needs to read the answer from the user's terminal.
    try:
        with open("/dev/tty", "r+", encoding="utf-8") as terminal:
            terminal.write("Restart SteamVR now? [y/N] ")
            terminal.flush()
            answer = terminal.readline().strip().lower()
    except OSError:
        print("Restart SteamVR when convenient to apply the driver change.")
        return
    if answer not in ("y", "yes"):
        return
    attempts = [
        ["systemctl", "--user", "--no-block", "restart", "steamvr.service"],
        [
            "busctl",
            "--user",
            "call",
            "org.freedesktop.systemd1",
            "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager",
            "RestartUnit",
            "ss",
            "steamvr.service",
            "replace",
        ],
        [
            "dbus-send",
            "--session",
            "--print-reply",
            "--dest=org.freedesktop.systemd1",
            "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager.RestartUnit",
            "string:steamvr.service",
            "string:replace",
        ],
    ]
    if Path("/run/host/usr/bin/systemctl").exists():
        attempts.insert(
            1,
            [
                "/run/host/usr/lib/ld-linux-aarch64.so.1",
                "--library-path",
                "/run/host/usr/lib:/run/host/usr/lib/systemd:/run/host/usr/lib64:/run/host/lib",
                "/run/host/usr/bin/systemctl",
                "--user",
                "--no-block",
                "restart",
                "steamvr.service",
            ],
        )
    for args in attempts:
        try:
            if subprocess.run(args, check=False).returncode == 0:
                return
        except FileNotFoundError:
            continue
    raise RuntimeError(
        "Could not restart SteamVR automatically. Please restart it manually."
    )


def steamvr_running():
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if (entry / "comm").read_text().strip() == "vrserver":
                return True
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
    return False


def download(url, destination):
    request = urllib.request.Request(url, headers={"User-Agent": "sranipatch-updater"})
    with (
        urllib.request.urlopen(request, timeout=60) as response,
        destination.open("wb") as output,
    ):
        shutil.copyfileobj(response, output)


def fetch_release(directory, platform):
    response = directory / "release.json"
    download(API, response)
    release = json.loads(response.read_text())
    filename = f"SRanipatch-Runtime.{platform}.tar.gz"
    assets = {
        asset["name"]: asset["browser_download_url"] for asset in release["assets"]
    }
    archive = directory / filename
    download(assets[filename], archive)
    checksum = directory / "checksum"
    download(assets[filename + ".sha256"], checksum)
    expected = checksum.read_text().split()[0].lower()
    if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
        raise RuntimeError("Release checksum does not match")
    return archive, release["tag_name"]


def extract(archive, destination):
    with tarfile.open(archive) as package:
        for member in package.getmembers():
            target = (destination / member.name).resolve()
            if not target.is_relative_to(destination.resolve()) or not (
                member.isfile() or member.isdir()
            ):
                raise RuntimeError("Unsafe entry in release archive")
        package.extractall(
            destination, filter="data"
        )  # Members were restricted to ordinary files/directories above.


def validate(root, platform):
    info = metadata(root)
    if info["platform"] != platform or not isinstance(info["driver_revision"], int):
        raise RuntimeError("Release does not match this platform")
    for name in (
        "app/bin/sranipatch-server",
        "app/bin/sranipatch-client",
        "app/bin/sranipatch-package-control",
        "app/driver/driver.vrdrivermanifest",
        "start-server",
        "start-client",
        "install",
        "uninstall",
        "update",
        "github",
    ):
        if not (root / name).is_file():
            raise RuntimeError(f"Release is missing {name}")
    return info


def apply_update(root, staged):
    was_registered = registered(root)
    state = control(root, "status")
    snapshot = processes(root)
    backup = root / f"app.{uuid.uuid4().hex}.old"
    moved = False
    try:
        if state["active"]:
            control(root, "pause")
        stop_processes(snapshot)
        (root / "app").rename(backup)
        moved = True
        try:
            (staged / "app").rename(root / "app")
        except OSError:
            backup.rename(root / "app")
            moved = False
            raise
        for name in (
            "start-server",
            "start-client",
            "install",
            "uninstall",
            "update",
            "github",
        ):
            shutil.copy2(staged / name, root / name)
        (root / "start").unlink(missing_ok=True)
        if was_registered:
            register(root)
    finally:
        if (root / "app").is_dir():
            # Managed processes are relaunched by the driver, not by this script.
            manual = [
                p
                for p in snapshot
                if not (state["managed"] and p[1].name == "sranipatch-server")
            ]
            try:
                restore_processes(root, manual)
            finally:
                if state["active"]:
                    control(root, "resume", "1" if state["run"] else "0")
    return state, moved


def update(root, archive=None, no_restart=False):
    lock = root / ".update-lock"
    lock.mkdir()  # Concurrent updates must not interleave directory swaps.
    try:
        cleanup(root)
        with tempfile.TemporaryDirectory(prefix=".update-", dir=root) as temporary:
            temporary = Path(temporary)
            current = metadata(root)
            release_tag = None
            if archive is None:
                archive, release_tag = fetch_release(temporary, current["platform"])
                if release_tag == current["version"]:
                    print("SRanipatch is already up to date.")
                    return
            staged = temporary / "package"
            staged.mkdir()
            extract(archive, staged)
            incoming = validate(staged, current["platform"])
            if release_tag and incoming["version"] != release_tag:
                raise RuntimeError("Release metadata does not match its tag")
            if incoming["version"] == current["version"]:
                print("SRanipatch is already up to date.")
                return
            state, _ = apply_update(root, staged)
            print(f"Updated SRanipatch to {incoming['version']}.")
            if state["active"] and state["revision"] != incoming["driver_revision"]:
                offer_restart(no_restart)
    finally:
        lock.rmdir()


def main():
    if len(sys.argv) > 1 and sys.argv[1] in ("start-server", "start-client"):
        cleanup(ROOT)
        application = sys.argv[1].removeprefix("start-")
        executable = ROOT / f"app/bin/sranipatch-{application}"
        os.execv(executable, [str(executable), *sys.argv[2:]])
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "command",
        choices=(
            "start-server",
            "start-client",
            "install",
            "uninstall",
            "update",
            "github",
        ),
    )
    parser.add_argument("--no-restart", action="store_true")
    parser.add_argument(
        "--archive", type=Path, help="Update from a local release archive"
    )
    options, arguments = parser.parse_known_args()
    root = ROOT
    if arguments:
        parser.error("unrecognized arguments: " + " ".join(arguments))
    cleanup(root)
    if options.command == "github":
        run("xdg-open", REPOSITORY)
    elif options.command == "install":
        ensure_uvc()
        register(root)
        print("Registered SRanipatch. SteamVR will load it on its next start.")
        offer_restart(options.no_restart)
    elif options.command == "uninstall":
        register(root, remove=True)
        print("You may now delete this folder to finish uninstalling")
    else:
        update(root, options.archive, options.no_restart)


if __name__ == "__main__":
    try:
        main()
    except (
        OSError,
        ValueError,
        KeyError,
        RuntimeError,
        subprocess.CalledProcessError,
        tarfile.TarError,
    ) as error:
        print(f"SRanipatch: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr, file=sys.stderr)
        sys.exit(1)
