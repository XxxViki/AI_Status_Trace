/*
 * ai_sessions.c —— 会话表 v2 实现
 *
 * 关键结构：每个会话只存"语义状态 + 最后事件时间戳"，
 * 灯效模式是 (语义状态, 距上次事件时长) 的纯函数——不需要为灯效存状态，
 * 升级/衰减自然发生，不会出现"忘了转移"的边界 bug。
 *
 * 历史教训：
 *   #016 v1 版忘了给新会话设 used=true，事件全部落入"幽灵槽"，
 *   症状是日志正常但聚合永远为空——初始化标志位是固定数组方案的必修课
 */
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "ai_state.h"
#include "ai_sessions.h"

#define MAX_SESSIONS 8

/* 判定阈值（毫秒），会被 time_scale 缩放（仅用于测试加速） */
#define HEARTBEAT_LOST_MS   (180 * 1000)      /* working 无心跳 → 黄常亮(#052: 60s太急,长思考误判) */
#define APPROVAL_ESCALATE_MS (2 * 60 * 1000)   /* 等审批 → 升级红闪 */
#define APPROVAL_GIVEUP_MS  (10 * 60 * 1000)   /* 等审批无人理 → 降级完成（红闪封顶10分钟）*/
#define GREEN_FLASH_MS      (10 * 1000)        /* 绿闪衰减为绿常亮 */
#define WORKING_KILL_MS  (30 * 60 * 1000)   /* #054: 5min会误杀长思考会话;活进程由看门狗负责,此值只兜底ZCode关标签 */
#define GHOST_TIMEOUT_MS    (60 * 60 * 1000)   /* #054: 10min会误清"空闲但存活"的会话;进程被杀由看门狗即时清 */

static const char *TAG = "sessions";

typedef struct {
    char       session_id[40];
    char       tool[8];
    char       project[32];   /* #038 扩容 */
    char       last_tool[12];  /* 最近调用的工具（Bash/Edit…） */
    int32_t    tokens;         /* 上下文 token 用量（Claude 专属） */
    ai_state_t state;          /* 语义状态：IDLE/WORKING/DONE/ERROR(=等审批) */
    int64_t    started_ms;     /* 会话创建时刻（显示总时长） */
    int64_t    state_since_ms; /* 进入当前状态的时刻（think/等待计时） */
    int64_t    last_event_ms;
    bool       used;
    bool       taken;   /* ai_sessions_top 排序导出过程的临时标记 */
} session_slot_t;

static session_slot_t s_slots[MAX_SESSIONS];
static int s_time_scale = 1;
static bool s_dirty = false;          /* 有变更待持久化 */

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static int64_t scaled(int64_t ms)
{
    return ms / s_time_scale;
}

/* ---------- 灯效推导：纯函数，无副作用 ---------- */

static lamp_mode_t session_lamp(const session_slot_t *s, int64_t now)
{
    int64_t age = now - s->last_event_ms;
    switch (s->state) {
    case AI_STATE_WORKING:
        return (age >= scaled(HEARTBEAT_LOST_MS)) ? LAMP_YELLOW_STEADY : LAMP_YELLOW_BREATH;
    case AI_STATE_ERROR:   /* 语义：等待审批 */
        return (age >= scaled(APPROVAL_ESCALATE_MS)) ? LAMP_RED_FLASH : LAMP_YELLOW_FLASH;
    case AI_STATE_DONE:
        return (age < scaled(GREEN_FLASH_MS)) ? LAMP_GREEN_FLASH : LAMP_GREEN_STEADY;
    case AI_STATE_IDLE:    /* #070: 会话存在但无动作——显示 IDLE 卡(用户要求) */
        return LAMP_IDLE;
    default:
        return LAMP_OFF;
    }
}

static int mode_rank(lamp_mode_t m)
{
    /* 显示/聚合优先级（2026-09-26 与用户讨论定稿，见 #032）：
     * 红色审批最急；绿闪排第二的依据是"时间稀缺性"——只有10秒窗口，
     * 错过即消失；黄闪(等审批)持续存在且2分钟后自动升级为红回到第1位，
     * 所以排在绿闪之后不会丢失。 */
    switch (m) {
    case LAMP_RED_FLASH:     return 6;   /* 1. 审批晾超时 */
    case LAMP_GREEN_FLASH:   return 5;   /* 2. 新鲜结果(10s窗口) */
    case LAMP_YELLOW_FLASH:  return 4;   /* 3. 等审批(会自升级) */
    case LAMP_YELLOW_BREATH: return 3;   /* 4. 干活中(#052: 活动会话优先上屏) */
    case LAMP_YELLOW_STEADY: return 2;   /* 5. 心跳丢失(不确定,低于活动会话) */
    case LAMP_GREEN_STEADY:  return 1;   /* 6. 旧结果 */
    case LAMP_IDLE:          return 1;   /* 7. 空闲(#070: 和旧结果同级,都无行动需求) */
    default:                 return 0;   /* 7. 无动静 */
    }
}

/* ---------- 对外接口 ---------- */

void ai_sessions_init(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    s_time_scale = 1;
}

void ai_sessions_set_time_scale(int scale)
{
    if (scale < 1) {
        scale = 1;
    }
    if (scale > 120) {
        scale = 120;
    }
    s_time_scale = scale;
    ESP_LOGW(TAG, "时间缩放 x%d（仅测试用）", s_time_scale);
}

static session_slot_t *find_session(const char *sid)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_slots[i].used && strcmp(s_slots[i].session_id, sid) == 0) {
            return &s_slots[i];
        }
    }
    return NULL;
}

static session_slot_t *alloc_slot(void)
{
    session_slot_t *oldest = NULL;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!s_slots[i].used) {
            return &s_slots[i];
        }
        if (oldest == NULL || s_slots[i].last_event_ms < oldest->last_event_ms) {
            oldest = &s_slots[i];
        }
    }
    ESP_LOGW(TAG, "会话表满，驱逐最旧会话 %.20s", oldest->session_id);
    return oldest;
}

void ai_sessions_on_event(const ai_event_msg_t *msg)
{
    if (msg->event == AI_EV_SESSION_END) {
        session_slot_t *s = find_session(msg->session_id);
        if (s != NULL) {
            ESP_LOGI(TAG, "会话结束 %.20s", msg->session_id);
            s->used = false;
            s_dirty = true;
        }
        return;
    }

    session_slot_t *s = find_session(msg->session_id);
    bool is_new = (s == NULL);
    if (is_new) {
        /* 同(工具,项目)的去重: 重开同项目会话时旧卡(多为被杀进程残留)立即顶掉 */
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (s_slots[i].used && strcmp(s_slots[i].session_id, msg->session_id) != 0
                && msg->tool[0] && strcmp(s_slots[i].tool, msg->tool) == 0
                && msg->project[0] && strcmp(s_slots[i].project, msg->project) == 0) {
                ESP_LOGW(TAG, "同项目新会话顶掉旧卡 %.20s", s_slots[i].session_id);
                s_slots[i].used = false;
            }
        }
        s = alloc_slot();
        if (s == NULL) {
            return;
        }
        memset(s, 0, sizeof(*s));   /* 槽位卫生：新住户入住前彻底清空旧字段(#026)。
                                     * 漏此行时 tokens/last_tool 等会继承前任住户的值
                                     * （zcode 卡显示别的会话的 token 就是这么来的） */
        strlcpy(s->session_id, msg->session_id, sizeof(s->session_id));
        s->used = true;   /* v1 就漏了这一行（#016）：槽位写入但从不生效 */
    }
    if (s->state != msg->state) {
        s->state_since_ms = now_ms();   /* 状态切换时刻：think/等待计时的起点 */
        s_dirty = true;
    }
    s->state = msg->state;
    s->last_event_ms = now_ms();
    if (msg->tool[0]) {
        strlcpy(s->tool, msg->tool, sizeof(s->tool));
    }
    if (msg->last_tool[0]) {
        strlcpy(s->last_tool, msg->last_tool, sizeof(s->last_tool));
    }
    if (msg->tokens > 0) {
        s->tokens = msg->tokens;
        s_dirty = true;
    }
    /* 项目名只在会话首次事件时锁定(#037)：ZCode 的 cwd 会跟着终端 cd 漂移，
     * 若每次覆盖，卡片名字会变来变去（用户看到的"任务名不对"）*/
    if (msg->project[0] && !s->project[0]) {
        strlcpy(s->project, msg->project, sizeof(s->project));
    }
    s_dirty = true;
    if (is_new) {
        s->started_ms = now_ms();
        s->state_since_ms = now_ms();
        ESP_LOGI(TAG, "新会话 %.20s [%s/%s] -> %s", msg->session_id,
                 s->tool[0] ? s->tool : "?", s->project[0] ? s->project : "?",
                 ai_state_name(msg->state));
    }
}

void ai_sessions_tick(void)
{
    int64_t now = now_ms();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!s_slots[i].used) {
            continue;
        }
        int64_t age = now - s_slots[i].last_event_ms;
        if (s_slots[i].state == AI_STATE_WORKING && age >= scaled(WORKING_KILL_MS)) {
            /* working 5 分钟无任何心跳：进程大概率被杀（真在干活时工具事件几秒一个），
               直接清卡片——比猜测"判定完成"更诚实 */
            ESP_LOGW(TAG, "会话 %.20s 心跳超时5分钟,视为已杀,清理", s_slots[i].session_id);
            s_slots[i].used = false;
        } else if (s_slots[i].state == AI_STATE_ERROR && age >= scaled(APPROVAL_GIVEUP_MS)) {
            /* 等审批被晾超过10分钟：批准后的恢复事件大概率丢了，或窗口被直接关掉。
             * 红闪封顶10分钟后降级完成——红灯警示性强，不能无限期占用（#018） */
            s_slots[i].state = AI_STATE_DONE;
            s_slots[i].state_since_ms = now;
            ESP_LOGW(TAG, "会话 %.20s 审批等待超时，降级完成", s_slots[i].session_id);
        } else if (s_slots[i].state != AI_STATE_WORKING && age >= scaled(GHOST_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "幽灵会话 %.20s 超时清理", s_slots[i].session_id);
            s_slots[i].used = false;
        }
    }
}

lamp_mode_t ai_sessions_aggregate(void)
{
    int64_t now = now_ms();
    lamp_mode_t best = LAMP_OFF;
    int best_rank = 0;
    int used = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!s_slots[i].used) {
            continue;
        }
        used++;
        int r = mode_rank(session_lamp(&s_slots[i], now));
        if (r > best_rank) {
            best_rank = r;
            best = session_lamp(&s_slots[i], now);
        }
    }
    return used ? best : LAMP_OFF;
}

int ai_sessions_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        n += s_slots[i].used ? 1 : 0;
    }
    return n;
}

int ai_sessions_dump_json(char *buf, int buflen)
{
    int64_t now = now_ms();
    int off = 0;
    off += snprintf(buf + off, buflen - off, "[");
    for (int i = 0; i < MAX_SESSIONS && off < buflen - 48; i++) {
        if (!s_slots[i].used) {
            continue;
        }
        if (off > 1) {
            buf[off++] = ',';
        }
        off += snprintf(buf + off, buflen - off,
                        "{\"id\":\"%.8s\",\"tool\":\"%s\",\"proj\":\"%s\",\"state\":\"%s\",\"tok\":%ld,\"idle_s\":%lld}",
                        s_slots[i].session_id,
                        s_slots[i].tool[0] ? s_slots[i].tool : "?",
                        s_slots[i].project[0] ? s_slots[i].project : "?",
                        ai_state_name(s_slots[i].state),
                        (long)s_slots[i].tokens,
                        (long long)((now - s_slots[i].last_event_ms) / 1000));
    }
    off += snprintf(buf + off, buflen - off, "]");
    return off;
}

const char *ai_sessions_mode_name(lamp_mode_t m)
{
    switch (m) {
    case LAMP_OFF:           return "OFF";
    case LAMP_GREEN_STEADY:  return "GREEN_STEADY";
    case LAMP_GREEN_FLASH:   return "GREEN_FLASH";
    case LAMP_YELLOW_BREATH: return "YELLOW_BREATH";
    case LAMP_YELLOW_STEADY: return "YELLOW_STEADY";
    case LAMP_YELLOW_FLASH:  return "YELLOW_FLASH";
    case LAMP_RED_FLASH:     return "RED_FLASH";
    default:                 return "?";
    }
}

/* 按严重度降序导出前 max 个会话（卡片显示用） */
int ai_sessions_top(ai_card_info_t *out, int max)
{
    int64_t now = now_ms();
    int n = 0;
    /* 选择排序：最多 8 个会话，直接两轮扫描即可 */
    for (;;) {
        int best = -1, best_rank = -1;
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (!s_slots[i].used || s_slots[i].taken) {
                continue;
            }
            int r = mode_rank(session_lamp(&s_slots[i], now));
            /* 同severity时先到者排前(遍历序即数组序,近似按创建顺序) */
            if (r > best_rank) {
                best_rank = r;
                best = i;
            }
        }
        if (best < 0 || n >= max) {
            break;
        }
        session_slot_t *s = &s_slots[best];
        s->taken = true;   /* 漏了这行=死循环：同一槽位被反复选中(#020) */
        strlcpy(out[n].id, s->session_id, sizeof(out[n].id));
        strlcpy(out[n].tool, s->tool, sizeof(out[n].tool));
        strlcpy(out[n].project, s->project, sizeof(out[n].project));
        strlcpy(out[n].last_tool, s->last_tool, sizeof(out[n].last_tool));
        out[n].tokens = s->tokens;
        out[n].state = s->state;
        out[n].lamp = session_lamp(s, now);
        out[n].started_ms = s->started_ms;
        out[n].state_since_ms = s->state_since_ms;
        n++;
    }
    /* 清理 taken 标记 */
    for (int i = 0; i < MAX_SESSIONS; i++) {
        s_slots[i].taken = false;
    }
    return n;
}

/* ==================== NVS 持久化 ====================
 * 背景(#023): 烧录/断电重启会清空 RAM 会话表,把正在运行的活会话也"误杀"。
 * 方案: 表有变更时写入 NVS blob; 启动时恢复,并用
 *       (本次开机uptime - 保存时uptime) 平移所有时间戳,时长跨重启连续。
 * blob: [uptime_ms(8B)][count(1B)][slot...: used+state(2) + id/tool/proj/last_tool
 *        (40+8+16+12) + started/since/last(8*3)]                        */
#include "nvs_flash.h"

typedef struct __attribute__((packed)) {
    uint8_t used, state;
    char    id[40], tool[8], proj[32], last_tool[12];
    int32_t tokens;
    int64_t started_ms, state_since_ms, last_event_ms;
} slot_blob_t;

void ai_sessions_maybe_save(void)
{
    if (!s_dirty) {
        return;
    }
    s_dirty = false;
    nvs_handle_t h;
    if (nvs_open("ai_status", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    uint8_t buf[9 + 1 + sizeof(slot_blob_t) * MAX_SESSIONS];
    buf[0] = 4;   /* blob 版本: 结构/语义变更必须递增,旧版直接废弃(#024/#026/#038) */
    int64_t up = now_ms();
    memcpy(buf + 1, &up, 8);
    uint8_t n = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!s_slots[i].used) {
            continue;
        }
        slot_blob_t b = {
            .used = 1, .state = (uint8_t)s_slots[i].state, .tokens = s_slots[i].tokens,
            .started_ms = s_slots[i].started_ms,
            .state_since_ms = s_slots[i].state_since_ms,
            .last_event_ms = s_slots[i].last_event_ms,
        };
        strlcpy(b.id, s_slots[i].session_id, sizeof(b.id));
        strlcpy(b.tool, s_slots[i].tool, sizeof(b.tool));
        strlcpy(b.proj, s_slots[i].project, sizeof(b.proj));
        strlcpy(b.last_tool, s_slots[i].last_tool, sizeof(b.last_tool));
        memcpy(buf + 10 + n * sizeof(slot_blob_t), &b, sizeof(b));
        n++;
    }
    buf[9] = n;
    nvs_set_blob(h, "sessions", buf, 10 + n * sizeof(slot_blob_t));
    nvs_commit(h);
    nvs_close(h);
}

void ai_sessions_load(void)
{
    nvs_handle_t h;
    if (nvs_open("ai_status", NVS_READONLY, &h) != ESP_OK) {
        return;   /* 首次开机无数据 */
    }
    uint8_t buf[9 + 1 + sizeof(slot_blob_t) * MAX_SESSIONS];
    size_t len = sizeof(buf);
    if (nvs_get_blob(h, "sessions", buf, &len) != ESP_OK || len < 10
        || buf[0] != 4) {
        /* 版本不符(结构已变更)或无数据: 废弃旧 blob,干净起步(#024) */
        nvs_erase_key(h, "sessions");
        nvs_commit(h);
        nvs_close(h);
        return;
    }
    int64_t saved_up;
    memcpy(&saved_up, buf + 1, 8);
    int64_t delta = now_ms() - saved_up;   /* 跨重启的时间平移量 */
    uint8_t n = buf[9];
    for (uint8_t k = 0; k < n; k++) {
        slot_blob_t b;
        memcpy(&b, buf + 10 + k * sizeof(slot_blob_t), sizeof(b));
        if (!b.used) {
            continue;
        }
        session_slot_t *s = alloc_slot();
        if (s == NULL) {
            break;
        }
        strlcpy(s->session_id, b.id, sizeof(s->session_id));
        strlcpy(s->tool, b.tool, sizeof(s->tool));
        strlcpy(s->project, b.proj, sizeof(s->project));
        strlcpy(s->last_tool, b.last_tool, sizeof(s->last_tool));
        s->state = (ai_state_t)b.state;
        s->tokens = b.tokens;
        s->started_ms = b.started_ms + delta;
        s->state_since_ms = b.state_since_ms + delta;
        s->last_event_ms = b.last_event_ms + delta;
        s->used = true;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "从 NVS 恢复 %d 个会话(时间平移 %lld ms)", ai_sessions_count(),
             (long long)delta);
}

int ai_sessions_clear(const char *tool, const char *id_prefix)
{
    int n = 0;
    int plen = (id_prefix != NULL) ? (int)strlen(id_prefix) : 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!s_slots[i].used) {
            continue;
        }
        bool match;
        if (plen > 0) {
            match = (strncmp(s_slots[i].session_id, id_prefix, plen) == 0);
        } else if (tool != NULL && tool[0]) {
            match = (strcmp(s_slots[i].tool, tool) == 0);
        } else {
            match = true;
        }
        if (match) {
            ESP_LOGW(TAG, "看门狗清理会话 %.20s [%s]", s_slots[i].session_id, s_slots[i].tool);
            s_slots[i].used = false;
            n++;
        }
    }
    if (n > 0) {
        s_dirty = true;
    }
    return n;
}

int ai_sessions_set_tokens(const char *sid, int32_t tokens)
{
    if (sid == NULL || sid[0] == '\0') {
        return 0;
    }
    int plen = (int)strlen(sid);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!s_slots[i].used) {
            continue;
        }
        if (strncmp(s_slots[i].session_id, sid, plen) == 0) {
            if (s_slots[i].tokens != tokens) {
                s_slots[i].tokens = tokens;
                s_dirty = true;
            }
            return 1;
        }
    }
    return 0;
}
