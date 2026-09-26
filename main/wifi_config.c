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
    /* #076: 可选 BSSID 锁（blob 7B: [0]=1 表示启用, [1..6]=MAC） */
    uint8_t bb[7];
    size_t bl = sizeof(bb);
    if (nvs_get_blob(h, "sta_bssid", bb, &bl) == ESP_OK && bl == 7 && bb[0] == 1) {
        out->bssid_lock = true;
        memcpy(out->bssid, bb + 1, 6);
    }
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
    /* #076: SETBSSID <aa:bb:cc:dd:ee:ff> | SETBSSID off —— 同 SSID 多 AP 环境
     * 锁定主网桥接的那台 AP（本机实测: 同名 "Xxx" 4 个 AP, 最强的那台在跑
     * 独立 NAT, 板子按信号选网会连上 192.168.28.x 孤岛）。off 解锁恢复自动选。 */
    if (strncmp(line, "SETBSSID ", 9) == 0) {
        const char *rest = line + 9;
        nvs_handle_t h;
        ESP_ERROR_CHECK(nvs_open(NS, NVS_READWRITE, &h));
        if (strcmp(rest, "off") == 0) {
            nvs_erase_key(h, "sta_bssid");
            nvs_commit(h);
            nvs_close(h);
            ESP_LOGW(TAG, "BSSID 锁已清除（恢复自动选 AP）, 重启生效");
            return true;
        }
        uint8_t mac[7] = { 1, 0, 0, 0, 0, 0, 0 };
        if (sscanf(rest, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
                   &mac[1], &mac[2], &mac[3], &mac[4], &mac[5], &mac[6]) != 6) {
            nvs_close(h);
            ESP_LOGE(TAG, "格式错误, 用法: SETBSSID aa:bb:cc:dd:ee:ff | SETBSSID off");
            return true;    /* 是本模块命令但参数错: 消费掉, 不重启 */
        }
        nvs_set_blob(h, "sta_bssid", mac, sizeof(mac));
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "BSSID 锁已保存: %02X:%02X:%02X:%02X:%02X:%02X, 重启生效",
                 mac[1], mac[2], mac[3], mac[4], mac[5], mac[6]);
        return true;
    }

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
