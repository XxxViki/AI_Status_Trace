/*
 * app_wifi.c —— WiFi 连接管理（STA 配网双层 + SoftAP 网页配网 + 串口配网）
 *
 * 架构（#066/#067）：
 *   凭据双层: NVS 用户配置优先 → 无则回退 menuconfig 编译默认（开箱即用）
 *   串口配网: USB 控制台输入 SETWIFI <ssid> <pass> → 存 NVS → 重启
 *   SoftAP 配网: 长按 BOOT 3 秒写标志重启 → 启动时进 AP 模式
 *                → 手机连 "AI-Status-Setup" 热点 → 浏览器 192.168.4.1 配置
 *   BSSID 锁定(#034): 仅在使用编译默认凭据时启用（用户换网后 BSSID 属新路由器）
 *
 * 本文件由 Write 工具整体重写（#065B 教训: python heredoc 已损坏此文件 8 次）。
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "app_wifi.h"
#include "wifi_config.h"
#include "app_display.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0

#define SETUP_AP_SSID "AI-Status-Setup"
#define SETUP_AP_PASS "12345678"

static EventGroupHandle_t s_wifi_events;
static bool s_wifi_setup_mode = false;

bool app_wifi_in_setup_mode(void) { return s_wifi_setup_mode; }

/* ---------- 配网模式标志（NVS, 跨重启传递） ---------- */

static bool setup_flag_get(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    esp_err_t e = nvs_open("wifi_cfg", NVS_READONLY, &h);
    if (e == ESP_OK) {
        nvs_get_u8(h, "setup_mode", &v);
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "setup_flag_get: nvs_open err=%s", esp_err_to_name(e));
    }
    ESP_LOGI(TAG, "setup_flag_get: v=%d", v);
    return v == 1;
}

static void setup_flag_set(bool on)
{
    nvs_handle_t h;
    if (nvs_open("wifi_cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "setup_mode", on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

void app_wifi_setup_flag_set(void)
{
    setup_flag_set(true);
}

/* ---------- WiFi 事件 ---------- */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* 无延迟直接重连。若路由器长时间不可用, 重连约每秒一次,
         * 对桌面设备可接受; 真产品会做指数退避。 */
        ESP_LOGW(TAG, "断线, 自动重连...");
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

/* ---------- SoftAP 网页配网（#067） ---------- */

static esp_err_t setup_page_get(httpd_req_t *req)
{
    const char *page =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>AI Status Config</title></head>"
        "<body style='font-family:sans-serif;max-width:400px;margin:40px auto'>"
        "<h2>AI Status WiFi Setup</h2>"
        "<form method='POST' action='/save'>"
        "<p>WiFi Name<br><input name='ssid' required style='width:100%;padding:8px'></p>"
        "<p>Password<br><input name='pass' type='password' required style='width:100%;padding:8px'></p>"
        "<p><button style='padding:10px 24px'>Connect</button></p>"
        "</form></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

/* URL 解码（+ → 空格, %XX → 字节），src..end 区间 → dst */
static void url_decode(const char *src, const char *end, char *dst, int cap)
{
    int n = 0;
    while (src < end && *src && n < cap) {
        if (*src == '+') {
            dst[n++] = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[n++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            dst[n++] = *src++;
        }
    }
    dst[n] = '\0';
}

static esp_err_t setup_save_post(httpd_req_t *req)
{
    char body[256] = { 0 };
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty");
        return ESP_FAIL;
    }
    body[len] = '\0';

    char ssid[WIFI_CFG_SSID_MAX] = { 0 };
    char pass[WIFI_CFG_PASS_MAX] = { 0 };
    char *p_ssid = strstr(body, "ssid=");
    char *p_pass = strstr(body, "pass=");
    if (p_ssid) {
        char *end = strstr(p_ssid, "&");
        url_decode(p_ssid + 5, end ? end : p_ssid + strlen(p_ssid),
                   ssid, sizeof(ssid));
    }
    if (p_pass) {
        char *end = strstr(p_pass, "&");
        url_decode(p_pass + 5, end ? end : p_pass + strlen(p_pass),
                   pass, sizeof(pass));
    }

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    wifi_config_save(ssid, pass);
    app_display_show_saved();       /* #069: 屏幕显示 SAVED! */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req,
        "<meta charset='utf-8'><body style='font-family:sans-serif'>"
        "<h2>Saved!</h2><p>Device will restart and connect.</p>",
        HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

/* ---------- 配网模式串口命令（#075）----------
 * EXITSETUP: 直接重启回正常模式（标志在进入配网时已清，重启即退出；
 *   没有这条命令前，串口在配网模式下又聋又哑，只能靠手机或断电）
 * SETWIFI <ssid> <pass>: 串口配网（与正常模式同一条命令） */
static void setup_serial_task(void *arg)
{
    char line[128];
    int n = 0;
    printf("\r\nsetup> EXITSETUP | SETWIFI <ssid> <pass>\r\n");
    for (;;) {
        int c = getchar();
        if (c < 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (n > 0) {
                line[n] = '\0';
                if (strncmp(line, "EXITSETUP", 9) == 0) {
                    ESP_LOGW(TAG, "串口退出配网，重启回正常模式");
                    vTaskDelay(pdMS_TO_TICKS(200));
                    esp_restart();
                }
                if (wifi_config_handle_serial(line)) {
                    ESP_LOGI(TAG, "WiFi config updated, restart in 1s...");
                    vTaskDelay(pdMS_TO_TICKS(1000));
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

void app_wifi_start_setup_mode(void)
{
    setup_flag_set(false);       /* 清标志, 防止重启死循环 */
    s_wifi_setup_mode = true;
    ESP_LOGW(TAG, "=== Setup Mode: AP=%s pass=%s, browser http://192.168.4.1 ===",
             SETUP_AP_SSID, SETUP_AP_PASS);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = SETUP_AP_SSID,
            .ssid_len = strlen(SETUP_AP_SSID),
            .password = SETUP_AP_PASS,
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_handle_t server = NULL;
    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&server, &hcfg) == ESP_OK) {
        static const httpd_uri_t page = { .uri = "/", .method = HTTP_GET, .handler = setup_page_get };
        static const httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = setup_save_post };
        httpd_register_uri_handler(server, &page);
        httpd_register_uri_handler(server, &save);
    }
    ESP_LOGW(TAG, "Setup HTTP ready (192.168.4.1)");
    /* #069: 屏幕显示配网状态（AP 信息 + IP） */
    app_display_enter_setup();
    xTaskCreate(setup_serial_task, "setup_ser", 4096, NULL, 2, NULL);   /* #075 */

    for (;;) {                    /* 配网模式不返回——专职服务配置页 */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---------- 串口配网（#066） ---------- */

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
                    ESP_LOGI(TAG, "WiFi config updated, restart in 3s...");
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

/* ---------- 启动 ---------- */

esp_err_t app_wifi_start(int timeout_ms)
{
    /* NVS 初始化必须在 setup_flag_get 之前——否则标志读不到, 配网模式永远不触发(#068) */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* 配网标志位: 长按 BOOT 设置 → 本次启动直接进 SoftAP 配网 */
    if (setup_flag_get()) {
        ESP_LOGW(TAG, "Setup flag set: entering SoftAP setup mode");
        app_wifi_start_setup_mode();
        return ESP_OK;   /* 不可达 */
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL));

    /* 凭据双层（#066）: NVS 用户配置优先, 回退 menuconfig 编译默认 */
    wifi_cfg_t user = { 0 };
    bool have_nvs = wifi_config_load(&user);

    /* BSSID 锁定(#034): 仅在使用编译默认凭据时启用——
     * 用户换网后 BSSID 属于新路由器, 继续锁定旧 BSSID 会连不上 */
    static const uint8_t k_ap_bssid[6] = { 0x5c, 0xde, 0x34, 0xcd, 0x68, 0xae };

    wifi_config_t wifi_cfg = { 0 };
    if (have_nvs) {
        strlcpy((char *)wifi_cfg.sta.ssid, user.ssid, sizeof(wifi_cfg.sta.ssid));
        strlcpy((char *)wifi_cfg.sta.password, user.pass, sizeof(wifi_cfg.sta.password));
        /* #076: 用户凭据也可选锁 BSSID——同名多 AP 且其中一台跑独立 NAT 时,
         * 按信号自动选网会连上孤岛网段(串口 SETBSSID 设置/清除) */
        if (user.bssid_lock) {
            memcpy(wifi_cfg.sta.bssid, user.bssid, 6);
            wifi_cfg.sta.bssid_set = true;
            ESP_LOGI(TAG, "Credentials: NVS (ssid=\"%s\" bssid locked)", user.ssid);
        } else {
            ESP_LOGI(TAG, "Credentials: NVS (ssid=\"%s\")", user.ssid);
        }
    } else {
        strlcpy((char *)wifi_cfg.sta.ssid, CONFIG_AI_STATUS_WIFI_SSID,
                sizeof(wifi_cfg.sta.ssid));
        strlcpy((char *)wifi_cfg.sta.password, CONFIG_AI_STATUS_WIFI_PASSWORD,
                sizeof(wifi_cfg.sta.password));
        memcpy(wifi_cfg.sta.bssid, k_ap_bssid, 6);
        wifi_cfg.sta.bssid_set = true;
        ESP_LOGI(TAG, "Credentials: compiled default (ssid=\"%s\")",
                 CONFIG_AI_STATUS_WIFI_SSID);
    }
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 省电模式(#010): MIN_MODEM 在本环境比 NONE 稳定 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    /* HT20 频宽(#010): 2.4G 的 HT40 在干扰环境丢包重传 */
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20));

    /* 串口配网命令监听（#066） */
    xTaskCreate(serial_cmd_task, "sercmd", 4096, NULL, 2, NULL);

    ESP_LOGI(TAG, "Connecting to \"%s\" ...", wifi_cfg.sta.ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "%d ms timeout waiting IP, reconnecting in background", timeout_ms);
    return ESP_ERR_TIMEOUT;
}
