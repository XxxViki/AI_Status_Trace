/*
 * ai_sessions.h —— 会话表 v2：语义状态 + 灯效推导 + 聚合（灯效状态机 v2）
 *
 * 设计（2026-09-26 与用户讨论定稿）：
 *   黄·呼吸  = 在干活，心跳正常（1s 周期）
 *   黄·常亮  = 心跳丢失 ≥60s（卡住？跟踪断了？诚实显示不确定）
 *   黄·频闪  = 等待审批（3Hz）
 *   红·频闪  = 审批被晾 ≥2min，升级警示（红稀缺，防焦虑）
 *   绿·频闪  = 有结果，前 10s 抓注意力
 *   绿·常亮  = 结果放着，不催
 *   全暗     = 无活跃会话
 *
 * 多会话聚合：最严重档优先（红闪>黄闪>黄常亮>黄呼吸>绿闪>绿常亮>暗）
 */
#pragma once

#include <stdint.h>
#include "ai_state.h"

/* 灯效模式（显示层关心的最终形态） */
typedef enum {
    LAMP_OFF = 0,
    LAMP_IDLE,               /* #070: 会话存在但无动作——显示 IDLE 卡 */
    LAMP_GREEN_STEADY,
    LAMP_GREEN_FLASH,
    LAMP_YELLOW_BREATH,
    LAMP_YELLOW_STEADY,
    LAMP_YELLOW_FLASH,
    LAMP_RED_FLASH,
} lamp_mode_t;

void       ai_sessions_init(void);
void       ai_sessions_on_event(const ai_event_msg_t *msg);
void       ai_sessions_tick(void);                 /* 周期调用：超时转移与清理 */
lamp_mode_t ai_sessions_aggregate(void);           /* 所有会话的最严重灯效 */
int        ai_sessions_count(void);
void       ai_sessions_tool_stats(const char *tool, int64_t *tokens, int *count); /* #077 统计页 */
const char *ai_sessions_mode_name(lamp_mode_t m);

/* 调试用：把会话表导出成 JSON 数组（id前8位/语义状态/无事件秒数），写入 buf */
int ai_sessions_dump_json(char *buf, int buflen);

/* 卡片显示用：按严重度降序导出前 max 个会话的展示信息 */
typedef struct {
    char       id[12];
    char       tool[8];
    char       project[16];
    char       last_tool[12];
    int32_t    tokens;          /* 上下文 token 用量 */
    ai_state_t state;
    lamp_mode_t lamp;
    int64_t    started_ms;   /* 会话创建时刻（算总时长） */
    int64_t    state_since_ms; /* 进入当前状态的时刻（算 think/等待时长） */
} ai_card_info_t;
int ai_sessions_top(ai_card_info_t *out, int max);

/* 测试加速：把所有超时阈值除以 scale（仅缩放判定阈值，不缩放动画周期）。
 * Q10: 设置 5 分钟后自动复位回 1，/state 会回显当前值 */
void ai_sessions_set_time_scale(int scale);
int  ai_sessions_time_scale(void);

/* NVS 持久化：重启后恢复会话表（时间基准自动校正），显示任务周期调用保存 */
void ai_sessions_load(void);
void ai_sessions_maybe_save(void);

/* 看门狗清理接口：id_prefix 优先(前缀匹配)，否则按 tool 清，都空=清全部。
 * 返回移除的会话数。用于主机侧进程巡检发现"进程已死但卡还在"(#027) */
int ai_sessions_clear(const char *tool, const char *id_prefix);

/* 只更新 token 计数(不改状态)：实时 token 监视器用（#043） */
int ai_sessions_set_tokens(const char *sid, int32_t tokens);
