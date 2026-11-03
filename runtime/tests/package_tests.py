import ctypes
import hashlib
import importlib.util
import json
import os
import select
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest
import zipfile
from pathlib import Path
from unittest.mock import Mock, mock_open, patch

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("package", REPO / "scripts/package.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)
FIXTURE = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else None
DRIVER = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else None


def make_package(root, version="v1", platform="linux-arm64"):
    (root / "app/scripts").mkdir(parents=True)
    (root / "app/bin").mkdir()
    (root / "app/driver").mkdir()
    (root / "app/driver/driver.vrdrivermanifest").write_text("{}")
    (root / "app/package.json").write_text(
        json.dumps({"version": version, "platform": platform, "driver_revision": 1})
    )
    windows = platform == "windows-x64"
    for name in ("server", "client", "package-control"):
        target = root / ("app/bin/sranipatch-" + name + (".exe" if windows else ""))
        if FIXTURE:
            shutil.copy2(FIXTURE, target)
        else:
            target.touch()
    for command in (
        "start-server",
        "start-client",
        "install",
        "uninstall",
        "update",
        "github",
    ):
        if windows and command == "github":
            shutil.copy2(REPO / "scripts/github.url", root / "github.url")
            continue
        template = "launcher.cmd.in" if windows else "launcher.sh.in"
        (root / (command + (".cmd" if windows else ""))).write_text(
            (REPO / "scripts" / template).read_text().replace("@command@", command)
        )
        if not windows:
            (root / command).chmod(0o755)
    script = "package.ps1" if windows else "package.py"
    shutil.copy2(REPO / "scripts" / script, root / "app/scripts" / script)


@unittest.skipIf(os.name == "nt", "Linux UVC prerequisite")
class LinuxUvcTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.data = Path(self.temporary.name)
        self.destination = self.data / "steam-frame-uvc"
        self.real_confirm = package.confirm
        self.enterContext(patch.dict(os.environ, {"XDG_DATA_HOME": str(self.data)}))
        self.confirm = self.enterContext(
            patch.object(package, "confirm", return_value=True)
        )
        self.loaded = self.enterContext(
            patch.object(package, "uvc_loaded", return_value=False)
        )
        self.run = self.enterContext(patch.object(package, "run"))
        self.download = self.enterContext(
            patch.object(package, "download", side_effect=self.source_archive)
        )
        self.enterContext(patch.object(package.os, "geteuid", return_value=1000))
        self.enterContext(
            patch.object(
                package.os,
                "uname",
                return_value=Mock(machine="aarch64", release="test-kernel"),
            )
        )
        self.enterContext(
            patch.object(
                package.platform,
                "freedesktop_os_release",
                return_value={"ID": "steamos"},
            )
        )
        self.modinfo = self.enterContext(
            patch.object(package.shutil, "which", return_value=None)
        )

    def source_archive(self, url, destination):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "steam-frame-uvc-latest"
            (source / "src").mkdir(parents=True)
            for name in ("install.sh", "uninstall.sh", "src/build.sh", "src/load.sh"):
                (source / name).write_text("#!/bin/bash\n")
            with tarfile.open(destination, "w:gz") as archive:
                archive.add(source, arcname=source.name)

    def test_loaded_support_needs_no_camera_prompt_or_setup(self):
        self.loaded.return_value = True
        package.ensure_uvc()
        self.confirm.assert_not_called()
        self.run.assert_not_called()
        self.download.assert_not_called()

    def test_existing_driver_is_loaded_without_downloading(self):
        self.modinfo.return_value = "/usr/bin/modinfo"
        self.loaded.side_effect = [False, True]
        with patch.object(package.subprocess, "run", return_value=Mock(returncode=0)):
            package.ensure_uvc()
        self.run.assert_called_once_with("sudo", "modprobe", "uvcvideo")
        self.download.assert_not_called()

    def test_declined_existing_driver_is_not_loaded(self):
        self.modinfo.return_value = "/usr/bin/modinfo"
        self.confirm.return_value = False
        with (
            patch.object(package.subprocess, "run", return_value=Mock(returncode=0)),
            self.assertRaisesRegex(RuntimeError, "not approved"),
        ):
            package.ensure_uvc()
        self.run.assert_not_called()
        self.download.assert_not_called()

    def test_setup_downloads_latest_and_replaces_only_managed_checkout(self):
        self.loaded.side_effect = [False, True]
        package.ensure_uvc()
        self.assertTrue((self.destination / "install.sh").stat().st_mode & 0o111)
        (self.destination / "old-file").touch()
        package.setup_uvc()
        self.assertFalse((self.destination / "old-file").exists())
        self.assertEqual(self.download.call_count, 2)
        self.assertTrue(self.download.call_args.args[0].endswith("/tar.gz/HEAD"))
        self.run.assert_called_with(
            "bash", self.destination / "install.sh", cwd=self.destination
        )
        self.assertFalse((self.data / ".sranipatch-uvc-lock").exists())

    def test_unfamiliar_directory_and_symlinks_are_preserved(self):
        self.destination.mkdir()
        sentinel = self.destination / "personal-file"
        sentinel.write_text("keep")
        with self.assertRaisesRegex(RuntimeError, "not installer-managed"):
            package.setup_uvc()
        self.assertEqual(sentinel.read_text(), "keep")
        self.download.assert_not_called()
        other = self.data / "elsewhere"
        self.destination.rename(other)
        self.destination.symlink_to(other, target_is_directory=True)
        with self.assertRaisesRegex(RuntimeError, "not installer-managed"):
            package.setup_uvc()
        self.assertEqual((other / "personal-file").read_text(), "keep")

    def test_failed_download_preserves_previous_checkout_and_unlocks(self):
        package.setup_uvc()
        sentinel = self.destination / "previous-source"
        sentinel.touch()
        self.download.side_effect = OSError("offline")
        with self.assertRaisesRegex(OSError, "offline"):
            package.setup_uvc()
        self.assertTrue(sentinel.exists())
        self.assertFalse((self.data / ".sranipatch-uvc-lock").exists())

    def test_unsupported_platform_gets_manual_guidance(self):
        with (
            patch.object(
                package.platform,
                "freedesktop_os_release",
                return_value={"ID": "debian"},
            ),
            self.assertRaisesRegex(RuntimeError, "distribution"),
        ):
            package.ensure_uvc()
        self.confirm.assert_not_called()
        self.download.assert_not_called()

    def test_setup_success_without_loaded_driver_still_fails(self):
        with self.assertRaisesRegex(RuntimeError, "still unavailable"):
            package.ensure_uvc()

    def test_upstream_failure_preserves_checkout_for_retry(self):
        self.run.side_effect = subprocess.CalledProcessError(1, "install.sh")
        with self.assertRaisesRegex(RuntimeError, "SteamOS update/reboot"):
            package.ensure_uvc()
        self.assertTrue((self.destination / "install.sh").is_file())
        self.assertFalse((self.data / ".sranipatch-uvc-lock").exists())

    def test_declined_setup_never_changes_registration_even_with_no_restart(self):
        self.confirm.return_value = False
        with (
            patch.object(sys, "argv", ["package.py", "install", "--no-restart"]),
            patch.object(package, "cleanup"),
            patch.object(package, "register") as register,
            self.assertRaisesRegex(RuntimeError, "not approved"),
        ):
            package.main()
        register.assert_not_called()
        self.run.assert_not_called()
        self.download.assert_not_called()

    def test_prompt_requires_explicit_yes_and_handles_missing_terminal(self):
        for answer, accepted in (
            ("yes\n", True),
            ("Y\n", True),
            ("\n", False),
            ("", False),
        ):
            with patch("builtins.open", mock_open(read_data=answer)):
                self.assertEqual(self.real_confirm("Set up UVC?"), accepted)
        with patch("builtins.open", side_effect=OSError("no controlling terminal")):
            self.assertFalse(self.real_confirm("Set up UVC?"))

    def test_prompt_reads_controlling_terminal_with_piped_stdin(self):
        import pty

        pid, terminal = pty.fork()
        if pid == 0:
            try:
                null = os.open(os.devnull, os.O_RDONLY)
                os.dup2(null, 0)
                os.close(null)
                accepted = self.real_confirm("Set up UVC?")
                os._exit(0 if accepted else 1)
            except BaseException:
                os._exit(2)
        try:
            output = b""
            deadline = time.monotonic() + 5
            while b"[y/N] " not in output:
                ready, _, _ = select.select(
                    [terminal], [], [], max(0, deadline - time.monotonic())
                )
                self.assertTrue(ready, "Confirmation prompt did not appear")
                try:
                    chunk = os.read(terminal, 4096)
                except OSError:
                    chunk = b""
                self.assertTrue(chunk, "Confirmation exited before prompting")
                output += chunk
            os.write(terminal, b"yes\n")
            while time.monotonic() < deadline:
                finished, status = os.waitpid(pid, os.WNOHANG)
                if finished:
                    pid = None
                    self.assertEqual(os.waitstatus_to_exitcode(status), 0)
                    break
                time.sleep(0.01)
            else:
                self.fail("Confirmation did not consume the terminal answer")
        finally:
            if pid is not None:
                os.kill(pid, signal.SIGKILL)
                os.waitpid(pid, 0)
            os.close(terminal)

    def test_successful_install_verifies_support_before_registration(self):
        events = []
        self.loaded.side_effect = lambda: events.append("check") or True
        with (
            patch.object(sys, "argv", ["package.py", "install", "--no-restart"]),
            patch.object(package, "cleanup"),
            patch.object(
                package, "register", side_effect=lambda root: events.append("register")
            ),
            patch.object(package, "offer_restart"),
        ):
            package.main()
        self.assertEqual(events, ["check", "register"])

    def test_noninteractive_install_does_not_download_or_register(self):
        self.confirm.side_effect = self.real_confirm
        with (
            patch("builtins.open", side_effect=OSError("no controlling terminal")),
            patch.object(sys, "argv", ["package.py", "install"]),
            patch.object(package, "cleanup"),
            patch.object(package, "register") as register,
            self.assertRaisesRegex(RuntimeError, "not approved"),
        ):
            package.main()
        register.assert_not_called()
        self.download.assert_not_called()


class LinuxUpdateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "installed"
        self.staged = Path(self.temporary.name) / "staged"
        make_package(self.root)
        make_package(self.staged, "v2")

    def test_remote_update_selects_runtime_asset_and_checks_checksum(self):
        filename = "SRanipatch-Runtime.linux-arm64.tar.gz"
        assets = Path(self.temporary.name) / "assets"
        assets.mkdir()
        with tarfile.open(assets / filename, "w:gz") as archive:
            for child in self.staged.iterdir():
                archive.add(child, arcname=child.name)
        (assets / "release.json").write_text(
            json.dumps(
                {
                    "tag_name": "v2",
                    "assets": [
                        {
                            "name": name,
                            "browser_download_url": "https://fixture/" + name,
                        }
                        for name in (
                            filename,
                            filename + ".sha256",
                            "SRanipatch.VRCFT.win-x64.zip",
                        )
                    ],
                }
            )
        )

        def download(url, destination):
            name = "release.json" if url == package.API else url.rsplit("/", 1)[-1]
            shutil.copy2(assets / name, destination)

        state = {"active": False, "managed": False, "revision": 0, "run": False}
        with (
            patch.object(package, "download", side_effect=download),
            patch.object(package, "registered", return_value=False),
            patch.object(package, "control", return_value=state),
            patch.object(package, "processes", return_value=[]),
        ):
            # A corrupt download must leave the installed version untouched.
            (assets / (filename + ".sha256")).write_text("0" * 64 + "  " + filename)
            with self.assertRaisesRegex(RuntimeError, "checksum"):
                package.update(self.root)
            self.assertEqual(package.metadata(self.root)["version"], "v1")
            (assets / (filename + ".sha256")).write_text(
                hashlib.sha256((assets / filename).read_bytes()).hexdigest()
                + "  "
                + filename
            )
            package.update(self.root)
        self.assertEqual(package.metadata(self.root)["version"], "v2")
        self.assertFalse((self.root / ".update-lock").exists())

    def test_launchers_forward_arguments_to_only_the_requested_application(self):
        for application in ("server", "client"):
            executable = self.root / f"app/bin/sranipatch-{application}"
            with (
                patch.object(package, "ROOT", self.root),
                patch.object(
                    sys, "argv", ["package.py", f"start-{application}", "--help"]
                ),
                patch.object(package.os, "execv") as execute,
            ):
                # A real exec never returns.
                execute.side_effect = SystemExit(0)
                with self.assertRaises(SystemExit):
                    package.main()
            execute.assert_called_once_with(executable, [str(executable), "--help"])

    def test_portable_update_does_not_register_or_start_idle_apps(self):
        (self.root / "start").touch()
        state = {"active": False, "managed": False, "revision": 0, "run": False}
        with (
            patch.object(package, "registered", return_value=False),
            patch.object(package, "control", return_value=state),
            patch.object(package, "processes", return_value=[]),
            patch.object(package, "register") as register,
        ):
            package.apply_update(self.root, self.staged)
        register.assert_not_called()
        self.assertEqual(package.metadata(self.root)["version"], "v2")
        self.assertEqual(len(list(self.root.glob("app.*.old"))), 1)
        self.assertTrue((self.root / "start-server").is_file())
        self.assertTrue((self.root / "start-client").is_file())
        self.assertFalse((self.root / "start").exists())

    def test_registered_update_pauses_and_restores_preference(self):
        state = {"active": True, "managed": True, "revision": 1, "run": True}
        events = []

        def control(root, command, *values):
            events.append((command, package.metadata(root)["version"]))
            if command == "status":
                return state.copy()
            if command == "pause":
                state["run"] = False
            elif command == "resume":
                state["run"] = values[0] == "1"

        def register(root):
            (root / "registration-marker").write_text(package.metadata(root)["version"])

        with (
            patch.object(package, "registered", return_value=True),
            patch.object(package, "control", side_effect=control),
            patch.object(package, "processes", return_value=[]),
            patch.object(package, "register", side_effect=register),
        ):
            package.apply_update(self.root, self.staged)
        self.assertEqual(package.metadata(self.root)["version"], "v2")
        self.assertEqual((self.root / "registration-marker").read_text(), "v2")
        self.assertTrue(state["run"])
        self.assertIn(("pause", "v1"), events)
        self.assertIn(("resume", "v2"), events)

    def test_failed_promotion_restores_old_directory_and_runtime(self):
        original = Path.rename

        def rename(source, destination):
            if source == self.staged / "app":
                raise OSError("injected promotion failure")
            return original(source, destination)

        state = {"active": True, "managed": True, "revision": 1, "run": False}
        with (
            patch.object(package, "registered", return_value=False),
            patch.object(package, "control", return_value=state) as control,
            patch.object(package, "processes", return_value=[]),
            patch.object(Path, "rename", rename),
            self.assertRaisesRegex(OSError, "promotion failure"),
        ):
            package.apply_update(self.root, self.staged)
        self.assertEqual(package.metadata(self.root)["version"], "v1")
        self.assertEqual(control.call_args.args[1:], ("resume", "0"))
        self.assertEqual(list(self.root.glob("app.*.old")), [])

    def test_archive_rejects_traversal(self):
        archive = Path(self.temporary.name) / "bad.tar.gz"
        with tarfile.open(archive, "w:gz") as output:
            output.addfile(tarfile.TarInfo("../outside"))
        with self.assertRaisesRegex(RuntimeError, "Unsafe"):
            package.extract(archive, self.staged)

    def test_driver_restart_uses_loaded_revision_and_inequality(self):
        archive = Path(self.temporary.name) / "release.tar.gz"
        with tarfile.open(archive, "w:gz") as output:
            for child in self.staged.iterdir():
                output.add(child, arcname=child.name)
        state = {"active": True, "managed": True, "revision": 2, "run": True}
        with (
            patch.object(package, "registered", return_value=False),
            patch.object(package, "control", return_value=state),
            patch.object(package, "processes", return_value=[]),
            patch.object(package, "offer_restart") as restart,
        ):
            package.update(self.root, archive)
        restart.assert_called_once()

    @unittest.skipIf(os.name == "nt", "Linux bootstrap integration")
    def test_bootstrap_installs_then_updates_existing_destination(self):
        directory = Path(self.temporary.name)
        assets, tools = directory / "assets", directory / "tools"
        assets.mkdir()
        tools.mkdir()
        destination = directory / "bootstrap destination"
        config = directory / "config/openvr"
        config.mkdir(parents=True)
        (config / "openvrpaths.vrpath").write_text('{"runtime":["/unused"]}')
        registration = self.staged / "app/scripts/register-steamvr.sh"
        registration.write_text(
            '#!/bin/sh\nprintf "installed\\n" >> "$SRANIPATCH_TEST_LOG"\n'
        )
        helper = self.staged / "app/bin/sranipatch-package-control"
        helper.write_text(
            '#!/bin/sh\necho \'{"active":false,"managed":false,"run":false,"revision":0}\'\n'
        )
        helper.chmod(0o755)
        # Simulate a host with UVC already loaded for the bootstrap subprocess.
        script = self.staged / "app/scripts/package.py"
        script.write_text(
            script.read_text().replace(
                'return Path("/sys/module/uvcvideo").is_dir()', "return True"
            )
        )
        curl = tools / "curl"
        curl.write_text(
            "#!/usr/bin/env python3\nimport os,shutil,sys\nfrom pathlib import Path\n"
            'url=sys.argv[2]\nname="release.json" if url.endswith("/latest") else url.rsplit("/",1)[-1]\n'
            'shutil.copyfile(Path(os.environ["SRANIPATCH_TEST_ASSETS"])/name,sys.argv[4])\n'
        )
        curl.chmod(0o755)
        uname = tools / "uname"
        uname.write_text(
            '#!/bin/sh\nif [ "$1" = -s ]; then echo Linux; else echo aarch64; fi\n'
        )
        uname.chmod(0o755)
        environment = os.environ.copy()
        environment.update(
            {
                "PATH": str(tools) + os.pathsep + environment["PATH"],
                "XDG_CONFIG_HOME": str(config.parent),
                "SRANIPATCH_TEST_ASSETS": str(assets),
                "SRANIPATCH_TEST_LOG": str(directory / "registration.log"),
            }
        )
        filename = "SRanipatch-Runtime.linux-arm64.tar.gz"
        for version in ("v2", "v3"):
            info = package.metadata(self.staged)
            info["version"] = version
            (self.staged / "app/package.json").write_text(json.dumps(info))
            with tarfile.open(assets / filename, "w:gz") as archive:
                for child in self.staged.iterdir():
                    archive.add(child, arcname=child.name)
            (assets / (filename + ".sha256")).write_text(
                hashlib.sha256((assets / filename).read_bytes()).hexdigest()
                + "  "
                + filename
            )
            (assets / "release.json").write_text(
                json.dumps(
                    {
                        "tag_name": version,
                        "assets": [
                            {
                                "name": name,
                                "browser_download_url": "https://fixture/" + name,
                            }
                            for name in (filename, filename + ".sha256")
                        ],
                    }
                )
            )
            result = subprocess.run(
                [
                    "bash",
                    str(REPO / "scripts/get.sh"),
                    "--dir",
                    str(destination),
                    "--no-restart",
                ],
                env=environment,
                capture_output=True,
                text=True,
                check=False,
                timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(package.metadata(destination)["version"], version)
        self.assertEqual(
            (directory / "registration.log").read_text().splitlines(),
            ["installed", "installed"],
        )


@unittest.skipUnless(os.name != "nt" and FIXTURE, "Linux process integration")
class LinuxProcessTests(unittest.TestCase):
    def test_manual_server_is_relaunched_from_new_package(self):
        with tempfile.TemporaryDirectory(prefix="sranipatch package ") as temporary:
            directory = Path(temporary)
            root, staged = directory / "installed", directory / "staged"
            make_package(root)
            make_package(staged, "v2")
            record = directory / "manual-server.txt"
            server = subprocess.Popen(
                [str(root / "app/bin/sranipatch-server"), "--record", str(record)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            try:
                deadline = time.monotonic() + 5
                while not record.exists() and time.monotonic() < deadline:
                    time.sleep(0.05)
                self.assertTrue(record.exists())
                with patch.object(package, "registered", return_value=False):
                    package.apply_update(root, staged)
                server.wait(timeout=5)
                deadline = time.monotonic() + 5
                while (
                    len(record.read_text().splitlines()) < 2
                    and time.monotonic() < deadline
                ):
                    time.sleep(0.05)
                self.assertEqual(len(record.read_text().splitlines()), 2)
                self.assertEqual(package.metadata(root)["version"], "v2")
            finally:
                for process in package.processes(root):
                    os.kill(process[0], signal.SIGTERM)
                server.wait(timeout=5)


@unittest.skipUnless(os.name == "nt", "Windows package commands")
class WindowsCommandTests(unittest.TestCase):
    def invoke_function(self, body):
        source = REPO / "scripts/package.ps1"
        harness = r"""
$ErrorActionPreference = 'Stop'
$ast = [Management.Automation.Language.Parser]::ParseFile($env:SRANIPATCH_SCRIPT, [ref]$null, [ref]$null)
foreach ($statement in $ast.EndBlock.Statements) {
    if ($statement -is [Management.Automation.Language.FunctionDefinitionAst]) {
        . ([scriptblock]::Create($statement.Extent.Text))
    }
}
""" + body
        return subprocess.run(
            ["powershell.exe", "-NoProfile", "-Command", harness],
            check=False,
            capture_output=True,
            text=True,
            env={**os.environ, "SRANIPATCH_SCRIPT": str(source)},
            timeout=10,
        )

    def test_expand_release_accepts_normalized_destination(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            archive = directory / "release.zip"
            with zipfile.ZipFile(archive, "w") as output:
                output.writestr("app/package.json", "{}")
            result = self.invoke_function(
                "$parent = '" + str(directory).replace("'", "''") + "'\n"
                "$destination = Join-Path $parent 'staging/../package'\n"
                "Expand-Release '" + str(archive).replace("'", "''") + "' $destination\n"
                "if (-not (Test-Path (Join-Path $parent 'package/app/package.json'))) { throw 'Missing extracted file' }\n"
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_expand_release_rejects_traversal(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            archive = directory / "release.zip"
            with zipfile.ZipFile(archive, "w") as output:
                output.writestr("../outside.txt", "unsafe")
            result = self.invoke_function(
                "Expand-Release '"
                + str(archive).replace("'", "''")
                + "' '"
                + str(directory / "package").replace("'", "''")
                + "'\n"
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Unsafe entry in release archive", result.stderr)
            self.assertFalse((directory / "outside.txt").exists())

    def test_process_stop_uses_cim_path_when_get_process_path_is_missing(self):
        result = self.invoke_function(r"""
$script:stopped = $false
function Get-Process { $process = [pscustomobject]@{ Path = $null }; $process | Add-Member ScriptMethod WaitForExit { return $true }; return $process }
function Get-CimInstance { [pscustomobject]@{ ExecutablePath = 'C:\package\server.exe' } }
function Stop-Process { $script:stopped = $true }
Invoke-PackageProcessStop @([pscustomobject]@{ ProcessId = 42; ExecutablePath = 'C:\package\server.exe'; Name = 'sranipatch-server.exe' })
if (-not $script:stopped) { throw 'Matching process was not stopped' }
""")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_package_process_discovery_uses_pid_when_name_filter_misses(self):
        result = self.invoke_function(r"""
$packageRoot = 'C:\package'
function Get-Process {
    [pscustomobject]@{ Id = 42; Path = 'C:\package\app\bin\sranipatch-server.exe' }
}
function Get-CimInstance {
    param($ClassName, $Filter)
    if ($Filter -like 'Name*') { return }
    [pscustomobject]@{
        ProcessId = 42
        ExecutablePath = 'C:\package\app\bin\sranipatch-server.exe'
        CommandLine = '"C:\package\app\bin\sranipatch-server.exe" --record log.txt'
        Name = 'sranipatch-server.exe'
    }
}
$snapshot = @(Get-PackageProcess)
if ($snapshot.Count -ne 1 -or $snapshot[0].ProcessId -ne 42) { throw 'Package process was not discovered' }
""")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rename_accepts_root_with_trailing_separator(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "app").mkdir()
            result = self.invoke_function(
                "$packageRoot = '"
                + str(root).replace("'", "''")
                + "' + [IO.Path]::DirectorySeparatorChar\n"
                "Rename-PackageDirectory (Join-Path $packageRoot 'app') (Join-Path $packageRoot 'app.old')\n"
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue((root / "app.old").is_dir())

    def invoke(self, command, mocks):
        source = REPO / "scripts/package.ps1"
        harness = r"""
$ErrorActionPreference = 'Stop'
$NoRestart = $false
$packageRoot = Split-Path $env:SRANIPATCH_SCRIPT
$ast = [Management.Automation.Language.Parser]::ParseFile($env:SRANIPATCH_SCRIPT, [ref]$null, [ref]$null)
$statements = $ast.EndBlock.Statements
foreach ($statement in $statements) {
    if ($statement -is [Management.Automation.Language.FunctionDefinitionAst]) {
        . ([scriptblock]::Create($statement.Extent.Text))
    }
}
function Invoke-OldPackageCleanup {}
function Register-Driver { param([switch]$Remove) }
function Read-Host { param($Prompt); Write-Output "PROMPT: $Prompt" | Out-Host; return 'yes' }
"""
        harness += mocks + "\n$Command = '" + command + "'\n"
        harness += r"""
foreach ($statement in ($statements | Select-Object -Last 2)) {
    . ([scriptblock]::Create($statement.Extent.Text))
}
"""
        environment = os.environ.copy()
        environment["SRANIPATCH_SCRIPT"] = str(source)
        return subprocess.run(
            ["powershell.exe", "-NoProfile", "-Command", harness],
            check=False,
            capture_output=True,
            text=True,
            env=environment,
            timeout=10,
        )

    def test_uninstall_waits_after_success_and_failure(self):
        for mocks, code in (
            ("", 0),
            ("function Register-Driver { throw 'injected failure' }", 1),
        ):
            result = self.invoke("uninstall", mocks)
            self.assertEqual(result.returncode, code, result.stdout + result.stderr)
            self.assertIn("Press Enter to exit", result.stdout)

    def test_remote_update_selects_runtime_asset_and_checks_checksum(self):
        with tempfile.TemporaryDirectory(prefix="sranipatch download ") as temporary:
            directory = Path(temporary)
            root, staged, assets = (
                directory / "installed",
                directory / "staged",
                directory / "assets",
            )
            make_package(root, platform="windows-x64")
            make_package(staged, "v2", platform="windows-x64")
            assets.mkdir()
            filename = "SRanipatch-Runtime.win-x64.zip"
            with zipfile.ZipFile(assets / filename, "w") as archive:
                for file in staged.rglob("*"):
                    if file.is_file():
                        archive.write(file, file.relative_to(staged))
            (assets / "release.json").write_text(
                json.dumps(
                    {
                        "tag_name": "v2",
                        "assets": [
                            {
                                "name": name,
                                "browser_download_url": "https://fixture/" + name,
                            }
                            for name in (
                                filename,
                                filename + ".sha256",
                                "SRanipatch.VRCFT.win-x64.zip",
                            )
                        ],
                    }
                )
            )
            mocks = (
                "$packageRoot = '" + str(root).replace("'", "''") + "'\n"
                "$script:assets = '"
                + str(assets).replace("'", "''")
                + "'\n"
                + r"""
function Invoke-RestMethod { Get-Content -Raw (Join-Path $script:assets 'release.json') | ConvertFrom-Json }
function Invoke-WebRequest {
    param([switch]$UseBasicParsing, [string]$Uri, [string]$OutFile)
    Copy-Item -LiteralPath (Join-Path $script:assets ([Uri]$Uri).Segments[-1]) -Destination $OutFile
}
function Test-Registered { return $false }
function Invoke-Control { return @{ active = $false; managed = $false; revision = 0; run = $false } }
function Get-PackageProcess {}
"""
            )
            (assets / (filename + ".sha256")).write_text("0" * 64 + "  " + filename)
            result = self.invoke("update", mocks)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("checksum", result.stderr)
            self.assertEqual(package.metadata(root)["version"], "v1")
            (assets / (filename + ".sha256")).write_text(
                hashlib.sha256((assets / filename).read_bytes()).hexdigest()
                + "  "
                + filename
            )
            result = self.invoke("update", mocks)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(package.metadata(root)["version"], "v2")
            self.assertFalse((root / ".update-lock").exists())

    def test_install_when_steamvr_stopped_waits_without_restart_offer(self):
        result = self.invoke("install", "function Get-Process {}")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Installed successfully", result.stdout)
        self.assertIn("Press Enter to exit", result.stdout)
        self.assertNotIn("Restart SteamVR now", result.stdout)

    def test_restart_does_not_wait_for_process_tree_or_startup(self):
        result = self.invoke(
            "install",
            r"""
$script:running = $true
function Get-Process { if ($script:running) { return [pscustomobject]@{ Id = 1 } } }
function Get-OpenVRPathRegistry { return @{ runtime = @('C:/SteamVR') } }
function Start-Process {
    param($FilePath, $ArgumentList, $WindowStyle, [switch]$Wait)
    if ($Wait) { throw 'Must not wait for process tree' }
    if ($ArgumentList -eq '-shutdown') { $script:running = $false }
    else { Write-Output 'RELAUNCH REQUESTED' }
}
""",
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("RELAUNCH REQUESTED", result.stdout)
        self.assertNotIn("Press Enter to exit", result.stdout)


@unittest.skipUnless(
    os.name == "nt" and FIXTURE and DRIVER, "Windows package integration"
)
class WindowsUpdateTests(unittest.TestCase):
    def test_repeated_swap_with_loaded_driver_and_manual_server(self):
        with tempfile.TemporaryDirectory(prefix="sranipatch package ") as temporary:
            directory = Path(temporary)
            root, staged = directory / "installed", directory / "staged"
            make_package(root, platform="windows-x64")
            make_package(staged, "v2", platform="windows-x64")
            loaded_path = root / "app/driver/loaded.dll"
            shutil.copy2(DRIVER, loaded_path)
            loaded = ctypes.WinDLL(str(loaded_path))
            record = directory / "manual-server.txt"
            server = subprocess.Popen(
                [str(root / "app/bin/sranipatch-server.exe"), "--record", str(record)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            try:
                deadline = time.monotonic() + 5
                while not record.exists():
                    if time.monotonic() > deadline:
                        self.fail("fixture server did not start")
                    time.sleep(0.05)
                probe_environment = {
                    **os.environ,
                    "SRANIPATCH_SCRIPT": str(root / "app/scripts/package.ps1"),
                    "SRANIPATCH_TEST_ROOT": str(root),
                    "SRANIPATCH_TEST_PID": str(server.pid),
                }
                probe_script = r"""
$ErrorActionPreference = 'Stop'
$ast = [Management.Automation.Language.Parser]::ParseFile($env:SRANIPATCH_SCRIPT, [ref]$null, [ref]$null)
foreach ($statement in $ast.EndBlock.Statements) {
    if ($statement -is [Management.Automation.Language.FunctionDefinitionAst]) {
        . ([scriptblock]::Create($statement.Extent.Text))
    }
}
$packageRoot = $env:SRANIPATCH_TEST_ROOT
Get-PackageProcess | Select-Object -ExpandProperty ProcessId
"""

                def process_snapshot():
                    return subprocess.run(
                        ["powershell.exe", "-NoProfile", "-Command", probe_script],
                        check=False,
                        env=probe_environment,
                        capture_output=True,
                        text=True,
                        timeout=10,
                    )

                snapshot = process_snapshot()
                self.assertEqual(snapshot.returncode, 0, snapshot.stderr)
                if str(server.pid) not in snapshot.stdout.splitlines():
                    diagnostic = subprocess.run(
                        [
                            "powershell.exe",
                            "-NoProfile",
                            "-Command",
                            r"$p = Get-Process -Id $env:SRANIPATCH_TEST_PID -ErrorAction SilentlyContinue; "
                            r"$c = Get-CimInstance Win32_Process -Filter ('ProcessId = ' + $env:SRANIPATCH_TEST_PID); "
                            r"[pscustomobject]@{ ProcessName = $p.ProcessName; ProcessPath = $p.Path; "
                            r"CimName = $c.Name; CimPath = $c.ExecutablePath; "
                            r"CommandLine = $c.CommandLine } | ConvertTo-Json -Compress",
                        ],
                        check=False,
                        env=probe_environment,
                        capture_output=True,
                        text=True,
                        timeout=10,
                    )
                    self.fail(
                        f"Fixture process {server.pid} was not discovered; "
                        f"snapshot={snapshot.stdout!r}; diagnostic={diagnostic.stdout!r}; "
                        f"errors={diagnostic.stderr!r}"
                    )
                archive = directory / "release.zip"
                with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as output:
                    for file in staged.rglob("*"):
                        if file.is_file():
                            output.write(file, file.relative_to(staged))
                environment = os.environ.copy()
                environment["LOCALAPPDATA"] = str(directory / "empty-settings")
                for index, version in enumerate(("v2", "v3", "v3")):
                    info = package.metadata(staged)
                    info["version"] = version
                    (staged / "app/package.json").write_text(json.dumps(info))
                    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as output:
                        for file in staged.rglob("*"):
                            if file.is_file():
                                output.write(file, file.relative_to(staged))
                    result = subprocess.run(
                        [
                            "powershell.exe",
                            "-NoProfile",
                            "-ExecutionPolicy",
                            "Bypass",
                            "-File",
                            str(root / "app/scripts/package.ps1"),
                            "-Command",
                            "update",
                            "-Archive",
                            str(archive),
                            "-NoRestart",
                        ],
                        check=False,
                        env=environment,
                        text=True,
                        capture_output=True,
                        timeout=45,
                    )
                    self.assertEqual(
                        result.returncode, 0, result.stdout + result.stderr
                    )
                    if index < 2:
                        deadline = time.monotonic() + 5
                        while (
                            len(record.read_text().splitlines()) < index + 2
                            and time.monotonic() < deadline
                        ):
                            time.sleep(0.05)
                        self.assertGreaterEqual(
                            len(record.read_text().splitlines()),
                            index + 2,
                            f"Server did not relaunch after {version}; "
                            f"initial server alive={server.poll() is None}; "
                            f"process snapshot={process_snapshot().stdout!r}",
                        )
                self.assertEqual(package.metadata(root)["version"], "v3")
                self.assertTrue(loaded.HmdDriverFactory)
                deadline = time.monotonic() + 5
                while (
                    len(record.read_text().splitlines()) < 3
                    and time.monotonic() < deadline
                ):
                    time.sleep(0.05)
                self.assertGreaterEqual(len(record.read_text().splitlines()), 3)
            finally:
                if server.poll() is None:
                    server.terminate()
                server.wait(timeout=5)
                cleanup_environment = os.environ.copy()
                cleanup_environment["SRANIPATCH_TEST_EXE"] = str(
                    root / "app/bin/sranipatch-server.exe"
                )
                # Only terminate this test package's relaunched fixture processes.
                subprocess.run(
                    [
                        "powershell.exe",
                        "-NoProfile",
                        "-Command",
                        (
                            "Get-CimInstance Win32_Process | Where-Object "
                            "{ $_.ExecutablePath -eq $env:SRANIPATCH_TEST_EXE } | ForEach-Object "
                            "{ Stop-Process -Id $_.ProcessId -Force }"
                        ),
                    ],
                    check=False,
                    env=cleanup_environment,
                    capture_output=True,
                    timeout=15,
                )
                server.wait(timeout=5)
                ctypes.windll.kernel32.FreeLibrary(ctypes.c_void_p(loaded._handle))


if __name__ == "__main__":
    unittest.main()
