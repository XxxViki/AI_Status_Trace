/*
 * wifi_config.c —— WiFi 凭据 NVS 存取 + 串口配网命令（#066）
 *
 * 学习点：
 * 1. NVS 键值对 vs 会话表的 blob：凭据是两个短字符串，用独立键比 blob 简单
 * 2. 密码不打印日志（\* 掩码），串口回显只确认成功
 * 3. 编译期默认值(Kconfig)与 NVS 用户配置的双层设计：
 *    "开箱即用"由固件提供默认，"用户自定义"存 NVS 且优先
 */
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "wifi_config.h"

static const char *TAG = "wifi_cfg";
#define NS "wifi_cfg"

bool wifi_config_load(wifi_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t sl = sizeof(out->ssid), pl = sizeof(out->pass);
    esp_err_t e1 = nvs_get_str(h, "ssid", out->ssid, &sl);
    esp_err_t e2 = nvs_get_str(h, "pass", out->pass, &pl);
    nvs_close(h);
    if (e1 != ESP_OK || e2 != ESP_OK || out->ssid[0] == '\0') {
        return false;
    }
    out->from_nvs = true;
    return true;
}

void wifi_config_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NS, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_str(h, "ssid", ssid));
    ESP_ERROR_CHECK(nvs_set_str(h, "pass", pass));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "WiFi 配置已保存: ssid=\"%s\" pass=*(%d字符)", ssid, (int)strlen(pass));
}

bool wifi_config_handle_serial(const char *line)
{
    if (strncmp(line, "SETWIFI ", 8) != 0) {
        return false;
    }
    const char *rest = line + 8;
    char ssid[WIFI_CFG_SSID_MAX] = {0};
    char pass[WIFI_CFG_PASS_MAX] = {0};
    /* 格式: SETWIFI <ssid> <pass>  —— ssid 不含空格, pass 可含空格 */
    const char *sp = strchr(rest, ' ');
    if (sp == NULL || sp == rest || sp[1] == '\0') {
        ESP_LOGE(TAG, "格式错误, 用法: SETWIFI <ssid> <pass>");
        return true;    /* 是 SETWIFI 命令但参数错: 消费掉, 不重启 */
    }
    int sl = (int)(sp - rest);
    if (sl >= (int)sizeof(ssid)) {
        sl = sizeof(ssid) - 1;
    }
    memcpy(ssid, rest, sl);
    ssid[sl] = '\0';
    strlcpy(pass, sp + 1, sizeof(pass));

    wifi_config_save(ssid, pass);
    return true;    /* 调用方 esp_restart() */
}
