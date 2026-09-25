#!/usr/bin/env python3
"""
AI Status hook 安装器：把 ESP32 红绿灯 hooks 同时装进 Claude Code 和 ZCode

安全策略（学自 ai-light）：
  1. 写入前先备份到 <文件名>.bak-<时间戳>
  2. 只追加/更新我们自己的 hook 条目（按标记识别），已有其它 hooks 原样保留
  3. --remove 参数可完整卸载

用法:
  python install_hooks.py            # 安装/更新（两个工具都装）
  python install_hooks.py --remove   # 卸载

两个工具的适配差异（学习点：模式通用，配置不通用）：
  Claude Code: ~/.claude/settings.json 的 hooks.<Event>，command 型
  ZCode:       ~/.zcode/cli/config.json 的 hooks.events.<Event>，process 型，
               且必须 hooks.enabled=true（配置文件 hooks 默认关闭！）
"""
import json
import shutil
import sys
from datetime import datetime
from pathlib import Path

HOOK_DIR = Path(__file__).resolve().parent
WRAPPER = HOOK_DIR / "ai_status_hook.cmd"
CLAUDE_SETTINGS = Path.home() / ".claude" / "settings.json"
ZCODE_SETTINGS = Path.home() / ".zcode" / "cli" / "config.json"
# 板子地址（IP 变了改这里，重跑本脚本即可）
BOARD_URL = "http://192.168.1.20"
# 识别"我们装的 hook"的标记
HOOK_MARKERS = ["esp_light_hook.ps1", "/events?event_type=", "ai_status_hook.cmd"]

# ---- 事件映射：工具原生事件名 -> ai-light 协议参数 ----
CLAUDE_EVENTS = {
    "SessionStart": "session-start",
    "UserPromptSubmit": "prompt-submit",
    "PreToolUse": "pre-tool-use",
    "PostToolUse": "post-tool-use",
    "Notification": "notification",       # Claude 用 Notification 承载权限请求
    "Stop": "stop",
    "SessionEnd": "session-end",
}
ZCODE_EVENTS = {
    "SessionStart": "session-start",
    "UserPromptSubmit": "prompt-submit",
    "PreToolUse": "pre-tool-use",
    "PostToolUse": "post-tool-use",
    "PostToolUseFailure": "post-tool-use",  # 工具失败但 AI 仍在处理，仍是"工作中"
    "PermissionRequest": "permission-request",  # ZCode 有原生权限事件，直接映射红灯
    "Stop": "stop",
    # ZCode 没有 SessionEnd / Notification，不注册
}

# 批处理包装（ASCII only！见问题记录 #004 的中文 .bat 编码坑）
WRAPPER_TEMPLATE = (
    "@echo off\r\n"
    "rem AI Status hook wrapper v5: queue-to-file (#030)\r\n"
    "rem Network moved OFF the AI critical path: hook only appends a local file (~30ms);\r\n"
    "rem the resident watchdog forwards the queue to the board with retries.\r\n"
    "rem %1=event, %2=tool source (claude/zcode)\r\n"
    "echo [%date% %time%] ev=%~1 src=%~2 cwd=%cd% >> \"%USERPROFILE%\\.ai_status\\hook_exec.log\"\r\n"
    "set \"QDIR=%USERPROFILE%\\.ai_status\\queue\"\r\n"
    "if not exist \"%QDIR%\" mkdir \"%QDIR%\"\r\n"
    "findstr /R \".*\" > \"%QDIR%\\%~1_%~2_%RANDOM%%RANDOM%.ev\"\r\n"
    "exit /b 0\r\n"
)


def is_ours(hook: dict) -> bool:
    blob = json.dumps(hook.get("args", [])) + hook.get("command", "")
    return any(m in blob for m in HOOK_MARKERS)


def merge_groups(groups: list, arg: str, entry: dict, remove: bool,
                 find_group, new_group: dict):
    """通用的"事件组列表"合并：清掉我们的旧条目，再追加新条目（或卸载）"""
    for g in groups:
        g["hooks"] = [h for h in g.get("hooks", []) if not is_ours(h)]
    if remove:
        return [g for g in groups if g.get("hooks")]
    target = find_group(groups)
    if target is None:
        import copy
        target = copy.deepcopy(new_group)
        target["hooks"] = []
        groups.append(target)
    target["hooks"].append(entry(arg))
    return groups


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}


def save_json(path: Path, data: dict, label: str):
    if path.exists():
        bak = path.with_name(f"{path.name}.bak-{datetime.now():%Y%m%d-%H%M%S}")
        shutil.copy2(path, bak)
        print(f"  已备份: {bak}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"  {label} 完成 -> {path}")


def install_claude(remove: bool):
    token_hook = str(HOOK_DIR / "claude_token_hook.py")

    def entry(arg):
        if arg == "stop":
            # Stop 换专用脚本：附带 transcript 的 token 用量（每轮一次，300ms 可接受）
            return {"type": "command", "command": "python",
                    "args": ["-X", "utf8", token_hook]}
        return {"type": "command", "command": "cmd",
                "args": ["/c", str(WRAPPER), arg, "claude"]}

    settings = load_json(CLAUDE_SETTINGS)
    hooks = settings.setdefault("hooks", {})
    for event, arg in CLAUDE_EVENTS.items():
        groups = merge_groups(hooks.setdefault(event, []), arg, entry, remove,
                              lambda gs: next((g for g in gs if g.get("matcher", "") == ""), None),
                              {"matcher": "", "hooks": []})
        if groups:
            hooks[event] = groups
        else:
            hooks.pop(event, None)
    if remove and not hooks:
        settings.pop("hooks", None)
    save_json(CLAUDE_SETTINGS, settings, "Claude Code hooks " + ("卸载" if remove else "安装"))


def install_zcode(remove: bool):
    """插件 hooks 是 ZCode 的唯一事件通道；本函数只负责清理 config.json
    里旧版安装的 hooks（v4 wrapper 让 process 通路复活后会造成双重触发 #022）"""
    settings = load_json(ZCODE_SETTINGS)
    hooks = settings.get("hooks", {})
    events = hooks.get("events", {})
    for event in list(events.keys()):
        groups = events[event]
        for g in groups:
            g["hooks"] = [h for h in g.get("hooks", []) if not is_ours(h)]
        groups[:] = [g for g in groups if g.get("hooks")]
        if not groups:
            events.pop(event, None)
    if not events:
        hooks.pop("events", None)
    if not hooks:
        settings.pop("hooks", None)
    save_json(ZCODE_SETTINGS, settings, "ZCode config hooks 清理(插件为唯一通道)")


def main() -> int:
    remove = "--remove" in sys.argv
    if not remove:
        WRAPPER.write_text(WRAPPER_TEMPLATE, encoding="ascii", newline="")
        alt = HOOK_DIR.parent.parent / "hook" / "ai_status_hook.cmd"  # ZCode插件引用根目录(#021)
        alt.parent.mkdir(parents=True, exist_ok=True)
        alt.write_text(WRAPPER_TEMPLATE, encoding="ascii", newline="")
        print(f"包装脚本已双写: {WRAPPER} + {alt}")
        print("安装到 Claude Code 和 ZCode:")
    install_claude(remove)
    install_zcode(remove)
    return 0


if __name__ == "__main__":
    sys.exit(main())
