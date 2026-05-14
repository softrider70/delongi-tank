# Aktiviert die vorhandene ESP-IDF Installation fuer diese PowerShell-Session.
# Verwendung: . .\activate-esp-idf.ps1

param(
    [string]$IdfPath = $env:IDF_PATH,
    [string]$IdfToolsPath = $env:IDF_TOOLS_PATH
)

$idfCandidates = @(
    $IdfPath,
    "C:\Users\win4g\Downloads\GitHub\VS-Projekte\CascadeProjects\esp-idf",
    "C:\esp\v6.0\esp-idf",
    "C:\esp\esp-idf"
) | Where-Object { $_ -and (Test-Path $_) }

$resolvedIdfPath = $idfCandidates | Select-Object -First 1

if (-not $resolvedIdfPath) {
    Write-Error "ESP-IDF wurde nicht gefunden. Setze IDF_PATH oder uebergib -IdfPath."
    return
}

$exportScript = Join-Path $resolvedIdfPath "export.ps1"
if (-not (Test-Path $exportScript)) {
    Write-Error "export.ps1 wurde unter '$resolvedIdfPath' nicht gefunden."
    return
}

$env:IDF_PATH = $resolvedIdfPath
if ($IdfToolsPath) {
    $env:IDF_TOOLS_PATH = $IdfToolsPath
}

& $exportScript | Out-Host

Write-Host "ESP-IDF Umgebung aktiviert." -ForegroundColor Green
Write-Host "IDF_PATH: $env:IDF_PATH" -ForegroundColor Cyan
