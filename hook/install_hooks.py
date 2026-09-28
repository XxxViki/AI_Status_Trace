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
# 板子地址(#085)：hook 侧不再写死——事件走本地队列由 watchdog 转发，
# 板子地址统一在 ~/.ai_status/board_url（环境变量 AI_STATUS_BOARD 可覆盖），
# 换网段只改那一个文件，本脚本无需重跑。
# 识别"我们装的 hook"的标记。
# Q13: 必须含 claude_token_hook.py——Stop 装的是它而非 wrapper，
# 标记表漏了它导致 --remove 卸不掉、每次重装再叠一条（实测叠过 2 条）
HOOK_MARKERS = ["esp_light_hook.ps1", "/events?event_type=", "ai_status_hook.cmd",
                "claude_token_hook.py", "trae_hook.py"]

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


def self_check():
    """Q13: 安装后自检——每个事件里"我们的"hook 条目必须恰好 1 条。
    重复叠加（曾因标记表漏了 token 脚本，Stop 组叠到 2 条，
    每次 Stop 起双份 Python 进程）要能立刻被看见"""
    dup = []
    settings = load_json(CLAUDE_SETTINGS)
    for event, groups in settings.get("hooks", {}).items():
        n = sum(1 for g in groups for h in g.get("hooks", []) if is_ours(h))
        if n > 1:
            dup.append(f"{event}x{n}")
    if dup:
        print(f"  !! 自检发现重复条目: {', '.join(dup)}（应为各 1 条）——请 --remove 后重装")
        return False
    print("  自检通过: 各事件 hook 条目无重复")
    return True


def install_trae(remove: bool):
    """#080: Trae(CN) 原生支持 hooks.json——
    全局配置 ~/.trae-cn/hooks.json（项目级 .trae/hooks.json 可覆盖）。
    
    v5.5 用 Python 包装脚本 trae_hook.py——
    原因: Trae 在 Windows 上通过 PowerShell 执行 hook, cmd/findstr 的管道 stdin 会损坏 JSON,
    watchdog 侧全部事件被 drop 为"非JSON事件丢弃"。Python 直接 sys.stdin.buffer.read() 绕过问题。
    
    格式: Trae 原生 hooks.json 的 command 是**完整 shell 命令字符串**(无 args 数组),
    必须符合 Trae 文档规范。
    
    注意：Trae 侧还需要在 UI 里为本工作区启用 Hooks（安全开关）。"""
    path = Path.home() / ".trae-cn" / "hooks.json"
    if remove:
        if path.exists():
            path.write_text("{}", encoding="utf-8")
            print(f"  Trae hooks 已清空 -> {path}")
        return
    trae_hook = str(HOOK_DIR / "trae_hook.py")
    events = {
        "SessionStart": "session-start",
        "UserPromptSubmit": "prompt-submit",
        "PreToolUse": "pre-tool-use",
        "PostToolUse": "post-tool-use",
        "Notification": "notification",
        "Stop": "stop",
        "SessionEnd": "session-end",
    }
    cfg = {"version": 1, "hooks": {}}
    for pascal, kebab in events.items():
        # Trae 原生格式: command 是完整 shell 命令字符串, 不是 args 数组
        full_cmd = f'python -X utf8 "{trae_hook}" {kebab} trae'
        cfg["hooks"][pascal] = [
            {"matcher": "", "hooks": [
                {"type": "command", "command": full_cmd}
            ]}
        ]
    save_json(path, cfg, "Trae hooks 安装")
    print("  !! 记得在 Trae 设置里为本工作区启用 Hooks 开关")


def install_zcode_plugin_hooks():
    """#086/#087评审修复: 生成 ZCode 插件的 hooks/ai-status-hooks/hooks/hooks.json。
    命令里必须引用外层 wrapper 的**绝对路径**，而它随 clone 位置变——
    之前手写死本机路径，换机器/挪仓库后插件加载正常但每个 hook 静默失败。
    现在由安装器按实际路径生成（ZCODE_EVENTS 就是事件映射的单一事实来源），
    换机器重跑 `python hook/install_hooks.py` 即自动修正。"""
    plugin_hooks = (HOOK_DIR.parent / "plugins" / "ai-status-hooks"
                    / "hooks" / "hooks.json")
    plugin_hooks.parent.mkdir(parents=True, exist_ok=True)
    outer = HOOK_DIR.parent.parent / "hook" / "ai_status_hook.cmd"
    hooks = {}
    for event, arg in ZCODE_EVENTS.items():
        cmd = f'cmd /c "{outer}" {arg} zcode'
        hooks[event] = [{"hooks": [{"type": "command", "command": cmd,
                                    "timeout": 15}]}]
    # ensure_ascii=True: 路径含非 ASCII 用户名时也保持文件为纯 ASCII
    plugin_hooks.write_text(json.dumps({"hooks": hooks}, indent=2) + "\n",
                            encoding="ascii", newline="\n")
    print(f"ZCode 插件 hooks 已生成 -> {plugin_hooks}")


def main() -> int:
    remove = "--remove" in sys.argv
    if not remove:
        WRAPPER.write_text(WRAPPER_TEMPLATE, encoding="ascii", newline="")
        alt = HOOK_DIR.parent.parent / "hook" / "ai_status_hook.cmd"  # ZCode插件引用根目录(#021)
        alt.parent.mkdir(parents=True, exist_ok=True)
        alt.write_text(WRAPPER_TEMPLATE, encoding="ascii", newline="")
        print(f"包装脚本已双写: {WRAPPER} + {alt}")
        install_zcode_plugin_hooks()
        print("安装到 Claude Code 和 ZCode:")
    install_claude(remove)
    install_zcode(remove)
    install_trae(remove)
    if not remove:
        self_check()
    return 0


if __name__ == "__main__":
    sys.exit(main())
