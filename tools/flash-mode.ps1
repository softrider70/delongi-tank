param(
    [ValidateSet("usb", "ota", "last")]
    [string]$Mode = "last",

    [string]$UsbPort = "",
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

function Get-AutoUsbPort {
    $ports = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
    if ($ports.Count -eq 0) {
        throw "Keine seriellen Ports gefunden. Bitte -UsbPort explizit setzen."
    }

    if ($ports.Count -eq 1) {
        return $ports[0]
    }

    if ($ports -contains 'COM3') {
        return 'COM3'
    }

    Write-Host "Verfügbare serielle Ports: $($ports -join ', ')" -ForegroundColor Yellow
    throw "Mehrere serielle Ports gefunden. Bitte -UsbPort explizit setzen."
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $repoRoot

$env:ESP_IDF_VERSION = "6.0"
. .\activate-esp-idf.ps1

$stateFile = Join-Path $repoRoot ".last_flash_mode.json"
$previousState = $null
if (Test-Path $stateFile) {
    try {
        $previousState = Get-Content -Raw $stateFile | ConvertFrom-Json
    } catch {
        Write-Host "⚠️ Konnte letzte Flash-Konfiguration nicht laden. Ignoriere sie." -ForegroundColor Yellow
    }
}

function Get-GitCommitHash {
    try {
        return (& git rev-parse HEAD 2>$null).Trim()
    } catch {
        return $null
    }
}

function Is-GitTreeDirty {
    try {
        & git diff --quiet --ignore-submodules --
        $unstaged = $LASTEXITCODE
    } catch {
        $unstaged = 1
    }

    try {
        & git diff --cached --quiet --ignore-submodules --
        $staged = $LASTEXITCODE
    } catch {
        $staged = 1
    }

    try {
        $untracked = (& git ls-files --others --exclude-standard 2>$null)
        $hasUntracked = -not [string]::IsNullOrWhiteSpace($untracked)
    } catch {
        $hasUntracked = $false
    }

    return ($unstaged -ne 0 -or $staged -ne 0 -or $hasUntracked)
}

function Needs-Build {
    param(
        [string]$BinaryPath,
        [string]$CommitStatePath
    )

    if (-not (Test-Path $BinaryPath)) {
        Write-Host "[BUILD CHECK] Kein Build vorhanden. Build wird ausgeführt." -ForegroundColor Yellow
        return $true
    }

    if (-not (Test-Path $CommitStatePath)) {
        Write-Host "[BUILD CHECK] Kein letztes Build-Commit gefunden. Build wird ausgeführt." -ForegroundColor Yellow
        return $true
    }

    $currentCommit = Get-GitCommitHash
    if (-not $currentCommit) {
        Write-Host "[BUILD CHECK] Git Commit kann nicht ermittelt werden. Build wird sicherheitshalber ausgeführt." -ForegroundColor Yellow
        return $true
    }

    $lastBuiltCommit = Get-Content -Raw $CommitStatePath
    if ($currentCommit -ne $lastBuiltCommit) {
        Write-Host "[BUILD CHECK] Neuer Commit erkannt (aktuell: $currentCommit, letzter Build: $lastBuiltCommit). Build wird ausgeführt." -ForegroundColor Yellow
        return $true
    }

    if (Is-GitTreeDirty) {
        Write-Host "[BUILD CHECK] Uncommitted Änderungen vorhanden. Build wird ausgeführt." -ForegroundColor Yellow
        return $true
    }

    Write-Host "[BUILD CHECK] Build ist aktuell für Commit $currentCommit." -ForegroundColor Green
    return $false
}

if ($Mode -eq 'last') {
    if ($previousState -ne $null -and $previousState.mode) {
        $Mode = $previousState.mode
        Write-Host "[FLASH] Verwende letzten Modus: $Mode" -ForegroundColor Cyan
    } else {
        Write-Host "[FLASH] Kein letzter Flash-Modus gefunden, verwende usb als Standard." -ForegroundColor Yellow
        $Mode = 'usb'
    }
}

if ($Mode -eq 'usb') {
    if ([string]::IsNullOrWhiteSpace($UsbPort) -and $previousState -ne $null -and $previousState.usbPort) {
        $UsbPort = $previousState.usbPort
    }

    if ([string]::IsNullOrWhiteSpace($UsbPort)) {
        $UsbPort = Get-AutoUsbPort
        Write-Host "[FLASH] Auto-detected USB port: $UsbPort" -ForegroundColor Green
    }

    Write-Host "[FLASH] Mode=usb, Port=$UsbPort"

    $binPath = Join-Path $repoRoot "build\bosch-tank.bin"
    $lastBuiltCommitPath = Join-Path $repoRoot ".last_built_commit"
    if (Needs-Build -BinaryPath $binPath -CommitStatePath $lastBuiltCommitPath) {
        Write-Host "[FLASH] Baue Projekt vor USB-Flash..." -ForegroundColor Cyan
        & powershell -NoProfile -ExecutionPolicy Bypass -File "$repoRoot\tools\build-and-commit.ps1"
        if ($LASTEXITCODE -ne 0) {
            throw "Build vor USB-Flash fehlgeschlagen."
        }
    }

    python "$env:IDF_PATH\tools\idf.py" -p $UsbPort flash
    $exitCode = $LASTEXITCODE
    if ($exitCode -eq 0) {
        $state = @{ mode = 'usb'; usbPort = $UsbPort; deviceIp = ''; hostIp = ''; httpPort = $HttpPort }
        $state | ConvertTo-Json | Set-Content -NoNewline -Encoding UTF8 $stateFile
        Write-Host "[FLASH] Letzte Flash-Konfiguration gespeichert." -ForegroundColor Green
    }
    exit $exitCode
}

if ($Mode -eq 'ota') {
    if ([string]::IsNullOrWhiteSpace($DeviceIp) -and $previousState -ne $null -and $previousState.deviceIp) {
        $DeviceIp = $previousState.deviceIp
    }
    if ([string]::IsNullOrWhiteSpace($HostIp) -and $previousState -ne $null -and $previousState.hostIp) {
        $HostIp = $previousState.hostIp
    }
    if ($previousState -ne $null -and $previousState.httpPort) {
        $HttpPort = $previousState.httpPort
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

    $binPath = Join-Path $repoRoot "build\bosch-tank.bin"
    $lastBuiltCommitPath = Join-Path $repoRoot ".last_built_commit"
    if (Needs-Build -BinaryPath $binPath -CommitStatePath $lastBuiltCommitPath) {
        Write-Host "[FLASH] Baue Projekt vor OTA-Flash..." -ForegroundColor Cyan
        & powershell -NoProfile -ExecutionPolicy Bypass -File "$repoRoot\tools\build-and-commit.ps1"
        if ($LASTEXITCODE -ne 0) {
            throw "Build vor OTA fehlgeschlagen."
        }
    }

    $serverProc = $null
    try {
        $serverProc = Start-Process -FilePath "python" -ArgumentList "-m", "http.server", "$HttpPort", "--bind", "$HostIp" -WorkingDirectory (Join-Path $repoRoot "build") -PassThru -WindowStyle Hidden
        [System.Threading.Thread]::Sleep(1200)

        $otaUrl = "http://$HostIp`:$HttpPort/bosch-tank.bin"
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
                        $state = @{ mode = 'ota'; usbPort = ''; deviceIp = $DeviceIp; hostIp = $HostIp; httpPort = $HttpPort }
                        $state | ConvertTo-Json | Set-Content -NoNewline -Encoding UTF8 $stateFile
                        Write-Host "[FLASH] Letzte Flash-Konfiguration gespeichert." -ForegroundColor Green
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
}

