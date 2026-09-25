# main 组件：应用入口 + 业务模块
idf_component_register(
    SRCS
        "main.c"
        "ai_state.c"
        "ai_sessions.c"
        "app_wifi.c"
        "app_http.c"
        "app_display.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        bsp            # 板级驱动（LCD/IO扩展）
        esp_wifi       # WiFi
        esp_netif      # TCP/IP 协议栈
        esp_event      # 事件循环
        nvs_flash      # WiFi 驱动依赖的键值存储
        esp_http_server # HTTP 服务
        espressif__cjson # cJSON 解析（registry 组件，由 idf_component.yml 引入）
)
