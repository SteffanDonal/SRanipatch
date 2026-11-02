param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('start-server', 'start-client', 'install', 'uninstall', 'update')]
    [string]$Command,
    [switch]$NoRestart,
    [string]$Archive,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ApplicationArguments
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$packageRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))

function Invoke-Control([string]$Action, [string[]]$Values = @()) {
    $output = & (Join-Path $packageRoot 'app/bin/sranipatch-package-control.exe') $Action @Values
    if ($LASTEXITCODE -ne 0) { throw "Runtime control '$Action' failed ($LASTEXITCODE)" }
    if ($Action -eq 'status') { ($output | Where-Object { $_ -match '^\{' } | Select-Object -Last 1) | ConvertFrom-Json }
}

function Get-OpenVRPathRegistry {
    $path = Join-Path $env:LOCALAPPDATA 'openvr/openvrpaths.vrpath'
    if (Test-Path -LiteralPath $path) { Get-Content -Raw -LiteralPath $path | ConvertFrom-Json }
}

function Test-Registered {
    $paths = Get-OpenVRPathRegistry
    $driver = [IO.Path]::GetFullPath((Join-Path $packageRoot 'app/driver')).TrimEnd('\')
    foreach ($entry in $paths.external_drivers) {
        if ([IO.Path]::GetFullPath($entry).TrimEnd('\') -eq $driver) { return $true }
    }
    return $false
}

function Register-Driver([switch]$Remove) {
    if (-not (Get-OpenVRPathRegistry)) {
        if ($Remove) { return }
        throw 'SteamVR was not found. Install and run SteamVR first.'
    }
    & (Join-Path $packageRoot 'app/scripts/register-steamvr.ps1') -DriverPath (Join-Path $packageRoot 'app/driver') -Unregister:$Remove | Out-Host
}

function Invoke-OldPackageCleanup {
    if (Test-Path -LiteralPath (Join-Path $packageRoot '.update-lock')) { return }
    Get-ChildItem -LiteralPath $packageRoot -Directory -Filter 'app.*.old' | ForEach-Object {
        if ($_.Parent.FullName -eq $packageRoot -and -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            # Loaded driver files can stay locked until SteamVR exits.
            Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

function Rename-PackageDirectory([string]$Source, [string]$Destination) {
    $root = [IO.Path]::GetFullPath($packageRoot).TrimEnd([IO.Path]::DirectorySeparatorChar)
    foreach ($path in @($Source, $Destination)) {
        if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($path)) -ne $root) {
            throw 'Package rename must stay within its root directory'
        }
    }
    if (-not ('SRanipatch.PackageRename' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
namespace SRanipatch {
    public static class PackageRename {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern SafeFileHandle CreateFile(string name, uint access, uint share,
            IntPtr security, uint creation, uint flags, IntPtr template);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool SetFileInformationByHandle(SafeFileHandle file, int kind, IntPtr info, uint size);
        public static void Move(string source, string destination) {
            using (var file = CreateFile(source, 0x10000, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero)) {
                if (file.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
                byte[] name = System.Text.Encoding.Unicode.GetBytes(destination + "\0");
                // FILE_RENAME_INFO: flags, aligned root handle, byte length, UTF-16 name.
                int lengthOffset = IntPtr.Size == 8 ? 16 : 8;
                int nameOffset = lengthOffset + 4;
                IntPtr info = Marshal.AllocHGlobal(nameOffset + name.Length);
                try {
                    for (int i = 0; i < nameOffset; ++i) Marshal.WriteByte(info, i, 0);
                    Marshal.WriteInt32(info, 0, 2); // FILE_RENAME_FLAG_POSIX_SEMANTICS
                    Marshal.WriteInt32(info, lengthOffset, name.Length - 2);
                    Marshal.Copy(name, 0, IntPtr.Add(info, nameOffset), name.Length);
                    if (!SetFileInformationByHandle(file, 22, info, (uint)(nameOffset + name.Length)))
                        throw new Win32Exception(Marshal.GetLastWin32Error());
                } finally { Marshal.FreeHGlobal(info); }
            }
        }
    }
}
'@
    }
    try { [SRanipatch.PackageRename]::Move($Source, $Destination) }
    catch { throw "Rename '$Source' to '$Destination' failed: $_" }
}

function Get-PackageProcess {
    $bin = [IO.Path]::GetFullPath((Join-Path $packageRoot 'app/bin'))
    Get-Process -Name 'sranipatch-server', 'sranipatch-client' -ErrorAction SilentlyContinue | ForEach-Object {
        $entry = Get-CimInstance Win32_Process -Filter "ProcessId = $($_.Id)" -ErrorAction Stop
        $path = if ($entry.ExecutablePath) { $entry.ExecutablePath } else { $_.Path }
        if ($path -and [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($path)) -eq $bin) {
            if (-not $entry) { throw "Could not inspect package process $($_.Id)" }
            [pscustomobject]@{
                ProcessId = $_.Id
                ExecutablePath = $path
                CommandLine = $entry.CommandLine
                Name = [IO.Path]::GetFileName($path)
            }
        }
    }
}

function Invoke-PackageProcessStop($Snapshot) {
    foreach ($entry in $Snapshot) {
        $process = Get-Process -Id $entry.ProcessId -ErrorAction SilentlyContinue
        if (-not $process) { continue }
        $path = $process.Path
        if (-not $path) {
            $path = (Get-CimInstance Win32_Process -Filter "ProcessId = $($entry.ProcessId)" -ErrorAction SilentlyContinue).ExecutablePath
        }
        if ($path -eq $entry.ExecutablePath) {
            $process | Stop-Process -Force
            if (-not $process.WaitForExit(10000)) { throw "Timed out stopping $($entry.Name)" }
        }
    }
}

function Restore-PackageProcess($Snapshot, $State) {
    foreach ($entry in $Snapshot) {
        if ($State.managed -and $entry.Name -eq 'sranipatch-server.exe') { continue }
        if ($entry.CommandLine -match '(?:^|\s)"?--lifetime"?(?:\s|$)') { continue }
        if (Get-Process -Id $entry.ProcessId -ErrorAction SilentlyContinue) { continue }
        $arguments = [regex]::Match($entry.CommandLine, '^\s*(?:"[^"]*"|\S+)\s*(.*)$').Groups[1].Value
        $parameters = @{
            FilePath = $entry.ExecutablePath
            WorkingDirectory = (Join-Path $packageRoot 'app/bin')
            WindowStyle = 'Hidden'
        }
        if ($arguments) { $parameters.ArgumentList = $arguments }
        Start-Process @parameters
    }
}

function Show-RestartOffer([bool]$SkipRestart) {
    if ($SkipRestart -or -not (Get-Process vrserver -ErrorAction SilentlyContinue)) { return }
    Write-Information 'On Steam Frame, restarting SteamVR will make the display go black temporarily.' -InformationAction Continue
    $answer = Read-Host 'Restart SteamVR now? [y/N]'
    if ($answer -notmatch '^(y|yes)$') { return }
    $paths = Get-OpenVRPathRegistry
    $monitor = Join-Path $paths.runtime[0] 'bin/win64/vrmonitor.exe'
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    Start-Process -FilePath $monitor -ArgumentList '-shutdown' -WindowStyle Hidden
    while (Get-Process vrserver -ErrorAction SilentlyContinue) {
        if ([DateTime]::UtcNow -gt $deadline) { throw 'SteamVR did not exit. Please restart it manually.' }
        Start-Sleep -Milliseconds 200
    }
    Start-Process -FilePath $monitor -WindowStyle Hidden
}

function Expand-Release([string]$Path, [string]$Destination) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $destinationRoot = [IO.Path]::GetFullPath($Destination).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $zip = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        foreach ($entry in $zip.Entries) {
            $target = [IO.Path]::GetFullPath((Join-Path $destinationRoot $entry.FullName))
            if (-not $target.StartsWith($destinationRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
                throw 'Unsafe entry in release archive'
            }
        }
    } finally { $zip.Dispose() }
    [IO.Compression.ZipFile]::ExtractToDirectory($Path, $Destination)
}

function Get-PackageInfo([string]$Root) {
    Get-Content -Raw -LiteralPath (Join-Path $Root 'app/package.json') | ConvertFrom-Json
}

function Assert-Release([string]$Root, [string]$Platform) {
    $info = Get-PackageInfo $Root
    if ($info.platform -ne $Platform -or $info.driver_revision -notmatch '^\d+$') { throw 'Release does not match this platform' }
    foreach ($name in @('app/bin/sranipatch-server.exe', 'app/bin/sranipatch-client.exe', 'app/bin/sranipatch-package-control.exe',
                       'app/driver/driver.vrdrivermanifest', 'start-server.cmd', 'start-client.cmd', 'install.cmd', 'uninstall.cmd', 'update.cmd', 'github.url')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $name) -PathType Leaf)) { throw "Release is missing $name" }
    }
    return $info
}

function Install-Update([string]$Staged) {
    $wasRegistered = Test-Registered
    $state = Invoke-Control status
    $snapshot = @(Get-PackageProcess)
    $app = Join-Path $packageRoot 'app'
    $backup = Join-Path $packageRoot ('app.' + [guid]::NewGuid().ToString('N') + '.old')
    try {
        if ($state.active) { Invoke-Control pause }
        Invoke-PackageProcessStop $snapshot
        Rename-PackageDirectory $app $backup
        try { [IO.Directory]::Move((Join-Path $Staged 'app'), $app) }
        catch {
            Rename-PackageDirectory $backup $app
            throw
        }
        foreach ($name in @('start-server.cmd', 'start-client.cmd', 'install.cmd', 'uninstall.cmd', 'update.cmd', 'github.url')) {
            Copy-Item -LiteralPath (Join-Path $Staged $name) -Destination (Join-Path $packageRoot $name) -Force
        }
        Remove-Item -LiteralPath (Join-Path $packageRoot 'start.cmd') -Force -ErrorAction SilentlyContinue
        if ($wasRegistered) { Register-Driver }
    } finally {
        if (Test-Path -LiteralPath $app -PathType Container) {
            try { Restore-PackageProcess $snapshot $state }
            finally {
                if ($state.active) { Invoke-Control resume @([string][int][bool]$state.run) }
            }
        }
    }
    return $state
}

function Invoke-PackageUpdate([string]$UpdateArchive, [bool]$SkipRestart) {
    $lock = Join-Path $packageRoot '.update-lock'
    # Directory.CreateDirectory succeeds for existing paths; New-Item must reject a concurrent updater.
    New-Item -ItemType Directory -Path $lock -ErrorAction Stop | Out-Null
    $temporary = Join-Path $packageRoot ('.update-' + [guid]::NewGuid().ToString('N'))
    try {
        New-Item -ItemType Directory -Path $temporary | Out-Null
        $current = Get-PackageInfo $packageRoot
        $releaseTag = $null
        $localArchive = $UpdateArchive
        if (-not $localArchive) {
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
            $release = Invoke-RestMethod 'https://api.github.com/repos/steffandonal/sranipatch/releases/latest'
            $releaseTag = $release.tag_name
            if ($releaseTag -eq $current.version) { Write-Information 'SRanipatch is already up to date.' -InformationAction Continue; return }
            $filename = 'SRanipatch-Runtime.win-x64.zip'
            $asset = $release.assets | Where-Object name -eq $filename
            $checksum = $release.assets | Where-Object name -eq "$filename.sha256"
            if (-not $asset -or -not $checksum) { throw "Release is missing $filename or its checksum" }
            $localArchive = Join-Path $temporary $filename
            Invoke-WebRequest -UseBasicParsing $asset.browser_download_url -OutFile $localArchive
            $checksumPath = Join-Path $temporary 'checksum'
            Invoke-WebRequest -UseBasicParsing $checksum.browser_download_url -OutFile $checksumPath
            $expected = ((Get-Content -Raw -LiteralPath $checksumPath).Trim() -split '\s+')[0]
            $sha256 = [Security.Cryptography.SHA256]::Create()
            $archiveStream = [IO.File]::OpenRead($localArchive)
            try { $actual = [BitConverter]::ToString($sha256.ComputeHash($archiveStream)).Replace('-', '') }
            finally { $archiveStream.Dispose(); $sha256.Dispose() }
            if ($actual -ne $expected) { throw 'Release checksum does not match' }
        }
        $staged = Join-Path $temporary 'package'
        Expand-Release ([IO.Path]::GetFullPath($localArchive)) $staged
        $incoming = Assert-Release $staged $current.platform
        if ($releaseTag -and $incoming.version -ne $releaseTag) { throw 'Release metadata does not match its tag' }
        if ($incoming.version -eq $current.version) {
            Write-Information 'SRanipatch is already up to date.' -InformationAction Continue
            return
        }
        $state = Install-Update $staged
        Write-Information "Updated SRanipatch to $($incoming.version)." -InformationAction Continue
        if ($state.active -and $state.revision -ne $incoming.driver_revision) { Show-RestartOffer $SkipRestart }
    } finally {
        if ([IO.Path]::GetDirectoryName($temporary) -ne $packageRoot) { throw 'Invalid update staging directory' }
        Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $lock -Force
    }
}

$waitForEnter = $Command -eq 'uninstall'
try {
    Invoke-OldPackageCleanup
    switch ($Command) {
        { $_ -in 'start-server', 'start-client' } {
            Push-Location (Join-Path $packageRoot 'app/bin')
            try {
                $application = $Command.Substring('start-'.Length)
                & (Join-Path $packageRoot "app/bin/sranipatch-$application.exe") @ApplicationArguments
                exit $LASTEXITCODE
            } finally { Pop-Location }
        }
        install {
            $steamVRWasRunning = [bool](Get-Process vrserver -ErrorAction SilentlyContinue)
            $waitForEnter = -not $steamVRWasRunning
            Register-Driver
            Write-Information "Installed successfully. SRanipatch will auto-start with SteamVR so long as it's installed and enabled under SteamVR's settings." -InformationAction Continue
            if ($steamVRWasRunning) { Show-RestartOffer $NoRestart }
        }
        uninstall {
            Register-Driver -Remove
            Write-Information 'You may now delete this folder to finish uninstalling' -InformationAction Continue
        }
        update { Invoke-PackageUpdate $Archive $NoRestart }
    }
} catch {
    [Console]::Error.WriteLine("SRanipatch: $_")
    exit 1
} finally {
    if ($waitForEnter) { Read-Host 'Press Enter to exit' | Out-Null }
}
