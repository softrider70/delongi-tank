param(
    [string]$ProjectPath = "$PSScriptRoot\.."
)

$ProjectPath = Resolve-Path -Path $ProjectPath
Set-Location $ProjectPath

# ESP-IDF Pfad setzen
$espIdfPath = "C:\esp\v6.0\esp-idf"

if (-not (Test-Path $espIdfPath)) {
    Write-Host "❌ ESP-IDF nicht gefunden: $espIdfPath" -ForegroundColor Red
    exit 1
}

Write-Host "🔧 ESP-IDF Pfad: $espIdfPath" -ForegroundColor Cyan
$env:IDF_PATH = $espIdfPath

# Build-Nummer inkrementieren
$incrementScript = Join-Path $ProjectPath "tools\increment_build.py"
if (Test-Path $incrementScript) {
    Write-Host "🔢 Incrementing build number..." -ForegroundColor Cyan
    & python "$incrementScript"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "❌ Build number increment failed." -ForegroundColor Red
        exit $LASTEXITCODE
    }
} else {
    Write-Host "⚠️ Build increment script not found, skipping..." -ForegroundColor Yellow
}

# Build ausführen
Write-Host "🔨 Building project..." -ForegroundColor Cyan
& python "$espIdfPath\tools\idf.py" build
$buildExitCode = $LASTEXITCODE

if ($buildExitCode -ne 0) {
    Write-Host "❌ Build failed!" -ForegroundColor Red
    exit $buildExitCode
}

Write-Host "✅ Build successful!" -ForegroundColor Green
