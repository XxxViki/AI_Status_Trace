# 组件构建脚本：把 BSP 里所有源文件编进固件
# REQUIRES 声明依赖的其它组件（头文件搜索路径会传递）
idf_component_register(
    SRCS
        "io_extension.c"
        "ST7789.c"
        "Vernon_ST7789T/Vernon_ST7789T.c"
    INCLUDE_DIRS
        "."
        "include"
        "Vernon_ST7789T"
    REQUIRES
        esp_lcd
        driver
        esp_timer
)
