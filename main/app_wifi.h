#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* 启动 WiFi STA 并等待拿到 IP（阻塞，最长 timeout_ms）。
 * 返回 ESP_OK 表示已联网；超时不算致命错误（后台会自动重连）。 */
esp_err_t app_wifi_start(int timeout_ms);

/* SoftAP 网页配网模式(#067)：不返回, 专职服务配置页 */
void app_wifi_start_setup_mode(void);
void app_wifi_setup_flag_set(void);   /* 长按BOOT触发: 写标志+重启 */
bool app_wifi_in_setup_mode(void);

/* #085: 取当前 STA 的 IPv4 字符串（未连接/获取中返回 false）。
 * 换网段后板子 IP 会变，脚本端无从得知——屏显 IP 让人眼可读，
 * 不必再猜或扫网段（旧地址写死在各脚本里的教训）。 */
bool app_wifi_get_ip(char *out, size_t cap);
