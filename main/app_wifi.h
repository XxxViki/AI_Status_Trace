#pragma once
#include "esp_err.h"

/* 启动 WiFi STA 并等待拿到 IP（阻塞，最长 timeout_ms）。
 * 返回 ESP_OK 表示已联网；超时不算致命错误（后台会自动重连）。 */
esp_err_t app_wifi_start(int timeout_ms);

/* SoftAP 网页配网模式(#067)：不返回, 专职服务配置页 */
void app_wifi_start_setup_mode(void);
void app_wifi_setup_flag_set(void);   /* 长按BOOT触发: 写标志+重启 */
bool app_wifi_in_setup_mode(void);
