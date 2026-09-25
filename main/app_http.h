#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

/* 启动 HTTP 服务（端口 80），注册 /events 和调试用端点 */
esp_err_t app_http_start(void);
