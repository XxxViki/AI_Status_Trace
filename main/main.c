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
#include "nvs_flash.h"
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

    /* Q08: NVS 就地初始化（原先只在 app_wifi_start 里做），会话表恢复放到
     * 显示任务启动之前——显示任务起来后每 50ms tick/aggregate 读表，主任务
     * 再并发写表就是无锁竞态；提前 load 还让重启后卡片不用先等 WiFi 的 15 秒 */
    ai_sessions_init();
    {
        esp_err_t nvs_err = nvs_flash_init();
        if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            nvs_err = nvs_flash_init();
        }
        ESP_ERROR_CHECK(nvs_err);
    }
    ai_sessions_load();   /* 恢复重启前的会话表（活会话不再被烧录误杀 #023） */

    /* 显示任务先起，联网过程中屏幕就有东西看 */
    app_display_init();

    /* WiFi：最多等 15 秒拿 IP；超时也继续（后台自动重连） */
    app_wifi_start(15000);

    /* HTTP 服务：端口 80，端点 /health /state /events */
    ESP_ERROR_CHECK(app_http_start());

    ESP_LOGI(TAG, "启动完成。测试：curl http://<上面打印的IP>/health");

    /* main 任务退出后由空闲任务回收——各业务任务继续跑 */
}
