# Aktiviere die ESP-IDF Umgebung für diese PowerShell-Session
# Verwendung: .\activate-esp-idf.ps1

$IDF_PATH = "C:\esp\v6.0\esp-idf"
$IDF_TOOLS_PATH = "C:\Espressif\tools"

# Setze Umgebungsvariablen
$env:IDF_PATH = $IDF_PATH
$env:IDF_TOOLS_PATH = $IDF_TOOLS_PATH
$env:IDF_PYTHON_ENV_PATH = "C:\Espressif\tools\python\v6.0\venv"

# Aktiviere die Python venv
& "C:\Espressif\tools\python\v6.0\venv\Scripts\Activate.ps1"

# Setze PATH mit allen Tools
$toolsPaths = @(
    "C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64",
    "C:\Espressif\tools\cmake\4.0.3\bin",
    "C:\Espressif\tools\dfu-util\0.11\dfu-util-0.11-win64",
    "C:\Espressif\tools\esp-clang\esp-20.1.1_20250829\esp-clang\bin",
    "C:\Espressif\tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin",
    "C:\Espressif\tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\esp32ulp-elf\bin",
    "C:\Espressif\tools\ninja\1.12.1",
    "C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20251215\openocd-esp32\bin",
    "C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin",
    "C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin",
    $IDF_PATH
)

$env:PATH = ($toolsPaths + $env:PATH.Split(';')) -join ';'

Write-Host "✅ ESP-IDF Umgebung aktiviert!" -ForegroundColor Green
Write-Host "IDF_PATH: $env:IDF_PATH" -ForegroundColor Cyan
Write-Host ""
Write-Host "Du kannst nun 'idf.py build' verwenden" -ForegroundColor Yellow
