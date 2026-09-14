# xm-serial.ps1 - 零依赖串口日志采集（PowerShell 5.1 可用，不需要 npm serialport）
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File .\xm-serial.ps1
#   powershell -ExecutionPolicy Bypass -File .\xm-serial.ps1 -Port COM6 -Baud 1000000 -Seconds 30
#
# 参数:
#   -Port     串口号，默认 COM6
#   -Baud     波特率，默认 1000000（思澈默认）
#   -Seconds  采集秒数，0 = 一直采到 Ctrl+C，默认 30
#   -Out      输出文件，默认 ../logs/serial-<时间戳>.log
#   -Highlight 关键字（逗号分隔），命中行加！！标记

param(
    [string]$Port = "COM6",
    [int]$Baud = 1000000,
    [int]$Seconds = 30,
    [string]$Out = "",
    [string]$Highlight = "ERROR,FAIL,ASSERT,RESET,HardFault,wdt,reboot"
)

$ErrorActionPreference = "Continue"

if ([string]::IsNullOrWhiteSpace($Out)) {
    $logDir = Join-Path (Split-Path $PSScriptRoot -Parent) "logs"
    if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
    $Out = Join-Path $logDir ("serial-{0}.log" -f (Get-Date -Format "yyyyMMdd-HHmmss"))
}

$hot = @()
if (-not [string]::IsNullOrWhiteSpace($Highlight)) {
    $hot = $Highlight.Split(",") | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" }
}

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, "None", 8, "One"
$sp.ReadTimeout = 200
$sp.DtrEnable = $false
$sp.RtsEnable = $false

try {
    $sp.Open()
} catch {
    Write-Host "打开 $Port 失败: $($_.Exception.Message)"
    exit 1
}

Write-Host "采集 $Port @ $Baud ... 输出 -> $Out  (Ctrl+C 结束)"
$hits = 0
$lines = 0
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$buf = New-Object byte[] 4096
$sb = New-Object System.Text.StringBuilder

function Flush-Line($text) {
    if ([string]::IsNullOrEmpty($text)) { return }
    $t = $text.TrimEnd()
    if ($t.Length -eq 0) { return }
    $script:lines++
    $flag = ""
    foreach ($k in $hot) {
        if ($t -match [regex]::Escape($k)) { $flag = " !!"; $script:hits++; break }
    }
    $line = "[{0}] {1}{2}" -f (Get-Date -Format "HH:mm:ss.fff"), $t, $flag
    Write-Host $line
    Add-Content -Path $Out -Value $line -Encoding UTF8
}

while ($true) {
    if ($Seconds -gt 0 -and $sw.Elapsed.TotalSeconds -ge $Seconds) { break }
    try {
        $n = $sp.Read($buf, 0, $buf.Length)
        if ($n -gt 0) {
            for ($i = 0; $i -lt $n; $i++) {
                $c = $buf[$i]
                if ($c -eq 10) {
                    Flush-Line $sb.ToString()
                    [void]$sb.Clear()
                } elseif ($c -eq 13) {
                    # ignore CR
                } elseif ($c -ge 32 -and $c -lt 127) {
                    [void]$sb.Append([char]$c)
                } elseif ($c -eq 9) {
                    [void]$sb.Append(" ")
                } else {
                    [void]$sb.Append(".")
                }
            }
        }
    } catch [TimeoutException] {
    } catch {
        Write-Host "读取出错: $($_.Exception.Message)"
        break
    }
}

Flush-Line $sb.ToString()
try { $sp.Close() } catch {}

Write-Host ""
Write-Host "共 $lines 行，命中关键字 $hits 行。日志: $Out"
