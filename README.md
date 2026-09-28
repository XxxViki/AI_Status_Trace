# AI Status 固件工程

ESP32-C3-LCD-1.47 上的 AI 状态红绿灯固件。从零搭建，仅复用厂商例程的板级驱动。

## 目录结构（学习点：ESP-IDF 工程 anatomy）

```
AI_Status/
├── CMakeLists.txt        # 顶层：引入 IDF 构建系统，声明工程名
├── sdkconfig.defaults    # 默认配置：唯一事实来源（4MB flash / 分区表 / target），必须入库
├── sdkconfig             # 完整配置：生成物 + 明文 WiFi 密码 → 不入库，新机器自动重建
├── .gitignore            # 只提交"能从零重建工程"的核心文件（策略见文件内注释）
├── .gitattributes        # 行尾规范：.bat/.cmd 强制 CRLF，源码 LF
├── partitions.csv        # flash 分区表：nvs(存WiFi凭据) + factory(应用)
├── build.bat             # 一键构建（首次自动 set-target）
├── flash.bat             # 一键烧录：flash.bat COM9
├── idf.bat               # IDF 环境入口：idf.bat <任意 idf.py 参数>
├── components/
│   └── bsp/              # Board Support Package 板级支持包
│       ├── io_extension.c/h    # I2C IO 扩展芯片驱动（背光/LCD_CS/LCD_RST/SD_CS）
│       ├── ST7789.c/h          # LCD 总线+面板初始化（SPI2 80MHz）
│       └── Vernon_ST7789T/     # ST7789 "T" 变体面板初始化序列（厂商提供）
├── docs/                 # 学习路线 + 问题记录（踩坑日志）
├── hook/                 # 电脑侧脚本：事件 hook + 看门狗 + 一键安装器
└── main/
    ├── CMakeLists.txt
    └── main.c            # 应用入口 app_main()
```

## 常用命令（在项目根目录 AI_Status\ 下）

```bash
# 构建（首次会自动 set-target esp32c3）
cmd //c build.bat

# 或手动：
cmd //c "idf.bat set-target esp32c3"    # 只需一次
cmd //c "idf.bat build"

# 烧录（端口按设备管理器实际为准，默认 COM9）
cmd //c "flash.bat COM9"

# 烧录 + 看日志
cmd //c "idf.bat -p COM9 flash monitor"

# 退出 monitor：Ctrl+]
```

## 克隆后首次构建（换一台电脑）

```bash
git clone git@github.com:XxxViki/AI_Status_Trace.git
cd AI_Status
cmd //c build.bat
```

三件"不入库的东西"要留意：

1. **ESP-IDF 位置**：`idf.bat` 先读环境变量 `IDF_PATH`，没有才回退 `C:\esp\v6.1\esp-idf`。
   装在别处就 `set IDF_PATH=D:\esp\v6.1\esp-idf`，不用改脚本。
2. **WiFi 凭据**：`sdkconfig` 不入库，新机器由 `sdkconfig.defaults` 重新生成，
   flash 大小 / 分区表 / 芯片型号都已写在里面，不用手动 menuconfig；
   填真实 SSID/密码：`cmd //c "idf.bat menuconfig"` → AI Status Configuration。
3. **第三方组件**：`espressif/cjson` 首次构建自动下载（版本由 `dependencies.lock` 锁定）。
   下载慢或不通时用国内镜像，或把已有机器的 `managed_components\` 整个拷过来：

   ```bash
   set IDF_COMPONENT_STORAGE_URL=https://components-file.espressif.cn
   ```

## 硬件速查（ESP32-C3-LCD-1.47）

| 资源 | 参数 |
|---|---|
| 芯片 | ESP32-C3 (RISC-V 单核 160MHz, 400KB SRAM, 无 PSRAM) |
| LCD | ST7789T 172x320 IPS, SPI2 @80MHz |
| LCD 引脚 | SCLK=7 MOSI=5 MISO=6 DC=8 |
| 特殊设计 | LCD_CS/RST、SD_CS、背光 PWM 都在 I2C(0x24) IO 扩展芯片上，I2C: SCL=3 SDA=4 |
| Flash | 4MB |

## 模块结构（阶段 2 起）

```
main/
├── main.c         # 编排：初始化各模块、启动服务
├── ai_state.c/h   # 状态机：hook事件 → 灯状态（全项目核心）
├── app_wifi.c/h   # WiFi STA + 自动重连
├── app_http.c/h   # HTTP 服务：POST /events、GET /health、GET /state
├── app_display.c/h# 显示任务：队列消费，唯一碰 LCD 的任务
└── Kconfig.projbuild  # WiFi SSID/密码（menuconfig 可改）
```

## HTTP API

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | /health | 探活，返回 "ok" |
| GET | /state | 当前灯状态 JSON |
| POST | /events | 接收 ai-light 协议 HookEvent（event_type/session_id），切灯 |

测试（地址取自 `~/.ai_status/board_url`，见下节"板子地址"）：

```bash
BOARD=$(grep -v '^#' ~/.ai_status/board_url | grep -v '^$' | head -1)
curl -X POST $BOARD/events -d '{"event_type":"stop","session_id":"t1"}'
```

## 阶段状态

- [x] 阶段 1：LCD 红绿灯测试画面
- [x] 阶段 2：WiFi + HTTP 服务器（menuconfig 硬编码版；SoftAP 配网待升级）
- [ ] 阶段 3：Claude Code hooks 联动
- [ ] 阶段 4：LVGL 多会话 UI
- [ ] 阶段 5：ZCode 支持

## AI 工具 hook 对接（阶段 3 + 阶段 5）

电脑侧脚本在项目根 `hook/` 目录，**一个安装器同时支持 Claude Code 和 ZCode**：

```
hook/
├── ai_status_hook.cmd      # hook 包装（双档重试，永远 exit 0，ASCII only）——由安装器生成
├── board_addr.py           # 板子地址统一解析（#085：环境变量 > board_url 文件 > 默认）
├── claude_token_hook.py    # Claude Stop 专用：转发 stop 事件 + 附带 transcript token 用量
├── install_hooks.py        # 安装器：python install_hooks.py 装两个工具（--remove 卸载）
├── esp_light_watchdog.py   # 进程看门狗（见下节）
└── esp_light_hook.ps1      # v1 原型：已被 ai_status_hook.cmd 取代，仅留存参考

plugins/
└── ai-status-hooks/        # ZCode 事件插件（#086：hooks.json 7 事件 → wrapper 队列）
```

- Claude Code: `~/.claude/settings.json`，7 事件（Notification 承载权限请求）
- ZCode: **插件通道**（#086）——本仓库 `plugins/ai-status-hooks/`，经
  `~/.zcode/cli/config.json` 的 `plugins.dirs` 内联启用（默认启用，免集市安装；
  详见 Hook配置说明 §5）。改完插件/config 要重启 ZCode
- 已知限制：DHCP 换 IP 由失联自愈兜底（mDNS #087 已落地）；长期可再给路由器做
  MAC 绑定静态 IP，让地址永远不变

### 板子地址（#085/#087：唯一事实来源 + 失联自愈）

日常配置只有**一个文件** `~/.ai_status/board_url`（首个非注释行），watchdog 1~3 秒
自动生效。当前推荐直接写域名（板子固件注册了 mDNS）：

```
http://10.0.2.127         # 推荐：裸 IP。Windows 对 .local 每次请求都现场解析
                          # (~110ms/次, HTTP 676ms vs IP 103ms)——域名进热路径会
                          # 把事件排队延迟放大数倍(#089)。IP 变了自愈会自动追
# http://aistatus.local   # 备选：mDNS 可用且不在意延迟时（一次性脚本用没问题）
# http://192.168.1.20     # 家里
```

**换网络环境的完整流程（#087 后，全程不改代码不编辑配置）**：

1. 长按板子 BOOT 3 秒 → 重启进配网模式（屏幕显示热点名）
2. 手机连 `AI-Status-Setup`（密码 12345678）→ 浏览器 `192.168.4.1`
   → **SSID 点输入框直接选**（#087 起配网页带扫描列表，不用手敲）→ 输密码 → Connect
3. 板子重启入网。PC 侧**零操作**：看门狗失联 >15 秒自动找回（先试 mDNS 域名，
   不通再按 MAC 扫本机 /24），改写 board_url 后队列里积压的事件自动补发
   （仅 180 秒内积压的——更早的按 #030 设计丢弃，状态事件重放无意义）

手动找回（自愈不可达时，比如板子在别的网段）：`python tools/find_board.py --write`。
临时覆盖：环境变量 `AI_STATUS_BOARD`（设置后自愈停用，尊重显式指定）。

> 看门狗带单实例锁（`~/.ai_status/watchdog.pid`）——双开会抢队列互踩
> （FileNotFoundError + 双份补发）。发现日志刷这两个错就查有没有第二个实例。
> ⚠️ 从 #087 之前的版本升级时，**旧实例不写 pid 文件、锁拦不住它**，需手动结束一次
> （任务管理器搜 pythonw.exe），之后新实例自会接管。

## 进程看门狗（杀进程立即清屏 + 批准加速）

hook 是被动事件，进程被杀不产生事件——由主机侧看门狗补齐：

```
hook/esp_light_watchdog.py   # pythonw 常驻，3 秒巡检进程表
hook/. 启动方式: Startup 自启动快捷方式「AI Status Watchdog.lnk」（删除即关闭）
       （新建：把 `pythonw hook/esp_light_watchdog.py` 的快捷方式丢进 startup 文件夹）
```

- claude: 进程数归零 -> 清所有 claude 卡；进程数 < 卡数 -> 清最闲的
- zcode: 应用退出 -> 清所有 zcode 卡
- 防护: 只清安静 >=10 秒的卡；枚举失败时不动
- **批准加速**: 尾随 ZCode 日志的 tool.permission.resolved，批准后 ~1 秒卡片回 WORKING
  （ZCode 的 hook 事件里没有"批准"，日志里有）
- 日志: ~/.ai_status/watchdog.log
- 手动启动: `pythonw hook/esp_light_watchdog.py`
