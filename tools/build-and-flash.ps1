# Clawd Mochi · 编译/烧录脚本
# 用法:
#   .\build-and-flash.ps1 -Setup    # 首次运行: 安装 esp32 核心与依赖库 (下载约 300MB)
#   .\build-and-flash.ps1           # 仅编译
#   .\build-and-flash.ps1 -Flash    # 编译 + 烧录到自动检测的 COM 口
#   .\build-and-flash.ps1 -Flash -Port COM5   # 手动指定端口
param(
  [switch]$Setup,
  [switch]$Flash,
  [string]$Port
)

$ErrorActionPreference = 'Stop'
$Sketch = Join-Path $PSScriptRoot "..\clawd_mochi"
# 关键: CDCOnBoot=cdc 即 "USB CDC On Boot=Enabled"(有线版命脉) / PartitionScheme=huge_app 容纳大素材
$Fqbn = "esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=huge_app,CPUFreq=160,UploadSpeed=921600"
$CoreUrl = "https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json"

# arduino-cli 优先用项目内自带的 tools\arduino-cli.exe (便携版, 不装进系统)
$Cli = Join-Path $PSScriptRoot "arduino-cli.exe"
if (-not (Test-Path $Cli)) {
  $cmd = Get-Command arduino-cli -ErrorAction SilentlyContinue
  if ($cmd) { $Cli = "arduino-cli" } else { $Cli = $null }
}
if (-not $Cli) {
  Write-Host "[!] 未找到 arduino-cli (tools\arduino-cli.exe 或 PATH)。先安装 (任选其一):"
  Write-Host "    1. 浏览器下载 https://github.com/arduino/arduino-cli/releases 里的"
  Write-Host "       arduino-cli_latest_Windows_64bit.zip, 解压出 exe 放到本 tools 文件夹"
  Write-Host "    2. winget install ArduinoSA.CLI"
  exit 1
}
Write-Host "使用 arduino-cli: $Cli"

if ($Setup) {
  Write-Host "[1/3] 更新索引并安装 esp32 核心 ..."
  & $Cli core update-index --additional-urls $CoreUrl
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  & $Cli core install esp32:esp32 --additional-urls $CoreUrl
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  Write-Host "[2/3] 安装依赖库 ..."
  & $Cli lib install "Adafruit GFX Library" "Adafruit ST7735 and ST7789 Library"
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  Write-Host "[3/3] Setup 完成。"
}

Write-Host "编译: $Sketch"
& $Cli compile -b $Fqbn $Sketch
if ($LASTEXITCODE -ne 0) {
  Write-Host "[x] 编译失败。若报路径相关错误(中文路径偶发兼容问题), 建议把项目移到纯英文路径再试。"
  exit $LASTEXITCODE
}

if ($Flash) {
  if (-not $Port) {
    $dev = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
           Where-Object { $_.PNPDeviceID -match 'VID_303A&PID_1001' -and $_.Name -match 'COM(\d+)' } |
           Select-Object -First 1
    if ($dev -and $dev.Name -match 'COM(\d+)') { $Port = "COM$($Matches[1])" }
  }
  if (-not $Port) {
    Write-Host "[!] 未检测到设备 (VID_303A&PID_1001)。插好数据线后重试, 或用 -Port COMx 指定"
    exit 1
  }
  Write-Host "烧录到 $Port ..."
  & $Cli upload -b $Fqbn -p $Port $Sketch
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  Write-Host "完成。烧录后等设备重启, 然后测试:"
  Write-Host "  ..\tools\send-state.ps1 done"
}
