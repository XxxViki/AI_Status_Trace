/*
 * app_wifi.c —— WiFi STA 连接（阶段 2）
 *
 * 学习点：
 * 1. ESP-IDF 事件驱动模型：esp_event。WiFi 的连接/断开不是函数返回值，
 *    而是异步事件（WIFI_EVENT / IP_EVENT），必须注册回调处理。
 * 2. 经典样板：netif 初始化 → 创建默认 STA → 配置 → start → 等事件。
 * 3. 断线自动重连：在 DISCONNECTED 事件回调里再调 esp_wifi_connect()。
 *    事件回调运行在 esp_event 任务栈里，不能做耗时操作。
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "app_wifi.h"
#include "wifi_config.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_events;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    /* 只处理 WIFI_EVENT；IP_EVENT 在下面单独注册 */
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* 这里没有延迟直接重连。若路由器长时间不可用，重连频率约每秒一次，
         * 对桌面设备可接受；真产品会做指数退避。 */
        ESP_LOGW(TAG, "断线，自动重连...");
        esp_wifi_connect();
        if (s_wifi_events) {
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "已连接! IP: " IPSTR " (网关 " IPSTR ")",
             IP2STR(&evt->ip_info.ip), IP2STR(&evt->ip_info.gw));
    if (s_wifi_events) {
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

/* #066 串口配网任务: 读 USB 控制台输入, 处理 SETWIFI 命令。
 * 用法: 在 idf.py monitor 里直接输入 SETWIFI 新SSID 新密码 回车 → 自动重启连新网。
 * UART0 就是 USB 口, 与日志共用——日志输出不影响行读取(行以
结束)。 */
static void serial_cmd_task(void *arg)
{
    char line[128];
    int n = 0;
    for (;;) {
        int c = getchar();
        if (c < 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (n > 0) {
                line[n] = '\0';
                if (wifi_config_handle_serial(line)) {
                    ESP_LOGI(TAG, "WiFi 配置已更新, 3 秒后重启...");
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    esp_restart();
                }
                n = 0;
            }
            continue;
        }
        if (n < (int)sizeof(line) - 1) {
            line[n++] = (char)c;
        }
    }
}

esp_err_t app_wifi_start(int timeout_ms)
{
    /* WiFi 驱动依赖 NVS（存校准数据），先初始化；已初始化时该调用返回错误码可忽略 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());            /* TCP/IP 协议栈 */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();          /* 把 WiFi 绑到协议栈上 */

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_wifi_events = xEventGroupCreate();

    /* 注册三个关键事件：STA启动(发起连接)、断线(重连)、拿到IP(置位) */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL));

    /* 凭据来源双层（#066）：NVS 用户配置优先, 回退 menuconfig 编译默认。
     * 换网 = 串口发 SETWIFI 命令(见 serial_cmd 任务), 不再需要烧固件。 */
    wifi_cfg_t user = {0};
    bool have_nvs = wifi_config_load(&user);

    /* AP 锁定（#034）：本环境有两个同名 SSID 的 AP（双频/双路由）。
     * BSSID 只在"使用编译默认凭据"时锁定——用户换网后 BSSID 属于新路由器。 */
    static const uint8_t k_ap_bssid[6] = { 0x5c, 0xde, 0x34, 0xcd, 0x68, 0xae };

    wifi_config_t wifi_cfg = {0};
    if (have_nvs) {
        strlcpy((char *)wifi_cfg.sta.ssid, user.ssid, sizeof(wifi_cfg.sta.ssid));
        strlcpy((char *)wifi_cfg.sta.password, user.pass, sizeof(wifi_cfg.sta.password));
        ESP_LOGI(TAG, "凭据来源: NVS 用户配置 (ssid=\"%s\")", user.ssid);
    } else {
        strlcpy((char *)wifi_cfg.sta.ssid, CONFIG_AI_STATUS_WIFI_SSID,
                sizeof(wifi_cfg.sta.ssid));
        strlcpy((char *)wifi_cfg.sta.password, CONFIG_AI_STATUS_WIFI_PASSWORD,
                sizeof(wifi_cfg.sta.password));
        memcpy(wifi_cfg.sta.bssid, k_ap_bssid, 6);
        wifi_cfg.sta.bssid_set = true;
        ESP_LOGI(TAG, "凭据来源: menuconfig 编译默认 (ssid=\"%s\")",
                 CONFIG_AI_STATUS_WIFI_SSID);
    }
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;
    ESP_LOGW(TAG, "锁定 AP: %02X:%02X:%02X:%02X:%02X:%02X",
             k_ap_bssid[0], k_ap_bssid[1], k_ap_bssid[2],
             k_ap_bssid[3], k_ap_bssid[4], k_ap_bssid[5]);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* A/B 实验结论（问题记录 #010）：这颗 C3 + 当前路由器组合下，
     * WIFI_PS_NONE 反而导致 TCP 握手间歇性卡 3 秒（链路层重传）；
     * 默认的 MIN_MODEM 省电模式反而稳定。唤醒延迟由"curl 直连(50ms启动)"
     * 方案吸收，足够快。 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    /* 强制 20MHz 频宽：路由器默认协商到 40MHz(HT40)，在干扰严重的 2.4G
     * 环境里 HT40 极易丢包重传（表现为 TCP 握手随机卡秒级）；
     * HT20 吞吐低一半但稳定，本应用每次只传几百字节。
     * 注：IDF 6.x 里枚举名从 WIFI_BW_HT20 改成了 WIFI_BW20 */
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20));

    ESP_LOGI(TAG, "正在连接 \"%s\" ...",
             have_nvs ? user.ssid : CONFIG_AI_STATUS_WIFI_SSID);

    /* #066 串口配网: USB 控制台监听 SETWIFI 命令(idf.py monitor 的输入即走这里) */
    xTaskCreate(serial_cmd_task, "sercmd", 4096, NULL, 2, NULL);

    /* 阻塞等 IP（只是让 main 的启动日志有明确顺序，业务上不必等） */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "%d ms 内未拿到 IP，继续后台重连", timeout_ms);
    return ESP_ERR_TIMEOUT;
}
