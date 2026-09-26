/*
 * wifi_config.h —— WiFi 凭据的 NVS 存取（#066 配网第 1 步）
 *
 * 优先级：NVS 里存了凭据 → 用它；没有 → 回退 menuconfig 编译期默认值。
 * 换网流程：串口发 SETWIFI ssid pass → 写 NVS → 重启 → 自动连新网。
 */
#pragma once

#include <stdint.h>

#define WIFI_CFG_SSID_MAX  33     /* 802.11 SSID 最长 32 字节 + NUL */
#define WIFI_CFG_PASS_MAX  65     /* WPA2 密码最长 64 字节 + NUL */

typedef struct {
    char ssid[WIFI_CFG_SSID_MAX];
    char pass[WIFI_CFG_PASS_MAX];
    bool from_nvs;      /* true=NVS 用户配置, false=menuconfig 编译默认 */
} wifi_cfg_t;

/* 读配置（须在 nvs_flash_init 之后调用）。返回 true 表示 NVS 里有用户配置 */
bool wifi_config_load(wifi_cfg_t *out);

/* 写配置到 NVS（立即提交） */
void wifi_config_save(const char *ssid, const char *pass);

/* 串口配网命令处理：解析 "SETWIFI <ssid> <pass>"，成功写 NVS 后返回 true
 * （调用方收到 true 后 esp_restart()）。非 SETWIFI 命令返回 false。 */
bool wifi_config_handle_serial(const char *line);
