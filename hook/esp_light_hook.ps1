# AI Status hook —— Claude Code 事件转发到 ESP32 红绿灯
#
# 用法（由 Claude Code 自动调用，stdin 收 hook payload JSON）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File esp_light_hook.ps1 <event>
#
# 设计原则（学自 ai-light 项目的经验）：
#   1. 永远 exit 0：hook 失败绝不能影响 Claude Code 本体工作
#   2. 2 秒超时：ESP32 不在线时快速放弃
#   3. 写本地日志 ~/.ai_status/hook.log：出问题时可排查
param(
    [Parameter(Position = 0)][string]$EventType = ""
)

# ==== 配置区：板子 IP 变了只需要改这里（未来升级为 mDNS/配置文件）====
$TargetUrl = "http://192.168.1.20/events"

$LogDir = Join-Path $env:USERPROFILE ".ai_status"
$LogFile = Join-Path $LogDir "hook.log"

function Write-Log {
    param([string]$Message)
    try {
        if (-not (Test-Path $LogDir)) {
            New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
        }
        $ts = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
        Add-Content -Path $LogFile -Value "[$ts] $Message"
    } catch { }
}

if (-not $EventType) {
    Write-Log "ignored: missing event type argument"
    exit 0
}

# 读取 stdin：Claude Code 会把 {"session_id":..,"cwd":..,"hook_event_name":..} 喂进来
$stdinText = ""
try { $stdinText = [Console]::In.ReadToEnd() } catch { }

$SessionId = "?"
try {
    if ($stdinText -and $stdinText.Trim()) {
        $payload = $stdinText | ConvertFrom-Json
        if ($payload.session_id) { $SessionId = $payload.session_id }
    }
} catch {
    Write-Log "warn: stdin payload not valid json"
}

# Claude Code 原生事件名 -> ai-light 协议的 kebab-case
$EventMap = @{
    "SessionStart"      = "session-start"
    "UserPromptSubmit"  = "prompt-submit"
    "Notification"      = "notification"
    "Stop"              = "stop"
    "SessionEnd"        = "session-end"
    "PreToolUse"        = "pre-tool-use"
    "PostToolUse"       = "post-tool-use"
    "PermissionRequest" = "permission-request"
}
if ($EventMap.ContainsKey($EventType)) { $EventType = $EventMap[$EventType] }

$Body = @{
    event_type  = $EventType
    session_id  = $SessionId
    tool_source = "claude-code"
} | ConvertTo-Json -Compress

try {
    $resp = Invoke-WebRequest -UseBasicParsing -Uri $TargetUrl -Method Post `
        -Body $Body -ContentType "application/json" -TimeoutSec 3
    Write-Log "sent: event=$EventType session=$SessionId status=$($resp.StatusCode)"
} catch {
    Write-Log "failed: event=$EventType session=$SessionId error=$($_.Exception.Message)"
}

exit 0
