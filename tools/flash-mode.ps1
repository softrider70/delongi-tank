param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("usb", "ota")]
    [string]$Mode,

    [string]$UsbPort = "COM3",
    [string]$DeviceIp = "",
    [string]$HostIp = "",
    [int]$HttpPort = 8070,
    [int]$StatusTimeoutSec = 240
)

$ErrorActionPreference = "Stop"

function Test-PrivateIp {
    param([string]$Ip)
    if ($Ip -match '^10\.') { return $true }
    if ($Ip -match '^192\.168\.') { return $true }
    if ($Ip -match '^172\.(1[6-9]|2[0-9]|3[0-1])\.') { return $true }
    return $false
}

function Get-AutoHostIp {
    $candidates = Get-NetIPAddress -AddressFamily IPv4 |
        Where-Object {
            $_.IPAddress -ne '127.0.0.1' -and
            $_.IPAddress -notlike '169.254*' -and
            (Test-PrivateIp $_.IPAddress)
        }

    $selected = $candidates | Select-Object -First 1
    if ($null -eq $selected) {
        throw "Keine private IPv4-Adresse gefunden. Bitte -HostIp explizit setzen."
    }

    return $selected.IPAddress
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $repoRoot

$env:ESP_IDF_VERSION = "6.0"
. .\activate-esp-idf.ps1

if ($Mode -eq "usb") {
    Write-Host "[FLASH] Mode=usb, Port=$UsbPort"
    python "$env:IDF_PATH\tools\idf.py" -p $UsbPort flash
    exit $LASTEXITCODE
}

if ([string]::IsNullOrWhiteSpace($DeviceIp)) {
    throw "Bei Mode=ota muss -DeviceIp gesetzt sein (z.B. 192.168.1.50)."
}

if ([string]::IsNullOrWhiteSpace($HostIp)) {
    $HostIp = Get-AutoHostIp
}

Write-Host "[FLASH] Mode=ota"
Write-Host "[FLASH] DeviceIp=$DeviceIp"
Write-Host "[FLASH] HostIp=$HostIp"
Write-Host "[FLASH] HttpPort=$HttpPort"

python "$env:IDF_PATH\tools\idf.py" build
if ($LASTEXITCODE -ne 0) {
    throw "Build fehlgeschlagen."
}

$binPath = Join-Path $repoRoot "build\delonghi-tank.bin"
if (-not (Test-Path $binPath)) {
    throw "Firmware-Binary nicht gefunden: $binPath"
}

$serverProc = $null
try {
    $serverProc = Start-Process -FilePath "python" -ArgumentList "-m", "http.server", "$HttpPort", "--bind", "$HostIp" -WorkingDirectory (Join-Path $repoRoot "build") -PassThru -WindowStyle Hidden
    [System.Threading.Thread]::Sleep(1200)

    $otaUrl = "http://$HostIp`:$HttpPort/delonghi-tank.bin"
    $startUri = "http://$DeviceIp/api/ota/start"
    $statusUri = "http://$DeviceIp/api/ota/status"
    $payload = @{ url = $otaUrl } | ConvertTo-Json -Compress

    Write-Host "[OTA] Start: $startUri"
    Write-Host "[OTA] URL:   $otaUrl"

    $startResp = Invoke-RestMethod -Method Post -Uri $startUri -ContentType "application/json" -Body $payload -TimeoutSec 15
    Write-Host ("[OTA] Antwort: {0}" -f ($startResp | ConvertTo-Json -Compress))

    $deadline = (Get-Date).AddSeconds($StatusTimeoutSec)
    do {
        try {
            $statusResp = Invoke-RestMethod -Method Get -Uri $statusUri -TimeoutSec 8
            $inProgress = [bool]$statusResp.ota.in_progress
            $phase = [string]$statusResp.ota.phase
            $msg = [string]$statusResp.ota.message
            Write-Host "[OTA] Status: $phase - $msg"

            if (-not $inProgress) {
                if ([bool]$statusResp.ota.last_result_ok) {
                    Write-Host "[OTA] Erfolgreich. Geraet startet neu."
                    exit 0
                }

                $err = [string]$statusResp.ota.last_error
                throw "OTA fehlgeschlagen: $err"
            }
        } catch {
            Write-Host "[OTA] Warte auf Status/Neustart..."
        }

        [System.Threading.Thread]::Sleep(2000)
    } while ((Get-Date) -lt $deadline)

    throw "OTA-Status Timeout nach $StatusTimeoutSec Sekunden."
}
finally {
    if ($null -ne $serverProc -and -not $serverProc.HasExited) {
        Stop-Process -Id $serverProc.Id -Force
    }
}
