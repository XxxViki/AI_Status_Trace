#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "ai_state.h"

/* 初始化显示：创建事件队列和显示任务 */
void app_display_init(void);

/* HTTP 任务往这个队列里投递事件（队列满自动丢弃旧事件策略见实现） */
QueueHandle_t app_display_queue(void);

/* 当前灯状态（供 /state 查询） */
ai_state_t app_display_current_state(void);

/* 调试用：读逻辑坐标的帧缓冲像素（/dbg/row 用，#047 排查） */
uint16_t app_display_pixel(int lx, int ly);

/* 调试用：请求切到指定页(下一次循环生效) */
void app_display_set_page(int page);
