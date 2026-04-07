param(
    [string]$ProjectPath = $PWD,
    [ValidateSet("usb", "ota", "last")]
    [string]$Mode = "last",
    [string]$UsbPort = "",
    [string]$DeviceIp = "",
    [string]$HostIp = "",
    [int]$HttpPort = 8070,
    [int]$StatusTimeoutSec = 240
)

$scriptPath = Join-Path $ProjectPath "tools\flash-mode.ps1"
if (-not (Test-Path $scriptPath)) {
    Write-Host "❌ Flash script not found: $scriptPath" -ForegroundColor Red
    exit 1
}

Write-Host "⚡ Running flash workflow..." -ForegroundColor Cyan
$arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $scriptPath)
if ($Mode) { $arguments += "-Mode"; $arguments += $Mode }
if ($UsbPort) { $arguments += "-UsbPort"; $arguments += $UsbPort }
if ($DeviceIp) { $arguments += "-DeviceIp"; $arguments += $DeviceIp }
if ($HostIp) { $arguments += "-HostIp"; $arguments += $HostIp }
if ($HttpPort) { $arguments += "-HttpPort"; $arguments += $HttpPort }
if ($StatusTimeoutSec) { $arguments += "-StatusTimeoutSec"; $arguments += $StatusTimeoutSec }

& powershell @arguments
exit $LASTEXITCODE
