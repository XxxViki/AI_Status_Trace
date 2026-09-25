/*
 * AI Status · 阶段 2：WiFi + HTTP 服务器驱动红绿灯
 *
 * main.c 现在只做"编排"：初始化各模块、启动服务。
 * 业务逻辑分散在：
 *   ai_state.c    状态机（事件→灯状态映射，全项目核心）
 *   app_wifi.c    WiFi STA 连接 + 自动重连
 *   app_http.c    HTTP 服务器（POST /events 接收 HookEvent）
 *   app_display.c 显示任务（队列消费，唯一碰 LCD 的任务）
 *
 * 数据流：curl/hook → HTTP任务 → 解析JSON → 队列 → 显示任务 → LCD
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "io_extension.h"
#include "ST7789.h"
#include "app_wifi.h"
#include "app_http.h"
#include "app_display.h"
#include "ai_sessions.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "=== AI Status 阶段 2：WiFi + HTTP ===");

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "芯片 rev%d, 可用堆 %u bytes",
             chip.revision, (unsigned)esp_get_free_heap_size());

    /* 板级初始化顺序：I2C IO 扩展（背光/CS/RST）→ SPI → ST7789 面板 */
    ESP_ERROR_CHECK(IO_EXTENSION_Init());
    ESP_ERROR_CHECK(LCD_Init());

    /* WiFi 初始化内部已完成 nvs_flash_init，会话表随后从 NVS 恢复 */
    ai_sessions_init();

    /* 显示任务先起，联网过程中屏幕就有东西看 */
    app_display_init();

    /* WiFi：最多等 15 秒拿 IP；超时也继续（后台自动重连） */
    app_wifi_start(15000);
    ai_sessions_load();   /* 恢复重启前的会话表（活会话不再被烧录误杀 #023） */

    /* HTTP 服务：端口 80，端点 /health /state /events */
    ESP_ERROR_CHECK(app_http_start());

    ESP_LOGI(TAG, "启动完成。测试：curl http://<上面打印的IP>/health");

    /* main 任务退出后由空闲任务回收——各业务任务继续跑 */
}
