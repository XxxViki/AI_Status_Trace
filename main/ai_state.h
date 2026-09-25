/*
 * ai_state.h —— AI 状态机：事件类型、灯状态、映射关系
 *
 * 这是整个项目的"业务核心"，协议沿用 ai-light 桌面版的 HookEvent。
 * 阶段 3 接 Claude Code hooks、阶段 5 接 ZCode 时，都走同一套事件定义。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 灯状态（与 ai-light 设计一致：error > working > done > idle 聚合） */
typedef enum {
    AI_STATE_IDLE = 0,   /* 会话存在但没开始干活：三灯全暗 */
    AI_STATE_WORKING,    /* AI 正在执行：黄灯 */
    AI_STATE_DONE,       /* 出结果了：绿灯 */
    AI_STATE_ERROR,      /* 报错 / 等权限确认：红灯 */
} ai_state_t;

/* hook 事件类型（POST /events 报文里 event_type 字段的取值） */
typedef enum {
    AI_EV_SESSION_START = 0,
    AI_EV_PROMPT_SUBMIT,
    AI_EV_PRE_TOOL_USE,
    AI_EV_POST_TOOL_USE,
    AI_EV_PERMISSION_REQUEST,
    AI_EV_NOTIFICATION,
    AI_EV_STOP,
    AI_EV_SESSION_END,
    AI_EV_UNKNOWN,
} ai_event_type_t;

/* 队列里传递的消息：HTTP 任务生产，显示任务消费 */
typedef struct {
    ai_event_type_t event;
    ai_state_t      state;              /* 由 event 映射好的灯状态 */
    char            session_id[40];     /* 会话标识 */
    char            tool[8];            /* ai 工具：claude / zcode */
    char            project[32];        /* 项目名（cwd basename，长名不截断#038）*/
    char            last_tool[12];      /* 最近调用的工具名（Bash/Edit…）*/
    int32_t         tokens;             /* 上下文 token 用量（Stop 事件附带）*/
} ai_event_msg_t;

const char      *ai_state_name(ai_state_t s);
const char      *ai_event_name(ai_event_type_t e);
ai_event_type_t  ai_event_from_str(const char *s);
ai_state_t       ai_state_from_event(ai_event_type_t e);

#ifdef __cplusplus
}
#endif
