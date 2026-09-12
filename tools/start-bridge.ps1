# Start Clawd Mochi M5 bridge in background
# Usage: .\start-bridge.ps1 [-Port COM3] [-DryRun]
param(
  [string]$Port = "COM3",
  [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$syspy = "C:\Users\pettycc\AppData\Local\Programs\Python\Python313\python.exe"
if (-not (Test-Path $syspy)) {
  $cmd = Get-Command python -ErrorAction SilentlyContinue
  if ($cmd) { $syspy = $cmd.Source } else { Write-Host "[!] Python not found"; exit 1 }
}

$script = Join-Path $PSScriptRoot "mochi-bridge.py"
$logDir = Join-Path $PSScriptRoot "logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log = Join-Path $logDir "mochi-bridge.log"

# Stop existing bridge
$procs = Get-CimInstance Win32_Process -Filter "Name='python.exe'"
foreach ($proc in $procs) {
  if ($proc.CommandLine -and $proc.CommandLine -like '*mochi-bridge.py*') {
    Write-Host "Stopping old bridge PID=$($proc.ProcessId)"
    Stop-Process -Id $proc.ProcessId -Force -ErrorAction SilentlyContinue
  }
}

$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'

# Build a single quoted argument string (handles spaces + CJK path)
$arg = '"' + $script + '" --port ' + $Port + ' --log "' + $log + '"'
if ($DryRun) { $arg += ' --dry-run' }

$p = Start-Process -FilePath $syspy -ArgumentList $arg -WindowStyle Hidden -PassThru

Start-Sleep -Milliseconds 1200
Write-Host "Bridge started PID=$($p.Id) port=$Port"
Write-Host "Log: $log"
if (Test-Path $log) {
  Get-Content $log -Encoding utf8 | Select-Object -Last 15
} else {
  Write-Host "(log not created yet)"
}
