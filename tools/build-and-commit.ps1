param(
    [string]$ProjectPath = "$PSScriptRoot\.."
)

$ProjectPath = Resolve-Path -Path $ProjectPath
Set-Location $ProjectPath

$scriptRoot = Resolve-Path -Path $PSScriptRoot
$activateScript = Resolve-Path -Path (Join-Path $scriptRoot "..\activate-esp-idf.ps1") -ErrorAction SilentlyContinue

if (-not $env:IDF_PATH) {
    if ($activateScript) {
        Write-Host "🔧 Activating ESP-IDF environment..." -ForegroundColor Cyan
        . $activateScript
    }
}

if (-not $env:IDF_PATH) {
    Write-Host "❌ ESP-IDF environment is not configured. Run .\activate-esp-idf.ps1 first." -ForegroundColor Red
    exit 1
}

$incrementScript = Join-Path $ProjectPath "tools\increment_build.py"
if (-not (Test-Path $incrementScript)) {
    Write-Host "❌ Build increment script not found: $incrementScript" -ForegroundColor Red
    exit 1
}

Write-Host "🔢 Incrementing build number..." -ForegroundColor Cyan
$incrementOutput = & python "$incrementScript" 2>&1
$incrementExitCode = $LASTEXITCODE
if ($incrementExitCode -ne 0) {
    Write-Host "❌ Build number increment failed." -ForegroundColor Red
    Write-Host $incrementOutput
    exit $incrementExitCode
}

Write-Host "🔨 Building project in $ProjectPath..." -ForegroundColor Cyan
$buildOutput = & python "$env:IDF_PATH\tools\idf.py" build 2>&1
$buildExitCode = $LASTEXITCODE

$versionHeaderPath = Join-Path $ProjectPath "include\version.h"

if ($buildExitCode -ne 0) {
    Write-Host "❌ Build failed!" -ForegroundColor Red
    Write-Host $buildOutput

    if (-not (Test-Path $versionHeaderPath)) {
        Write-Host "🔄 Regenerating version header and retrying build..." -ForegroundColor Yellow
        & python "$incrementScript" | Out-Null
        $buildOutput = & python "$env:IDF_PATH\tools\idf.py" build 2>&1
        $buildExitCode = $LASTEXITCODE
    }

    if ($buildExitCode -ne 0) {
        Write-Host "❌ Build still failed after retry." -ForegroundColor Red
        Write-Host $buildOutput
        exit $buildExitCode
    }
}

Write-Host "✅ Build successful!" -ForegroundColor Green

$currentCommit = $null
try {
    $currentCommit = (& git rev-parse HEAD 2>$null).Trim()
} catch {
}

$lastBuiltCommitPath = Join-Path $ProjectPath ".last_built_commit"
if ($currentCommit) {
    Set-Content -Path $lastBuiltCommitPath -Value $currentCommit -Encoding UTF8
    Write-Host "📌 Recorded last built commit: $currentCommit" -ForegroundColor Gray
}

$buildNumberPath = Join-Path $ProjectPath ".build_number"
$lastVersionPath = Join-Path $ProjectPath ".last_version"
$gitAddFiles = @()

if (Test-Path $buildNumberPath) { $gitAddFiles += $buildNumberPath }
if (Test-Path $lastVersionPath) { $gitAddFiles += $lastVersionPath }
if (Test-Path $versionHeaderPath) { $gitAddFiles += $versionHeaderPath }

if ($gitAddFiles.Count -eq 0) {
    Write-Host "⚠️ No build metadata files found to commit." -ForegroundColor Yellow
    exit 0
}

Write-Host "📦 Staging build metadata files..." -ForegroundColor Cyan
& git add -f @gitAddFiles

$buildNumber = if (Test-Path $buildNumberPath) { Get-Content $buildNumberPath -Raw } else { "?" }
$versionString = "build #$buildNumber"
if (Test-Path $versionHeaderPath) {
    $match = Select-String -Path $versionHeaderPath -Pattern 'APP_FULL_VERSION\s+"(.+)"'
    if ($match) {
        $versionString = $match.Matches[0].Groups[1].Value
    }
}

$commitMessage = "chore: auto-commit build metadata after successful build ($versionString)"
$commitResult = & git commit -m $commitMessage 2>&1
$commitExitCode = $LASTEXITCODE

if ($commitExitCode -eq 0) {
    Write-Host "✅ Build metadata committed." -ForegroundColor Green
} else {
    Write-Host "⚠️ No metadata changes to commit or commit failed." -ForegroundColor Yellow
    Write-Host $commitResult
}
