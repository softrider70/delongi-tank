param(
    [string]$ProjectPath = $PWD
)

$scriptPath = Join-Path $ProjectPath "tools\build-and-commit.ps1"
if (-not (Test-Path $scriptPath)) {
    Write-Host "❌ Build script not found: $scriptPath" -ForegroundColor Red
    exit 1
}

Write-Host "🔨 Running build workflow..." -ForegroundColor Cyan
& powershell -NoProfile -ExecutionPolicy Bypass -File $scriptPath
exit $LASTEXITCODE
