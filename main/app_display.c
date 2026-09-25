/*
 * app_display.c —— 显示任务 v4：横屏多卡片（逻辑/物理坐标分层）
 *
 * 坐标体系（本模块最重要的概念）：
 *   逻辑层：横屏 320x172，所有绘图 API 都用逻辑坐标（直觉坐标）
 *   物理层：面板原生竖屏 180x320，s_frame 与 draw_bitmap 都用物理坐标
 *   旋转：px() 里做 90° 顺时针映射 lx,ly -> px=171-ly, py=lx
 *   局部刷新：逻辑矩形转置成物理矩形，行段连续可直接 memcpy（#019）
 *
 * 布局（320x172 横屏）：
 *   y0..17   头部：聚合状态点(动画) + "AI STATUS" + 溢出 "+N"
 *   y20..167 三张卡片并排（101x148，严重度降序，第1张色条参与动画）
 *   卡片：顶部状态色条 / 26x26 工具徽章 / 总时长(×2) / 状态词(×2) / 项目名(×1)
 */
#include <stdint.h>
#include <ctype.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ST7789.h"
#include "ai_state.h"
#include "ai_sessions.h"
#include "font5x7.h"
#include "app_display.h"

static const char *TAG = "disp";

/* 物理面板（竖屏原生）与逻辑横屏 */
#define PW 180
#define PH 320
#define LW 320
#define LH 172

#define RGB565(r, g, b) ((uint16_t)(((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

#define COL_BG      RGB565(0x14, 0x14, 0x18)
#define COL_CARD    RGB565(0x24, 0x24, 0x2c)
#define COL_CARD_BG RGB565(0x1c, 0x1c, 0x24)
#define COL_TXT     RGB565(0xe8, 0xe8, 0xe8)
#define COL_TXT_DIM RGB565(0x90, 0x90, 0x98)
#define COL_RED     RGB565(0xef, 0x44, 0x44)
#define COL_YEL     RGB565(0xfb, 0xbf, 0x24)
#define COL_GRN     RGB565(0x22, 0xc5, 0x5e)
#define COL_STUCK   RGB565(0xd8, 0xd8, 0xd8)  /* STUCK=中性白:无信号=无颜色语义 */
#define COL_CLAUDE  RGB565(0xd9, 0x77, 0x57)   /* Anthropic 珊瑚色 */
#define COL_ZCODE   RGB565(0x4e, 0x9e, 0xe8)   /* ZCode 蓝 */

#define FRAME_MS    50
#define BREATH_MS   1000
#define FLASH_MS    333

#define CARD_W      101
#define CARD_H      148
#define CARD_X0     4
#define CARD_Y0     20
#define CARD_GAP    4

static uint16_t s_frame[PH * PW];          /* 物理竖屏全帧（.bss ~115KB） */
static uint16_t s_flush_buf[LW * 48];      /* 局部刷新过渡(卡片区148x101=14948需48行) */
static QueueHandle_t s_queue;

/* ---------- 坐标旋转层：逻辑 -> 物理 ---------- */

static void px(int lx, int ly, uint16_t c)
{
    if (lx < 0 || lx >= LW || ly < 0 || ly >= LH) {
        return;
    }
    s_frame[lx * PW + (LH - 1 - ly)] = c;   /* px=171-ly, py=lx */
}

static void fill_rect(int x0, int y0, int x1, int y1, uint16_t c)
{
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            px(x, y, c);
        }
    }
}

static void fill_circle(int cx, int cy, int r, uint16_t c)
{
    for (int y = cy - r; y <= cy + r; y++) {
        for (int x = cx - r; x <= cx + r; x++) {
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= r * r) {
                px(x, y, c);
            }
        }
    }
}

static uint16_t blend565(uint16_t a, uint16_t b, int ia)
{
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    return (uint16_t)((((ar * (255 - ia) + br * ia) / 255) << 11)
                    | (((ag * (255 - ia) + bg * ia) / 255) << 5)
                    |  ((ab * (255 - ia) + bb * ia) / 255));
}

/* 逻辑区域 -> 物理区域推屏：逻辑矩形的每一行在物理帧里是一段连续内存，
 * 转置后按物理行 memcpy 连续段进过渡缓冲再发送（#017 的行距教训同样适用） */
static void flush_region(int x0, int y0, int x1, int y1)
{
    int phys_x0 = LH - 1 - y1, phys_x1 = LH - 1 - y0;
    int phys_y0 = x0, phys_y1 = x1;
    int w = phys_x1 - phys_x0 + 1, h = phys_y1 - phys_y0 + 1;
    if (w <= 0 || h <= 0 || w * h > (int)(sizeof(s_flush_buf) / sizeof(uint16_t))) {
        return;   /* 超出过渡缓冲：布局设计上不会发生 */
    }
    for (int r = 0; r < h; r++) {
        memcpy(&s_flush_buf[r * w], &s_frame[(phys_y0 + r) * PW + phys_x0],
               w * sizeof(uint16_t));
    }
    esp_lcd_panel_draw_bitmap(panel_handle, phys_x0, phys_y0,
                              phys_x1 + 1, phys_y1 + 1, s_flush_buf);
}

static void flush_all(void)
{
    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, PW, PH, s_frame);
}

/* ---------- 文字 ---------- */

static void draw_char(int x, int y, char c, int scale, uint16_t color)
{
    const uint8_t *g = k_font5x7[(int)(unsigned char)c];
    for (int r = 0; r < FONT5X7_H; r++) {
        for (int col = 0; col < FONT5X7_W; col++) {
            if (g[r] & (0x10 >> col)) {
                fill_rect(x + col * scale, y + r * scale,
                          x + col * scale + scale - 1, y + r * scale + scale - 1, color);
            }
        }
    }
}

static void draw_text(int x, int y, const char *s, int scale, uint16_t color)
{
    for (const char *p = s; *p; p++, x += (FONT5X7_W + 1) * scale) {
        draw_char(x, y, (char)toupper((unsigned char)*p), scale, color);
    }
}

static int text_w(const char *s, int scale)
{
    return (int)strlen(s) * (FONT5X7_W + 1) * scale - scale;
}

static void draw_text_centered(int cx, int y, const char *s, int scale, uint16_t color)
{
    draw_text(cx - text_w(s, scale) / 2, y, s, scale, color);
}

/* ---------- 工具徽章（26x26） ---------- */

static void draw_logo_claude(int x, int y)
{
    /* Anthropic 星芒标：中心 + 12 条放射线 */
    float cx = x + 12.5f, cy = y + 12.5f;
    fill_circle((int)cx, (int)cy, 2, COL_CLAUDE);
    for (int i = 0; i < 12; i++) {
        float a = i * (3.1415926f * 2 / 12);
        float dx = cosf(a), dy = sinf(a);
        for (float r = 4.0f; r <= 11.0f; r += 0.5f) {
            px((int)(cx + dx * r), (int)(cy + dy * r), COL_CLAUDE);
            px((int)(cx + dx * r) + 1, (int)(cy + dy * r), COL_CLAUDE);
        }
    }
}

/* 简单线段（步进插值，画两遍错位1px加粗） */
static void draw_seg(int x0, int y0, int x1, int y1, uint16_t c)
{
    int steps = abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0);
    if (steps == 0) {
        steps = 1;
    }
    for (int s = 0; s <= steps; s++) {
        int ix = x0 + (x1 - x0) * s / steps;
        int iy = y0 + (y1 - y0) * s / steps;
        px(ix, iy, c);
        px(ix + 1, iy, c);
    }
}

/* Codex：双层六边形节点环（OpenAI 系极简风） */
static void draw_ring6(int cx, int cy, int r, uint16_t c)
{
    int hx[6], hy[6];
    for (int i = 0; i < 6; i++) {
        float a = 3.1415926f / 3.0f * i - 3.1415926f / 6.0f;
        hx[i] = (int)(cx + cosf(a) * r);
        hy[i] = (int)(cy + sinf(a) * r);
    }
    for (int i = 0; i < 6; i++) {
        int j = (i + 1) % 6;
        draw_seg(hx[i], hy[i], hx[j], hy[j], c);
    }
}

static void draw_logo_codex(int x, int y)
{
    draw_ring6(x + 13, y + 13, 11, COL_TXT);
    draw_ring6(x + 13, y + 13, 6, COL_TXT_DIM);
    fill_circle(x + 13, y + 13, 1, COL_TXT);
}

/* Trae：矩形外框 + 中间两个方块（用户指正的真实标志） */
static void draw_logo_trae(int x, int y)
{
    uint16_t c = RGB565(0x00, 0xb9, 0xa6);
    /* 外框 2px */
    fill_rect(x + 2, y + 2, x + 23, y + 3, c);
    fill_rect(x + 2, y + 22, x + 23, y + 23, c);
    fill_rect(x + 2, y + 2, x + 3, y + 23, c);
    fill_rect(x + 22, y + 2, x + 23, y + 23, c);
    /* 中间并排两个方块 */
    fill_rect(x + 6, y + 10, x + 11, y + 15, c);
    fill_rect(x + 14, y + 10, x + 19, y + 15, c);
}

static void draw_logo_zcode(int x, int y)
{
    fill_rect(x, y, x + 25, y + 25, COL_ZCODE);
    fill_rect(x + 2, y + 2, x + 23, y + 23, COL_CARD_BG);
    draw_text(x + 8, y + 6, "Z", 2, COL_ZCODE);
}

/* 通用首字母徽章：已登记工具用品牌色，未登记的任何工具自动有辨识度 */
static void draw_logo_badge(int x, int y, char letter, uint16_t color)
{
    fill_rect(x, y, x + 25, y + 25, color);
    fill_rect(x + 2, y + 2, x + 23, y + 23, COL_CARD_BG);
    char s[2] = { letter, '\0' };
    draw_text(x + 13 - text_w(s, 2) / 2, y + 6, s, 2, color);
}

static void draw_logo(const char *tool, int x, int y)
{
    if (tool[0] == '\0') {
        draw_logo_badge(x, y, '?', COL_TXT_DIM);
        return;
    }
    if (strcmp(tool, "claude") == 0) {
        draw_logo_claude(x, y);
        return;
    }
    if (strcmp(tool, "codex") == 0) {
        draw_logo_codex(x, y);
        return;
    }
    if (strcmp(tool, "trae") == 0) {
        draw_logo_trae(x, y);
        return;
    }
    /* 已登记工具表：名字 / 首字母 / 品牌色 */
    static const struct {
        const char *name;
        char letter;
        uint16_t color;
    } k_tools[] = {
        { "zcode",    'Z', COL_ZCODE },
        { "chatgpt",  'G', RGB565(0x10, 0xa3, 0x7f) },  /* OpenAI 绿 */
        { "hermes",   'H', RGB565(0x8b, 0x5c, 0xf6) },  /* Hermes 紫 */
    };
    for (int i = 0; i < (int)(sizeof(k_tools) / sizeof(k_tools[0])); i++) {
        if (strcmp(tool, k_tools[i].name) == 0) {
            draw_logo_badge(x, y, k_tools[i].letter, k_tools[i].color);
            return;
        }
    }
    /* 未登记工具：首字母 + 中性灰，接新工具零固件改动即可显示 */
    draw_logo_badge(x, y, (char)toupper((unsigned char)tool[0]), COL_TXT_DIM);
}

/* ---------- 灯效辅助 ---------- */

static uint16_t mode_anim_color(lamp_mode_t m, uint16_t on, uint16_t off, int64_t now)
{
    switch (m) {
    case LAMP_YELLOW_BREATH: {
        int phase100 = (int)((now % BREATH_MS) * 100 / BREATH_MS);
        int level = 77 + (int)(23.0f * cosf(2.0f * 3.1415926f * phase100 / 100.0f));
        return blend565(off, on, level * 255 / 100);
    }
    case LAMP_YELLOW_FLASH:
    case LAMP_RED_FLASH:
    case LAMP_GREEN_FLASH:
        return (now % FLASH_MS) < (FLASH_MS / 2) ? on : off;
    default:
        return on;
    }
}

static const char *mode_word(lamp_mode_t m)
{
    switch (m) {
    case LAMP_YELLOW_BREATH: return "WORKING";
    case LAMP_YELLOW_STEADY: return "STUCK?";
    case LAMP_YELLOW_FLASH:  return "APPROVE";
    case LAMP_RED_FLASH:     return "OVERDUE!";
    case LAMP_GREEN_FLASH:   return "DONE!";
    case LAMP_GREEN_STEADY:  return "DONE";
    default:                 return "";
    }
}

static uint16_t mode_color(lamp_mode_t m)
{
    switch (m) {
    case LAMP_RED_FLASH:     return COL_RED;
    case LAMP_GREEN_FLASH:
    case LAMP_GREEN_STEADY:  return COL_GRN;
    case LAMP_YELLOW_STEADY: return COL_STUCK;    /* STUCK: 中性白常亮(与红黄绿全拉开) */
    case LAMP_OFF:           return COL_TXT_DIM;
    default:                 return COL_YEL;
    }
}

/* ---------- 布局绘制 ---------- */

static void draw_duration(int x_right, int y, int64_t started_ms, int64_t now)
{
    int64_t sec = (now - started_ms) / 1000;
    if (sec < 0) {
        sec = 0;
    }
    if (sec >= 3600) {
        char buf[10];
        snprintf(buf, sizeof(buf), "%dh%02dm", (int)(sec / 3600), (int)((sec % 3600) / 60));
        draw_text(x_right - text_w(buf, 2), y, buf, 2, COL_TXT);
        return;
    }
    /* MM:SS，冒号 1Hz 闪烁（亮500ms灭500ms）：分钟/冒号/秒分三次画 */
    char mm[4], ss[4];
    snprintf(mm, sizeof(mm), "%d", (int)(sec / 60));
    snprintf(ss, sizeof(ss), "%02d", (int)(sec % 60));
    int n_mm = (int)strlen(mm);
    int total = (n_mm + 3) * 12 - 2;          /* ×2 字号每字符占 12px 格 */
    int x0 = x_right - total;
    draw_text(x0, y, mm, 2, COL_TXT);
    if ((now % 1000) < 500) {
        draw_char(x0 + n_mm * 12, y, ':', 2, COL_TXT);
    }
    draw_text(x0 + (n_mm + 1) * 12, y, ss, 2, COL_TXT);
}

/* 当前状态持续时长: THINK 0:45 / STUCK 2:10 / WAIT 1:30 / DONE 0:12 */
static void draw_state_age(int cx, int y, const ai_card_info_t *info, int64_t now)
{
    int64_t sec = (now - info->state_since_ms) / 1000;
    if (sec < 0) {
        sec = 0;
    }
    const char *label;
    switch (info->lamp) {
    case LAMP_YELLOW_BREATH: label = "THINK"; break;
    case LAMP_YELLOW_STEADY: label = "STUCK"; break;
    case LAMP_YELLOW_FLASH:  label = "WAIT";  break;
    case LAMP_RED_FLASH:     label = "WAIT";  break;
    default:                 label = "DONE";  break;
    }
    char buf[20];
    snprintf(buf, sizeof(buf), "%s %d:%02d", label, (int)(sec / 60), (int)(sec % 60));
    draw_text_centered(cx, y, buf, 1, COL_TXT_DIM);
}

/* 重画一张卡：徽章/时长/状态词/项目名 + 顶部状态色条 */
static void draw_card(int idx, const ai_card_info_t *info, int64_t now,
                      int32_t anim_color)   /* int32_t: RGB565 颜色 >=0x8000,不能用 int16 哨兵 */
{
    int x = CARD_X0 + idx * (CARD_W + CARD_GAP);
    int y = CARD_Y0;

    fill_rect(x, y, x + CARD_W - 1, y + CARD_H - 1, COL_CARD);      /* 外框 */
    fill_rect(x + 1, y + 3, x + CARD_W - 2, y + CARD_H - 2, COL_CARD_BG);

    uint16_t bar = (anim_color >= 0) ? (uint16_t)anim_color : mode_color(info->lamp);
    fill_rect(x, y, x + CARD_W - 1, y + 2, bar);

    draw_logo(info->tool, x + 7, y + 8);
    draw_text(x + 7, y + 38, info->tool[0] ? info->tool : "?", 1, COL_TXT_DIM);
    draw_duration(x + CARD_W - 7, y + 10, info->started_ms, now);   /* 会话总时长 */

    draw_text_centered(x + CARD_W / 2, y + 58, mode_word(info->lamp), 2,
                       mode_color(info->lamp));

    /* 当前状态持续时长（think/等待/卡住各计各的） */
    draw_state_age(x + CARD_W / 2, y + 82, info, now);

    char proj[16];
    strlcpy(proj, info->project[0] ? info->project
             : (info->tool[0] ? info->tool : "SESSION"), sizeof(proj));
    if ((int)strlen(proj) > 12) {
        proj[9] = proj[10] = proj[11] = '.';
        proj[12] = '\0';
    }
    draw_text_centered(x + CARD_W / 2, y + 102, proj, 1, COL_TXT);

    /* 最近调用的工具（BA/ED/WR/AG…），填充底部空间且有用 */
    if (info->last_tool[0]) {
        draw_text_centered(x + CARD_W / 2, y + 122, info->last_tool, 1, COL_TXT_DIM);
    }

    /* token 用量（Claude 专属，Stop 事件上报）：↓ 2.7K */
    if (info->tokens > 0) {
        char tb[16];
        if (info->tokens >= 1000) {
            snprintf(tb, sizeof(tb), "%d.%dK", (int)(info->tokens / 1000),
                     (int)((info->tokens % 1000) / 100));
        } else {
            snprintf(tb, sizeof(tb), "%d", (int)info->tokens);
        }
        int tw = 7 + 2 + text_w(tb, 1);          /* 箭头7px + 间距 + 文本 */
        int tx = x + CARD_W / 2 - tw / 2;
        int ay = y + 136;
        fill_rect(tx + 2, ay, tx + 2, ay + 3, COL_TXT_DIM);       /* 箭杆 */
        fill_rect(tx, ay + 4, tx + 4, ay + 4, COL_TXT_DIM);       /* 箭头横杠 */
        fill_rect(tx + 1, ay + 5, tx + 3, ay + 5, COL_TXT_DIM);   /* 收窄 */
        fill_rect(tx + 2, ay + 6, tx + 2, ay + 6, COL_TXT_DIM);   /* 箭尖朝下 */
        draw_text(tx + 9, ay, tb, 1, COL_TXT_DIM);
    }

    flush_region(x, y, x + CARD_W - 1, y + CARD_H - 1);
}

/* 头部：聚合状态点 + 标题 + 溢出计数 */
static void draw_header(lamp_mode_t agg, int total, int shown, int64_t now)
{
    fill_rect(0, 0, LW - 1, 17, COL_CARD);
    uint16_t on = mode_color(agg);
    uint16_t off = blend565(on, COL_CARD, 140);
    fill_circle(11, 9, 6, mode_anim_color(agg, on, off, now));
    draw_text(24, 5, "AI STATUS", 1, COL_TXT_DIM);
    if (total > shown) {
        char buf[8];
        snprintf(buf, sizeof(buf), "+%d", total - shown);
        draw_text(LW - 8 - text_w(buf, 1), 5, buf, 1, COL_TXT_DIM);
    }
    flush_region(0, 0, LW - 1, 17);
}

/* 每张卡的色条按"自己的状态"做动画（共享时钟 = 同相位同步呼吸，视觉统一）*/
static uint16_t card_anim_color(const ai_card_info_t *info, int64_t now)
{
    uint16_t on = mode_color(info->lamp);
    uint16_t off = blend565(on, COL_CARD, 140);
    return mode_anim_color(info->lamp, on, off, now);
}

/* ---------- 主任务 ---------- */

static void display_task(void *arg)
{
    (void)arg;
    /* 底图 */
    for (int i = 0; i < PH * PW; i++) {
        s_frame[i] = COL_BG;
    }
    for (int c = 0; c < 3; c++) {
        int x = CARD_X0 + c * (CARD_W + CARD_GAP);
        fill_rect(x, CARD_Y0, x + CARD_W - 1, CARD_Y0 + CARD_H - 1, COL_CARD);
    }
    flush_all();

    ai_event_msg_t msg;
    int64_t last_layout_ms = -10000;
    uint16_t last_card_anim[3] = { 0xFFFF, 0xFFFF, 0xFFFF };
    uint16_t last_header_anim = 0xFFFF;
    lamp_mode_t last_mode = LAMP_OFF;
    int last_n = -1;
    bool force_relayout = true;              /* 开机画一次 */

    for (;;) {
        while (xQueueReceive(s_queue, &msg, 0) == pdTRUE) {
            ai_sessions_on_event(&msg);
        }
        ai_sessions_tick();
        ai_sessions_maybe_save();   /* 有变更才写 NVS */

        int64_t now = esp_timer_get_time() / 1000;
        lamp_mode_t agg = ai_sessions_aggregate();
        if (agg != last_mode) {
            /* 状态一变立即全量重绘——审批提示零等待（原来最多等500ms刷新） */
            last_mode = agg;
            force_relayout = true;
        }

        ai_card_info_t cards[3];
        int n = ai_sessions_top(cards, 3);
        int total = ai_sessions_count();
        if (n != last_n) {
            last_n = n;
            force_relayout = true;      /* 卡片增删立即重排 */
        }

        /* 刷新策略：每 500ms 全量重画卡片（时长/冒号跳秒），
         * 中间的 50ms 动画帧只刷各卡自己的色条 + 头部状态点 */
        bool relayout = force_relayout || (now - last_layout_ms >= 500);
        if (relayout) {
            force_relayout = false;
            last_layout_ms = now;
            for (int i = 0; i < n; i++) {
                uint16_t c = card_anim_color(&cards[i], now);
                draw_card(i, &cards[i], now, (int32_t)c);
                last_card_anim[i] = c;
            }
            for (int i = n; i < 3; i++) {
                int x = CARD_X0 + i * (CARD_W + CARD_GAP);
                fill_rect(x, CARD_Y0, x + CARD_W - 1, CARD_Y0 + CARD_H - 1, COL_CARD);
                flush_region(x, CARD_Y0, x + CARD_W - 1, CARD_Y0 + CARD_H - 1);
                last_card_anim[i] = 0xFFFF;
            }
            draw_header(agg, total, n, now);
            last_header_anim = mode_anim_color(agg, mode_color(agg),
                                               blend565(mode_color(agg), COL_CARD, 140), now);
        } else {
            /* 每帧动画：每张卡各自的色条（working 呼吸 / 审批频闪） */
            for (int i = 0; i < n; i++) {
                uint16_t c = card_anim_color(&cards[i], now);
                if (c != last_card_anim[i]) {
                    int bx = CARD_X0 + i * (CARD_W + CARD_GAP);
                    fill_rect(bx, CARD_Y0, bx + CARD_W - 1, CARD_Y0 + 2, c);
                    flush_region(bx, CARD_Y0, bx + CARD_W - 1, CARD_Y0 + 2);
                    last_card_anim[i] = c;
                }
            }
            /* 头部状态点：只刷 13x13 小区域，不重绘文字 */
            uint16_t hc = mode_anim_color(agg, mode_color(agg),
                                          blend565(mode_color(agg), COL_CARD, 140), now);
            if (hc != last_header_anim) {
                fill_circle(11, 9, 6, hc);
                flush_region(5, 3, 17, 15);
                last_header_anim = hc;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }
}

void app_display_init(void)
{
    s_queue = xQueueCreate(10, sizeof(ai_event_msg_t));
    xTaskCreate(display_task, "display", 6144, NULL, 4, NULL);
}

QueueHandle_t app_display_queue(void)
{
    return s_queue;
}

ai_state_t app_display_current_state(void)
{
    switch (ai_sessions_aggregate()) {
    case LAMP_RED_FLASH:
    case LAMP_YELLOW_FLASH:    return AI_STATE_ERROR;
    case LAMP_YELLOW_BREATH:
    case LAMP_YELLOW_STEADY:   return AI_STATE_WORKING;
    case LAMP_GREEN_FLASH:
    case LAMP_GREEN_STEADY:    return AI_STATE_DONE;
    default:                   return AI_STATE_IDLE;
    }
}
