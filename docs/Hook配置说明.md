# Hook 配置说明（AI 工具 → ESP32 红绿灯）

> 面向场景：换电脑、换 AI 工具、板子 IP 变了、hook 突然不生效。
> 涉及两个 AI 工具：**Claude Code** 和 **ZCode**。

---

## 1. 它怎么工作的

```
  AI 工具（Claude Code / ZCode）
        │  生命周期事件
        ▼
  ai_status_hook.cmd          ← 只往本地文件追加一行，约 30ms，不碰网络
        │
        ▼
  ~/.ai_status/queue/*.ev     ← 本地队列
        │
        ▼
  esp_light_watchdog.py       ← 常驻进程，1s 巡检，带重试地转发
        │  HTTP POST /events
        ▼
  板子 ESP32-C3               ← 状态机 → LCD 卡片
```

**为什么绕一层队列**（问题记录 #030）：早期版本是 hook 里直接 `curl` 板子。网络在 AI 的关键路径上，路由器抖一下，AI 的每一次工具调用都要陪等 2 秒。改成"hook 只写本地文件、常驻进程负责转发"后，**网络彻底移出关键路径**，AI 侧的开销从秒级降到约 30ms。

副作用：hook 本身不再需要知道板子地址，**板子地址只由看门狗持有**（见第 3 节）。

---

## 2. 快速开始

```bash
cd D:/Xxx/Project/Esp32/AI_Light/AI_Status

# 1) 装 hook（Claude Code + 写 ZCode 插件用的包装脚本）
python hook/install_hooks.py

# 2) 启动看门狗（常驻，负责转发）
pythonw hook/esp_light_watchdog.py

# 3) 验证
curl http://192.168.1.20/health
```

| 操作 | 命令 |
|---|---|
| 安装 / 更新 | `python hook/install_hooks.py` |
| 卸载 | `python hook/install_hooks.py --remove` |
| 重装到干净状态 | 先 `--remove` 再装 |

安装器会自动备份被改的配置文件到 `<文件名>.bak-<时间戳>`，不会覆盖你原有的其它 hook。

---

## 3. 板子 IP：唯一的真源

> ⚠️ **板子地址只在 `hook/esp_light_watchdog.py:26` 的 `BOARD` 生效。**
>
> `hook/install_hooks.py:30` 里也有个 `BOARD_URL`，那是 v4 时代的遗留常量，**v5 起已完全不被引用**。
> README 里"板子 IP 改动：改 install_hooks.py 顶部 BOARD_URL 后重跑安装器"这句是**过期的**，
> 照它改不会生效。

换 IP 的正确做法：

```python
# hook/esp_light_watchdog.py:26
BOARD = "http://192.168.1.20"     # ← 改这里
```

改完**重启看门狗**（改安装器不重启看门狗是没用的）。

**长期建议**：路由器侧给板子做 **DHCP 保留**（绑定 MAC 固定 IP）。否则路由器重启后 IP 可能变，看门狗会静默转发失败。

---

## 4. Claude Code 配置

### 4.1 装到哪里

`~/.claude/settings.json` 的 `hooks.<EventName>`，**command 型**。

### 4.2 事件映射

| Claude Code 原生事件 | 转发为 | 说明 |
|---|---|---|
| `SessionStart` | `session-start` | 新会话，建卡 |
| `UserPromptSubmit` | `prompt-submit` | 用户发话，转 WORKING |
| `PreToolUse` | `pre-tool-use` | 工具调用前 |
| `PostToolUse` | `post-tool-use` | 工具调用后 |
| `Notification` | `notification` | **Claude 用它承载权限请求**，红灯 |
| `Stop` | `stop` | 回合结束 → DONE 绿 |
| `SessionEnd` | `session-end` | 会话结束，清卡 |

### 4.3 `Stop` 是特例

其余 6 个事件都走 `ai_status_hook.cmd` 排队；**只有 `Stop` 走专用脚本 `claude_token_hook.py`** —— 它除了转发 `stop`，还会读 Claude 的转录文件（transcript）把 token 用量一并推给板子，用于卡片上显示 token 数。

每轮只跑一次，约 300ms 开销，可接受。

### 4.4 装完之后长什么样

```json
{
  "hooks": {
    "PreToolUse": [
      {
        "matcher": "",
        "hooks": [
          {
            "type": "command",
            "command": "cmd",
            "args": ["/c", "D:\\Xxx\\...\\AI_Status\\hook\\ai_status_hook.cmd", "pre-tool-use", "claude"]
          }
        ]
      }
    ],
    "Stop": [
      {
        "matcher": "",
        "hooks": [
          {
            "type": "command",
            "command": "python",
            "args": ["-X", "utf8", "D:\\Xxx\\...\\AI_Status\\hook\\claude_token_hook.py"]
          }
        ]
      }
    ]
  }
}
```

安装器用**标记识别**自己装过的条目（`HOOK_MARKERS`），每次安装先清掉自己的旧条目再追加，所以**重复运行是安全的**，也不会动你原有的其它 hook。

---

## 5. ZCode 配置

> ⚠️ **ZCode 不走 `config.json`，走插件。**
>
> v4 时代 ZCode 的 hook 写在 `~/.zcode/cli/config.json` 的 `hooks.events.<Event>`（process 型）。
> v5 起改为**插件是唯一事件通道**，`install_hooks.py` 对 config.json 只做一件事：**清理旧版残留**
> （`install_zcode()` 不安装任何东西）。留着旧条目会造成双重触发（问题记录 #022）。

### 5.1 插件位置

```
D:\Xxx\Project\Esp32\AI_Light\plugins\
├── marketplace.json                     # 本地集市清单
└── ai-status-hooks/
    ├── .zcode-plugin/plugin.json        # 插件元信息，指向 hooks 文件
    └── hooks/hooks.json                 # 事件 → 命令 的映射
```

`plugin.json`：

```json
{
  "name": "ai-status-hooks",
  "version": "0.1.3",
  "hooks": "./hooks/hooks.json"
}
```

### 5.2 事件映射

比 Claude 多两个、少两个：

| ZCode 原生事件 | 转发为 | 说明 |
|---|---|---|
| `SessionStart` | `session-start` | |
| `UserPromptSubmit` | `prompt-submit` | |
| `PreToolUse` | `pre-tool-use` | |
| `PostToolUse` | `post-tool-use` | |
| `PostToolUseFailure` | `post-tool-use` | 工具失败但 AI 仍在处理，仍是"工作中" |
| `PermissionRequest` | `permission-request` | **ZCode 有原生权限事件**，直接红灯（Claude 要靠 Notification 承载） |
| `Stop` | `stop` | |
| ~~`SessionEnd`~~ | — | **ZCode 没有** |
| ~~`Notification`~~ | — | **ZCode 没有** |

### 5.3 hooks.json 的样子

```json
{
  "hooks": {
    "PreToolUse": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "cmd /c \"D:\\Xxx\\Project\\Esp32\\AI_Light\\hook\\ai_status_hook.cmd\" pre-tool-use zcode",
            "timeout": 15
          }
        ]
      }
    ]
  }
}
```

注意这里的命令是**绝对路径**，而且指向的是 **`AI_Light\hook\`（外层目录），不是 `AI_Status\hook\`** —— 见第 9 节的陷阱。

### 5.4 改完插件要重启 ZCode

运行中的 ZCode 内存里挂着旧配置，**删文件 / 改文件都不等于生效**。改完插件必须**重启 ZCode** 才会重新载入 hooks。

---

## 6. 队列与看门狗

### 6.1 队列

- 目录：`~/.ai_status/queue/`
- 文件名：`<event>_<tool>_<随机数>.ev`（如 `pre-tool-use_claude_48312.ev`）
- 写入者：`ai_status_hook.cmd`（AI 侧，约 30ms）
- 消费/删除者：看门狗（转发成功即删）

队列积压说明看门狗没在跑，或板子不可达。

### 6.2 看门狗的四项职责

常驻进程，`pythonw hook/esp_light_watchdog.py`，主循环 1 秒一轮：

| # | 职责 | 周期 | 干什么 |
|---|---|---|---|
| 1 | 队列转发 | 1s | 把 `queue/*.ev` POST 到板子，失败保持重试 |
| 2 | ZCode 日志尾随 | 1s | 读 ZCode 日志里的审批结果 → 推 WORKING（**批准加速**，约 1 秒回卡） |
| 3 | 进程巡检 | 3s | 进程被杀不产生事件，由它补齐：Claude 进程归零 → 清所有卡；ZCode 退出 → 清卡；有转录但无卡 → 重建 |
| 4 | token 监视 | 1s | 读 Claude 转录的 usage → `POST /sessions/tok` |

### 6.3 日志与启停

| 文件 | 内容 |
|---|---|
| `~/.ai_status/watchdog.log` | 看门狗主日志 |
| `~/.ai_status/hook_exec.log` | 每个 hook 的触发记录（AI 侧写的） |
| `~/.ai_status/tok_hook.log` | token 相关 |

- 自启动：Windows 启动文件夹里有快捷方式（删掉即可关闭）
- 手动启动：`pythonw hook/esp_light_watchdog.py`
- 手动停止：任务管理器结束 `pythonw.exe`

---

## 7. 验证装好了

按顺序查，卡在哪一步就是哪一环断了：

```bash
# ① 板子在线？
curl http://192.168.1.20/health              # 期望 "ok"

# ② 看门狗在跑？
tail -5 ~/.ai_status/watchdog.log

# ③ 让 AI 随便干点什么，然后看 hook 有没有触发
tail -5 ~/.ai_status/hook_exec.log           # 期望出现 ev=pre-tool-use src=claude 之类

# ④ 队列有没有被消费？（应很快清空）
ls ~/.ai_status/queue/

# ⑤ 板子上有没有卡？
curl http://192.168.1.20/state
```

**⑤ 的对照**：表头显示的数字应与 `/state` 的 `sessions` 一致。数字对不上说明有心跳丢失的卡还没被清。

---

## 8. 排错速查

| 症状 | 可能原因 | 处理 |
|---|---|---|
| AI 侧明显卡顿 | 又在 hook 里直连板子（旧版包装脚本） | 重跑安装器，确认 `ai_status_hook.cmd` 是 v5 版本（内容含 `queue`） |
| `hook_exec.log` 无记录 | hook 没装上 / 工具没重启 | 重跑安装器；**ZCode 必须重启** |
| 有记录但板子不动 | 看门狗没跑 / IP 错 | 起看门狗；核对 `esp_light_watchdog.py:26` 的 `BOARD` |
| 队列文件持续积压 | 板子不可达 | `curl /health`；查 IP 是否被 DHCP 换掉 |
| 卡片卡在红灯不恢复 | 批准事件没被识别 | 查 ZCode 日志尾随是否正常（职责 2） |
| 板子上有重复的卡 | ZCode 双重触发 | config.json 里还有旧 hook 残留，重跑安装器清理 |
| 重启电脑后就不工作了 | 看门狗没自启动 | 检查启动文件夹快捷方式 |
| 事件发了但状态不对 | session_id 太长被截断 | id 上限 **12 字符**，tool 上限 **8**，project 上限 **16** |

---

## 9. 已知陷阱

### 9.1 有两份 `install_hooks.py`，别跑错

| 路径 | 版本 | 行为 |
|---|---|---|
| `AI_Status/hook/install_hooks.py` | **v5（当前）** | 队列模式；ZCode 侧只**清理** config.json |
| `AI_Light/hook/install_hooks.py` | v4（遗留） | **curl 直连板子**；ZCode 侧往 config.json **写** process 型 hook |

跑错旧的那份会把包装脚本覆盖回 v4 的 curl 版本，把网络重新拉回 AI 关键路径。

### 9.2 包装脚本被"双写"

插件（`hooks.json`）引用的是 `AI_Light\hook\ai_status_hook.cmd`，但安装器本体在 `AI_Status\hook\`。所以 `install_hooks.py` 的 `main()` 会把同一份包装脚本**写到两个位置**（`AI_Status/hook/` 和 `AI_Light/hook/`）。

这是为了迁就插件的绝对路径（问题记录 #021）。**改了包装脚本必须重跑安装器**，否则两处不一致。

### 9.3 板子 IP 是 DHCP

见第 3 节。建议路由器侧做 DHCP 保留。

### 9.4 ZCode 的 `hooks.enabled`

ZCode 配置文件里 hooks 默认是**关闭**的。虽然 v5 起事件走插件通道，但如果排查时发现事件完全不触发，仍值得确认这个开关的状态。

### 9.5 中文路径 / 中文 .bat

包装脚本 `.cmd` 必须 **ASCII only**（问题记录 #004）。中文会导致编码问题。

---

## 10. 接入新的 AI 工具

协议层是工具无关的 —— **任何工具只要能发出下面这个请求，就能接入**：

```bash
curl -X POST "http://192.168.1.20/events?event_type=<事件>&src=<工具名>" \
     -H "Content-Type: application/json" \
     -d '{"session_id":"<会话id>","cwd":"<项目路径>","tool_name":"<可选>"}'
```

### 10.1 事件类型（板子认这 8 个）

| 事件 | 语义 | 灯 |
|---|---|---|
| `session-start` | 会话开始 | 建卡 / IDLE |
| `prompt-submit` | 用户发话 | WORKING（黄呼吸） |
| `pre-tool-use` | 工具调用前 | WORKING |
| `post-tool-use` | 工具调用后 | WORKING |
| `permission-request` | 请求授权 | ERROR（红闪） |
| `notification` | 通知 | ERROR（红闪） |
| `stop` | 回合结束 | DONE（绿闪） |
| `session-end` | 会话结束 | 清卡 |

### 10.2 参数

**查询参数**（URL 上）：

| 参数 | 说明 |
|---|---|
| `event_type` | 必填，上表 8 个之一 |
| `src` | 工具名（`claude` / `zcode` / …），决定卡片上的 logo 和品牌色 |
| `ts` | 时间缩放，仅测试用（`ts=30` 即所有超时阈值快 30 倍）。**正常使用不要带** |

**JSON body**：

| 字段 | 必填 | 说明 |
|---|---|---|
| `session_id` | 是 | **上限 12 字符**，超长会被截断 |
| `cwd` | 否 | 项目路径，卡片上显示为项目名（**上限 16 字符**） |
| `tool_name` | 否 | 最近调用的工具名（**上限 8 字符**） |

### 10.3 接入步骤

1. 在上面的事件表里找到新工具原生事件对应的语义
2. 写一个 hook/插件，把原生事件转成 `POST /events`
3. **别在 hook 里直接发网络请求** —— 按第 6 节的模式，只往 `~/.ai_status/queue/` 写文件
4. 未登记的工具会自动降级成"首字母徽章"（品牌色 + 首字母），**不需要改固件**就能显示

### 10.4 其它 HTTP 接口

| 方法 | 路径 | 用途 |
|---|---|---|
| GET | `/health` | 探活 |
| GET | `/state` | 当前所有卡的状态 JSON（含 `heap`） |
| POST | `/sessions/clear` | 清卡，`?tool=claude` 只清某个工具 |
| POST | `/sessions/tok` | 更新某会话的 token 用量，`?sid=&tok=` |
| GET | `/dbg/row?y=&step=` | 逐像素读某一行（调试显示用） |
| GET | `/dbg/page?p=` | 切页（调试用） |

---

## 附：文件清单

```
AI_Status/hook/
├── install_hooks.py           # 安装器（v5，跑这个）
├── ai_status_hook.cmd         # 包装脚本（安装器生成，勿手改）
├── claude_token_hook.py       # Claude Stop 专用：转发 stop + token 用量
├── esp_light_watchdog.py      # 常驻看门狗 ← 板子 IP 在这里
└── esp_light_hook.ps1         # v1 原型，已被取代，仅留存参考

AI_Light/plugins/ai-status-hooks/     # ZCode 插件
├── .zcode-plugin/plugin.json
└── hooks/hooks.json                  # 引用 AI_Light/hook/ai_status_hook.cmd

AI_Light/hook/                 # 插件引用的包装脚本副本（安装器双写）
```
