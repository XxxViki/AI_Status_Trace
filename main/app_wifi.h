#pragma once
#include "esp_err.h"

/* 启动 WiFi STA 并等待拿到 IP（阻塞，最长 timeout_ms）。
 * 返回 ESP_OK 表示已联网；超时不算致命错误（后台会自动重连）。 */
esp_err_t app_wifi_start(int timeout_ms);
