# Clawd Mochi · 串口状态测试发送器
# 用法: .\send-state.ps1 <状态词>
#   状态词: idle thinking reading coding running delegating planning
#           waiting compacting notify done error sleep
#   另支持: bg:#RRGGBB  light:on  light:off
# 注意: 发送时不要开着 Arduino Serial Monitor (会占住 COM 口)
param(
  [Parameter(Mandatory = $true)]
  [string]$State
)

$ErrorActionPreference = 'SilentlyContinue'
$ComCache = Join-Path $env:USERPROFILE ".clawd_mochi_com"   # 与成品版 cc/cx 发送器共用缓存

function Find-MochiCom {
  $devs = Get-CimInstance Win32_PnPEntity |
          Where-Object { $_.PNPDeviceID -match 'VID_303A&PID_1001' -and $_.Name -match 'COM(\d+)' }
  foreach ($d in $devs) {
    if ($d.Name -match 'COM(\d+)') { return "COM$($Matches[1])" }
  }
  $all = [System.IO.Ports.SerialPort]::GetPortNames()
  if ($all -and @($all).Count -eq 1) { return $all[0] }
  return $null
}

$port = (Get-Content -LiteralPath $ComCache) 2>$null
if ($port) { $port = $port.Trim() }

$sp = $null
$opened = $false
try {
  if ($port) {
    $sp = New-Object System.IO.Ports.SerialPort($port, 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $sp.NewLine = "`n"; $sp.ReadTimeout = 200; $sp.WriteTimeout = 400
    $sp.DtrEnable = $true   # "终端已连接" 标志
    $sp.RtsEnable = $false  # 不拉 RTS, 避免误复位
    $sp.Open(); $opened = $true
  }
} catch {}

if (-not $opened) {
  $port = Find-MochiCom
  if ($port) {
    Set-Content -LiteralPath $ComCache -Value $port -NoNewline
    $sp = New-Object System.IO.Ports.SerialPort($port, 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $sp.NewLine = "`n"; $sp.ReadTimeout = 200; $sp.WriteTimeout = 400
    $sp.DtrEnable = $true; $sp.RtsEnable = $false
    $sp.Open(); $opened = $true
  }
}

if (-not $opened) {
  Write-Host "[!] 设备不可达: 没检测到 COM 口, 或端口被占用 (关掉 Serial Monitor 再试)"
  exit 1
}

$sp.WriteLine($State)
Start-Sleep -Milliseconds 25   # 等待发送完成再关口
$sp.Close()
Write-Host "已发送 '$State' -> $port"
