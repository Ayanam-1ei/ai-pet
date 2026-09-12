# Start Clawd Mochi Pixel Studio
# Usage: .\start-pixel-studio.ps1 [-Port COM3] [-NoOpen]
param(
  [string]$Port = "COM3",
  [switch]$NoOpen
)

$ErrorActionPreference = 'Stop'
$syspy = "C:\Users\pettycc\AppData\Local\Programs\Python\Python313\python.exe"
if (-not (Test-Path $syspy)) {
  $cmd = Get-Command python -ErrorAction SilentlyContinue
  if ($cmd) { $syspy = $cmd.Source } else { Write-Host "[!] Python not found"; exit 1 }
}

$script = Join-Path $PSScriptRoot "pixel-studio.py"

# Free the serial port from mochi-bridge
Get-CimInstance Win32_Process -Filter "Name='python.exe'" | ForEach-Object {
  if ($_.CommandLine -and $_.CommandLine -like '*mochi-bridge.py*') {
    Write-Host "Stopping mochi-bridge PID=$($_.ProcessId)"
    Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
  }
}

$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'

$arg = '"' + $script + '" --port ' + $Port + ' --stop-bridge'
if ($NoOpen) { $arg += ' --no-open' }

$p = Start-Process -FilePath $syspy -ArgumentList $arg -WindowStyle Hidden -PassThru
Start-Sleep -Milliseconds 1500
Write-Host "Pixel Studio PID=$($p.Id)"
Write-Host "URL: http://127.0.0.1:8765/"
Write-Host "提示: 画完点「推送到麻薯」；要恢复自动表情请再跑 start-bridge.ps1"
