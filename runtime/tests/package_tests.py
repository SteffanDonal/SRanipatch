import ctypes
import hashlib
import importlib.util
import json
import os
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
from unittest.mock import patch

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


class LinuxUpdateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "installed"
        self.staged = Path(self.temporary.name) / "staged"
        make_package(self.root)
        make_package(self.staged, "v2")

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

    def test_restart_offer_is_silent_when_steamvr_is_stopped(self):
        with (
            patch.object(package, "steamvr_running", return_value=False),
            patch("builtins.print") as output,
            patch("builtins.open") as terminal,
            patch.object(package.subprocess, "run") as restart,
        ):
            package.offer_restart(False)
        output.assert_not_called()
        terminal.assert_not_called()
        restart.assert_not_called()

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
        with (
            patch.object(package, "registered", return_value=True),
            patch.object(package, "control", return_value=state) as control,
            patch.object(package, "processes", return_value=[]),
            patch.object(package, "register") as register,
        ):
            package.apply_update(self.root, self.staged)
        self.assertEqual(
            [call.args[1:] for call in control.call_args_list],
            [("status",), ("pause",), ("resume", "1")],
        )
        register.assert_called_once_with(self.root)

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
        filename = "sranipatch-linux-arm64.tar.gz"
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
                archive = directory / "release.zip"
                with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as output:
                    for file in staged.rglob("*"):
                        if file.is_file():
                            output.write(file, file.relative_to(staged))
                environment = os.environ.copy()
                environment["LOCALAPPDATA"] = str(directory / "empty-settings")
                for version in ("v2", "v3", "v3"):
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
