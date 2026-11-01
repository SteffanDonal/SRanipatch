param(
    [Parameter(Mandatory = $true)]
    [string]$DriverPath,
    [switch]$Unregister
)
$ErrorActionPreference = 'Stop'
$driverRoot = (Resolve-Path -LiteralPath $DriverPath).Path
if (-not (Test-Path -LiteralPath (Join-Path $driverRoot 'driver.vrdrivermanifest') -PathType Leaf)) {
    throw 'DriverPath must contain driver.vrdrivermanifest'
}
$paths = Get-Content -Raw -LiteralPath (Join-Path $env:LOCALAPPDATA 'openvr/openvrpaths.vrpath') | ConvertFrom-Json
$vrpathreg = Join-Path $paths.runtime[0] 'bin/win64/vrpathreg.exe'
$operation = if ($Unregister) { 'removedriver' } else { 'adddriver' }
& $vrpathreg $operation $driverRoot
if ($LASTEXITCODE -ne 0) { throw "vrpathreg failed with exit code $LASTEXITCODE" }
