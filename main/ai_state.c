#include <string.h>
#include "ai_state.h"

/* 事件名（kebab-case）与 ai-light hook 协议严格一致 */
static const char *k_event_names[] = {
    [AI_EV_SESSION_START]      = "session-start",
    [AI_EV_PROMPT_SUBMIT]      = "prompt-submit",
    [AI_EV_PRE_TOOL_USE]       = "pre-tool-use",
    [AI_EV_POST_TOOL_USE]      = "post-tool-use",
    [AI_EV_PERMISSION_REQUEST] = "permission-request",
    [AI_EV_NOTIFICATION]       = "notification",
    [AI_EV_STOP]               = "stop",
    [AI_EV_SESSION_END]        = "session-end",
    [AI_EV_UNKNOWN]            = "unknown",
};

/* Claude Code 原生 hook 事件名（PascalCase） */
static const char *k_event_names_pascal[] = {
    [AI_EV_SESSION_START]      = "SessionStart",
    [AI_EV_PROMPT_SUBMIT]      = "UserPromptSubmit",
    [AI_EV_PRE_TOOL_USE]       = "PreToolUse",
    [AI_EV_POST_TOOL_USE]      = "PostToolUse",
    [AI_EV_PERMISSION_REQUEST] = "PermissionRequest",
    [AI_EV_NOTIFICATION]       = "Notification",
    [AI_EV_STOP]               = "Stop",
    [AI_EV_SESSION_END]        = "SessionEnd",
    [AI_EV_UNKNOWN]            = "",
};

static const char *k_state_names[] = {
    [AI_STATE_IDLE]    = "IDLE",
    [AI_STATE_WORKING] = "WORKING",
    [AI_STATE_DONE]    = "DONE",
    [AI_STATE_ERROR]   = "ERROR",
};

const char *ai_state_name(ai_state_t s)
{
    /* Q06: 负值/越界都返回 "?"——坏数据不许一路打进数组索引 */
    return (s >= AI_STATE_IDLE && s <= AI_STATE_ERROR) ? k_state_names[s] : "?";
}

const char *ai_event_name(ai_event_type_t e)
{
    return (e >= AI_EV_SESSION_START && e <= AI_EV_UNKNOWN) ? k_event_names[e] : "?";
}

ai_event_type_t ai_event_from_str(const char *s)
{
    if (s == NULL) {
        return AI_EV_UNKNOWN;
    }
    for (int i = 0; i < AI_EV_UNKNOWN; i++) {
        if (strcmp(s, k_event_names[i]) == 0) {
            return (ai_event_type_t)i;
        }
    }
    /* 兼容 Claude Code 原生 PascalCase 事件名（hook_event_name 字段直接透传时） */
    for (int i = 0; i < AI_EV_UNKNOWN; i++) {
        if (strcmp(s, k_event_names_pascal[i]) == 0) {
            return (ai_event_type_t)i;
        }
    }
    return AI_EV_UNKNOWN;
}

/*
 * Hook 事件 → 灯状态映射（源自 ai-light 设计文档 §6.3）：
 *   session-start  → idle（灯组出现）
 *   prompt-submit / pre / post-tool-use → working（黄）
 *   notification / permission-request   → error（红，需要人介入）
 *   stop           → done（绿）
 *   session-end    → 阶段2先归 idle；阶段3实现会话表后 = 灯组消失
 */
ai_state_t ai_state_from_event(ai_event_type_t e)
{
    switch (e) {
    case AI_EV_SESSION_START:
    case AI_EV_SESSION_END:
        return AI_STATE_IDLE;
    case AI_EV_PROMPT_SUBMIT:
    case AI_EV_PRE_TOOL_USE:
    case AI_EV_POST_TOOL_USE:
        return AI_STATE_WORKING;
    case AI_EV_PERMISSION_REQUEST:
    case AI_EV_NOTIFICATION:
        return AI_STATE_ERROR;
    case AI_EV_STOP:
        return AI_STATE_DONE;
    default:
        return AI_STATE_IDLE;
    }
}
