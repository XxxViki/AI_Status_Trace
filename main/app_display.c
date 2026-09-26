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
#include "driver/gpio.h"
#include "ST7789.h"
#include "ai_state.h"
#include "ai_sessions.h"
#include "app_wifi.h"
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

#define FRAME_MS    33     /* 30fps: 动画更连贯(#064) */
#define BREATH_MS   1400     /* #064: 放慢呼吸更自然 */
#define FLASH_MS    400      /* #064: 频闪放缓 */

#define CARD_W      101
#define CARD_H      136
#define CARD_X0     4
#define CARD_Y0     32
#define CARD_GAP    4

static uint16_t s_frame[PH * PW];          /* 物理竖屏全帧（.bss ~115KB） */
static uint16_t s_flush_buf[LW * 70];      /* 局部刷新过渡(#047: 宽卡 136x160=21760 需70行) */
static QueueHandle_t s_queue;

/* ---------- 坐标旋转层：逻辑 -> 物理 ---------- */

static int s_clip_x0 = 0, s_clip_x1 = LW - 1;   /* 跑马灯水平裁剪窗口(#038) */

static void px(int lx, int ly, uint16_t c)
{
    if (lx < s_clip_x0 || lx > s_clip_x1 || lx < 0 || lx >= LW || ly < 0 || ly >= LH) {
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
        /* #019 的教训：保护分支不许静默跳过——布局改大后必须看得见告警 */
        ESP_LOGW(TAG, "flush 区域超限被跳过: %dx%d (缓冲 %d)", w, h,
                 (int)(sizeof(s_flush_buf) / sizeof(uint16_t)));
        return;
    }
    /* #051: draw_bitmap 异步排队——复用共享缓冲前必须等上一次传输完成，
     * 否则屏幕出现"重复/残缺"（单卡两分片背靠背时必现） */
    LCD_WaitFlushDone();
    for (int r = 0; r < h; r++) {
        memcpy(&s_flush_buf[r * w], &s_frame[(phys_y0 + r) * PW + phys_x0],
               w * sizeof(uint16_t));
    }
    esp_lcd_panel_draw_bitmap(panel_handle, phys_x0, phys_y0,
                              phys_x1 + 1, phys_y1 + 1, s_flush_buf);
}

/* 分段刷新（#049）：逻辑宽超过 160px 的区域自动切片——单卡铺满后转置区达
 * 136x312=42432 超过渡缓冲；每片 <=160px 逻辑宽(转置 136x160=21760)可安全发送 */
static void flush_region_auto(int x0, int y0, int x1, int y1)
{
    const int MAXW = 160;
    for (int x = x0; x <= x1; x += MAXW) {
        int xe = x + MAXW - 1;
        if (xe > x1) {
            xe = x1;
        }
        flush_region(x, y0, xe, y1);
    }
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
            int sx = (int)(cx + dx * r), sy = (int)(cy + dy * r);
            fill_rect(sx, sy, sx + 1, sy + 1, COL_CLAUDE);   /* 2x2 加粗(#039) */
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
        fill_rect(ix, iy, ix + 1, iy + 1, c);   /* 2x2 加粗(#039) */
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
        int level = 81 + (int)(19.0f * cosf(2.0f * 3.1415926f * phase100 / 100.0f));  /* 62..100 更柔和 */
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

/* ---------- ×1.5 字号渲染（#045）：像素块缩放，5x7 字形 -> 8x11 ----------
 * 整数缩放只能在 ×1(6px) 和 ×2(12px) 之间跳；×1.5 用"每源像素占 3/2 目标像素"
 * 实现半档：字形 8x11、步进 10px —— 比 ×2 小一点，还能多放字符。 */
static void draw_char_15(int x, int y, char c, uint16_t color)
{
    const uint8_t *g = k_font5x7[(int)(unsigned char)c];
    for (int r = 0; r < FONT5X7_H; r++) {
        int dy0 = (r * 3) / 2, dy1 = ((r + 1) * 3) / 2 - 1;
        for (int col = 0; col < FONT5X7_W; col++) {
            if (g[r] & (0x10 >> col)) {
                int dx0 = (col * 3) / 2, dx1 = ((col + 1) * 3) / 2 - 1;
                fill_rect(x + dx0, y + dy0, x + dx1, y + dy1, color);
            }
        }
    }
}

static void draw_text_15(int x, int y, const char *s, uint16_t color)
{
    for (const char *p = s; *p; p++, x += 10) {
        draw_char_15(x, y, (char)toupper((unsigned char)*p), color);
    }
}

/* ---------- 项目名：×1.5 字号（#045 定稿） ----------
 * 用户要求：所有卡片名字一样大（此前双档字号导致大小不一）。
 * 用户反馈：不用负片芯片、字号小一点 → ×1.5（8x11 字形，步进 10px），
 * 容量 9 字符；超长截断 7 字符 + ".."；居中显示。 */
#define NAME_MARGIN  3
#define NAME_Y_OFF   90
#define NAME_H       14

static void draw_card_name(int idx, int card_x, int cw, const ai_card_info_t *info,
                           bool advance)
{
    (void)idx;
    (void)advance;
    const char *name = info->project[0] ? info->project
                       : (info->tool[0] ? info->tool : "SESSION");
    int x0 = card_x + NAME_MARGIN;
    int name_y = CARD_Y0 + NAME_Y_OFF;
    int usable = cw - 2 * NAME_MARGIN;
    fill_rect(card_x + 1, name_y, card_x + cw - 2, name_y + NAME_H - 1, COL_CARD_BG);

    int len = (int)strlen(name);
    if (len == 0) {
        return;
    }

    /* ×1.5 字号（#045 用户要求：不用负片、字号小一点）：
     * 容量 9 字符；超出截断 7 字符 + ".."；整体居中 */
    const int ADV = 10;
    int k = len;
    bool truncated = false;
    if (k * ADV - 2 > usable) {
        truncated = true;
        k = 7;
        if (len < k) {
            k = len;
            truncated = false;
        }
    }

    char buf[32];
    int n = 0;
    for (int i = 0; i < k && name[i]; i++) {
        buf[n++] = (char)toupper((unsigned char)name[i]);
    }
    buf[n] = '\0';
    int total = n * ADV - 2 + (truncated ? 2 * ADV - 2 : 0);
    int tx = x0 + (usable - total) / 2;
    draw_text_15(tx, name_y + 2, buf, COL_TXT);
    if (truncated) {
        draw_text_15(tx + n * ADV, name_y + 2, "..", COL_TXT);
    }
}

/* ---------- 卡位均分布局（#046）：按实际会话数均分可用宽度 ----------
 * 1 张 160px 居中 / 2 张各 154px / 3 张各 101px；不再显示空占位盒。 */
static int s_slot_x[3];
static int s_slot_w = CARD_W;

static void layout_slots(int n)
{
    if (n <= 0) {
        s_slot_w = CARD_W;
        return;
    }
    const int margin = 4, gap = 4;
    int w = (LW - 2 * margin - (n - 1) * gap) / n;   /* #049: 单卡铺满,不再限宽 */
    int total = n * w + (n - 1) * gap;
    int x0 = (LW - total) / 2;
    for (int i = 0; i < n && i < 3; i++) {
        s_slot_x[i] = x0 + i * (w + gap);
    }
    s_slot_w = w;
}

/* 重画一张卡：徽章/时长/状态词/项目名 + 顶部状态色条 */
static void draw_card(int idx, const ai_card_info_t *info, int64_t now,
                      int32_t anim_color, int x, int cw)
                      /* int32_t: RGB565 颜色 >=0x8000,不能用 int16 哨兵 */
{
    int y = CARD_Y0;

    fill_rect(x, y, x + cw - 1, y + CARD_H - 1, COL_CARD);      /* 外框 */
    fill_rect(x + 1, y + 3, x + cw - 2, y + CARD_H - 2, COL_CARD_BG);

    uint16_t bar = (anim_color >= 0) ? (uint16_t)anim_color : mode_color(info->lamp);
    fill_rect(x, y, x + cw - 1, y + 2, bar);

    draw_logo(info->tool, x + 7, y + 6);
    draw_duration(x + cw - 7, y + 6, info->started_ms, now);   /* 会话总时长 */

    /* token 用量（Claude 专属）：logo 右侧、时间下方（#039 布局优化） */
    if (info->tokens > 0) {
        char tb[16];
        if (info->tokens >= 1000) {
            snprintf(tb, sizeof(tb), "%d.%dK", (int)(info->tokens / 1000),
                     (int)((info->tokens % 1000) / 100));
        } else {
            snprintf(tb, sizeof(tb), "%d", (int)info->tokens);
        }
        int tw = 7 + 2 + text_w(tb, 1);           /* 箭头7px + 间距 + 文本 */
        int tx = x + cw - 7 - tw;                 /* 右对齐，与时间同列 */
        int ay = y + 29;   /* #045 与时长拉开距离 */
        fill_rect(tx + 2, ay, tx + 2, ay + 3, COL_TXT_DIM);       /* 箭杆 */
        fill_rect(tx, ay + 4, tx + 4, ay + 4, COL_TXT_DIM);       /* 箭头横杠 */
        fill_rect(tx + 1, ay + 5, tx + 3, ay + 5, COL_TXT_DIM);   /* 收窄 */
        fill_rect(tx + 2, ay + 6, tx + 2, ay + 6, COL_TXT_DIM);   /* 箭尖朝下 */
        draw_text(tx + 9, ay, tb, 1, COL_TXT_DIM);
    }

    draw_text_centered(x + cw / 2, y + 44, mode_word(info->lamp), 2,
                       mode_color(info->lamp));

    /* 当前状态持续时长（think/等待/卡住各计各的） */
    draw_state_age(x + cw / 2, y + 70, info, now);

    draw_card_name(idx, x, cw, info, false);   /* ×1.5 字号(#045) */

    /* 最近调用的工具（BA/ED/WR/AG…），填充底部空间且有用 */
    if (info->last_tool[0]) {
        draw_text_centered(x + cw / 2, y + 112, info->last_tool, 1, COL_TXT_DIM);
    }

    flush_region_auto(x, y, x + cw - 1, y + CARD_H - 1);   /* #048/#049: 用 cw + 自动分段 */
}

/* ---------- 头部：会话数 + 全员 mini-logo 彩带（#035） ----------
 *
 * 设计（与用户讨论定稿）：
 *   左：会话数 + 页码（多页时）
 *   右：mini-logo 彩带，每格一个会话(按优先级序)：
 *       - 底色 = 状态色（兼作远观信号），字形 = 工具 logo（一眼知道是哪个任务）
 *       - 第 1 格(最高优先级)底色参与动画
 *       - 当前页内的会话全亮度，折叠的会话暗 45%（颜色仍可辨状态）
 *   BOOT 键(GPIO9)翻页 = 其余会话也能上卡片看清
 */
#define MAX_RIBBON  8      /* 与会话表上限一致 */
#define HEADER_H    28     /* 头部高度(#039 再加高) */
#define TILE        24     /* mini-logo 格边长(#039 再放大) */
#define TILE_GAP    2
#define TILE_TOP    2     /* (HEADER_H-TILE)/2 */
#define TILE_RIGHT  6

/* 第 i 格的 x 起点，n=总格数。全/快路径共用保证几何一致 */
static int tile_x(int n, int i)
{
    int total = n * (TILE + TILE_GAP) - TILE_GAP;
    return LW - TILE_RIGHT - total + i * (TILE + TILE_GAP);
}

/* 底色亮度粗判 -> 字形用深色还是亮色（用状态"亮色"判定，避免呼吸时闪烁） */
static uint16_t glyph_contrast(uint16_t bg)
{
    int r = (bg >> 11) & 0x1F, g = (bg >> 5) & 0x3F, b = bg & 0x1F;
    int lum = r * 2 + g * 3 + b;
    return (lum > 150) ? COL_BG : RGB565(0xf2, 0xf2, 0xf2);
}

/* 16x16 迷你 logo 字形 */
static void draw_mini_glyph(const char *tool, int x, int y, uint16_t fg)
{
    int cx = x + TILE / 2, cy = y + TILE / 2;
    if (strcmp(tool, "claude") == 0) {
        fill_circle(cx, cy, 1, fg);
        for (int i = 0; i < 8; i++) {
            float a = i * 3.1415926f / 4.0f;
            for (float r = 3.5f; r <= 10.0f; r += 0.5f) {
                int sx = (int)(cx + cosf(a) * r), sy = (int)(cy + sinf(a) * r);
                fill_rect(sx, sy, sx + 1, sy + 1, fg);   /* 2x2 加粗(#039) */
            }
        }
    } else if (strcmp(tool, "codex") == 0) {
        draw_ring6(cx, cy, 8, fg);
    } else if (strcmp(tool, "trae") == 0) {
        /* 矩形外框 + 中间两个方块（真标志的迷你版） */
        fill_rect(x + 2, y + 2, x + TILE - 3, y + 3, fg);
        fill_rect(x + 2, y + TILE - 4, x + TILE - 3, y + TILE - 3, fg);
        fill_rect(x + 2, y + 2, x + 3, y + TILE - 3, fg);
        fill_rect(x + TILE - 4, y + 2, x + TILE - 3, y + TILE - 3, fg);
        fill_rect(cx - 5, cy - 2, cx - 2, cy + 1, fg);
        fill_rect(cx + 2, cy - 2, cx + 5, cy + 1, fg);
    } else {
        char c = (tool[0] == '\0') ? '?' : (char)toupper((unsigned char)tool[0]);
        char s[2] = { c, '\0' };
        /* ×3 字号(15x21)填满 24px 格子 + 错位二次描画加粗
         * （#039 用户反馈"又细又小"——此前一直是 ×1 的 5x7 像素） */
        int tx = cx - text_w(s, 3) / 2;
        draw_text(tx, cy - 10, s, 3, fg);
        draw_text(tx + 1, cy - 9, s, 3, fg);
    }
}

/* 工具官方品牌色（#040 用户要求 logo 颜色与官方一致） */
static uint16_t brand_color(const char *tool)
{
    if (strcmp(tool, "claude") == 0) {
        return COL_CLAUDE;                       /* Anthropic 珊瑚橘 */
    }
    if (strcmp(tool, "zcode") == 0) {
        return COL_ZCODE;                        /* ZCode 蓝 */
    }
    if (strcmp(tool, "trae") == 0) {
        return RGB565(0x00, 0xb9, 0xa6);         /* Trae 青绿 */
    }
    if (strcmp(tool, "chatgpt") == 0) {
        return RGB565(0x10, 0xa3, 0x7f);         /* OpenAI 绿 */
    }
    if (strcmp(tool, "hermes") == 0) {
        return RGB565(0x8b, 0x5c, 0xf6);         /* Hermes 紫 */
    }
    return 0;                                    /* 0 = 无品牌色(单色品牌/未知),自适应 */
}

/* 画一格：底色=状态色(可选动画)，字形=官方品牌色 + 自适应对比阴影
 * （品牌色压在状态色底上可能对比不足，如珊瑚橘压红底——先画 1px 自适应色
 *   阴影再看品牌色，等价于描边，保证任意底色可辨） */
static void tile_glyph(const ai_card_info_t *info, int x, uint16_t bg_on)
{
    uint16_t brand = brand_color(info->tool);
    uint16_t edge = glyph_contrast(bg_on);
    if (brand == 0) {
        draw_mini_glyph(info->tool, x, TILE_TOP, edge);
        return;
    }
    draw_mini_glyph(info->tool, x + 1, TILE_TOP + 1, edge);   /* 阴影/描边 */
    draw_mini_glyph(info->tool, x, TILE_TOP, brand);          /* 品牌色 */
}

static void draw_tile(const ai_card_info_t *info, int x, bool on_page,
                      int64_t now, bool animate)
{
    uint16_t on = mode_color(info->lamp);
    uint16_t bg;
    if (animate) {
        bg = mode_anim_color(info->lamp, on, blend565(on, COL_CARD, 140), now);
    } else if (on_page) {
        bg = on;
    } else {
        bg = blend565(on, COL_CARD, 115);     /* 折叠：暗一档 */
    }
    fill_rect(x, TILE_TOP, x + TILE - 1, TILE_TOP + TILE - 1, bg);
    tile_glyph(info, x, on);
}

/* 重绘头部整条（relayout 时调用） */
static void draw_header(const ai_card_info_t *all, int n, int shown,
                        int page, int pages, int64_t now)
{
    fill_rect(0, 0, LW - 1, HEADER_H - 1, COL_CARD);

    char buf[48];
    if (pages > 1) {
        /* #053: "4 SESSIONS 1/2" 会被误读成"4个页面"——改成 P of M 更明确 */
        snprintf(buf, sizeof(buf), "%d SESSIONS   P%d OF %d", n, page + 1, pages);
    } else {
        snprintf(buf, sizeof(buf), "%d SESSION%s", n, n == 1 ? "" : "S");
    }
    /* 文字在"格子左侧区域"内居中（#039 用户要求居中显示） */
    int tiles_start = (n > 0) ? tile_x(n, 0) : LW - TILE_RIGHT;
    int tw = text_w(buf, 1);
    int tx = (tiles_start - tw) / 2;
    if (tx < 4) {
        tx = 4;
    }
    draw_text(tx, (HEADER_H - 7) / 2, buf, 1, COL_TXT_DIM);

    int start = page * 3;
    for (int i = 0; i < n && i < MAX_RIBBON; i++) {
        bool on_page = (i >= start && i < start + shown);
        draw_tile(&all[i], tile_x(n, i), on_page, now, i == 0);
    }
    flush_region(0, 0, LW - 1, HEADER_H - 1);
}

/* 每张卡的色条按"自己的状态"做动画（共享时钟 = 同相位同步呼吸，视觉统一）*/
static uint16_t card_anim_color(const ai_card_info_t *info, int64_t now)
{
    uint16_t on = mode_color(info->lamp);
    uint16_t off = blend565(on, COL_CARD, 140);
    return mode_anim_color(info->lamp, on, off, now);
}

/* ---------- 主任务 ---------- */

#define BOOT_BTN_GPIO     GPIO_NUM_9   /* 厂商确认：BOOT 键=GPIO9，低电平有效 */

static volatile int s_page_req = -1;   /* /dbg/page 用的请求式翻页 */

void app_display_set_page(int page)
{
    s_page_req = page;
}
#define PAGE_SIZE         3
#define PAGE_HOME_MS      20000        /* 手动翻页后 20 秒自动回第 1 页 */
#define BTN_DEBOUNCE_MS   150

static void display_task(void *arg)
{
    (void)arg;
    /* BOOT 键：翻页用（看被折叠的其余会话） */
    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << BOOT_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    /* 底图（#046：不再画空占位盒，卡位按会话数动态均分） */
    for (int i = 0; i < PH * PW; i++) {
        s_frame[i] = COL_BG;
    }
    flush_all();

    ai_event_msg_t msg;
    int64_t last_layout_ms = -10000;
    uint16_t last_card_anim[3] = { 0xFFFF, 0xFFFF, 0xFFFF };
    uint16_t last_tile0_anim = 0xFFFF;
    lamp_mode_t last_mode = LAMP_OFF;
    int last_n = -1;
    int last_shown = -1;
    int page = 0;
    int64_t page_set_ms = 0;
    bool btn_prev = false;
    int64_t btn_last_ms = 0;
    int64_t btn_press_start = 0;
    bool btn_long_fired = false;
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

        /* 导出全部会话（按优先级序）：当前页 3 张上卡片，其余进顶部 mini-logo 带 */
        ai_card_info_t all[8];
        int n = ai_sessions_top(all, 8);
        int pages = (n + PAGE_SIZE - 1) / PAGE_SIZE;
        if (pages < 1) {
            pages = 1;
        }
        if (n != last_n) {
            last_n = n;
            page = 0;                   /* 会话增减：回到第 1 页 */
            force_relayout = true;
        }
        if (page >= pages) {
            page = 0;                   /* 页数缩水：回第 1 页 */
            force_relayout = true;
        }

        /* 调试端点请求的翻页 */
        if (s_page_req >= 0) {
            page = s_page_req % pages;
            s_page_req = -1;
            page_set_ms = now;
            force_relayout = true;
        }

        /* BOOT 长按 3 秒 → 进配网模式（#067）：
         * 屏幕显示"配网模式 AP:AI-Status-Setup", 重启后变热点 */
        bool btn = (gpio_get_level(BOOT_BTN_GPIO) == 0);
        if (btn) {
            if (btn_press_start == 0) {
                btn_press_start = now;
            } else if (now - btn_press_start > 3000 && !btn_long_fired) {
                btn_long_fired = true;
                ESP_LOGW(TAG, "BOOT 长按 3 秒 -> 进入配网模式");
                /* 屏幕显示配网状态(帧缓冲直接画, 不走 LVGL) */
                for (int i = 0; i < PH * PW; i++) {
                    s_frame[i] = COL_BG;
                }
                /* 逻辑坐标: 居中大字 SETUP MODE + AP 名 + IP */
                s_clip_x0 = 0; s_clip_x1 = LW - 1;
                fill_rect(0, 30, LW - 1, 60, COL_YEL);
                draw_text_centered(LW / 2, 36, "SETUP MODE", 2, COL_BG);
                draw_text_centered(LW / 2, 80, "AP: AI-Status-Setup", 1, COL_TXT);
                draw_text_centered(LW / 2, 100, "pass: 12345678", 1, COL_TXT);
                draw_text_centered(LW / 2, 120, "http://192.168.4.1", 1, COL_GRN);
                flush_all();
                app_wifi_setup_flag_set();
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }
        } else {
            btn_press_start = 0;
            btn_long_fired = false;
        }
        btn_prev = btn;
        if (btn && !btn_prev && now - btn_last_ms > BTN_DEBOUNCE_MS) {
            btn_last_ms = now;
            page = (page + 1) % pages;
            page_set_ms = now;
            force_relayout = true;
            ESP_LOGI(TAG, "BOOT键 -> 第 %d/%d 页", page + 1, pages);
        }
        btn_prev = btn;

        /* 手动翻页 20 秒后自动回第 1 页（关键信息不用手动找回） */
        if (page > 0 && now - page_set_ms > PAGE_HOME_MS) {
            page = 0;
            force_relayout = true;
        }

        int start = page * PAGE_SIZE;
        int shown = n - start;
        if (shown > 3) {
            shown = 3;
        }
        if (shown < 0) {
            shown = 0;
        }

        /* 刷新策略：每 500ms 全量重画卡片（时长/冒号跳秒），
         * 中间的 50ms 动画帧只刷各卡色条 + 头部第 1 格 mini-logo */
        int prev_w = s_slot_w;
        int prev_shown = last_shown;
        layout_slots(shown);
        last_shown = shown;

        bool relayout = force_relayout || (now - last_layout_ms >= 500);
        if (relayout) {
            force_relayout = false;
            last_layout_ms = now;
            if (s_slot_w != prev_w || shown != prev_shown) {
                /* 卡位几何变化：清整条卡区（避免旧卡残影）。
                 * #047：整条 320x136 转置后超缓冲，分两条 320x68 刷 */
                int half = CARD_H / 2;
                fill_rect(0, CARD_Y0, LW - 1, CARD_Y0 + half - 1, COL_BG);
                flush_region(0, CARD_Y0, LW - 1, CARD_Y0 + half - 1);
                fill_rect(0, CARD_Y0 + half, LW - 1, CARD_Y0 + CARD_H - 1, COL_BG);
                flush_region(0, CARD_Y0 + half, LW - 1, CARD_Y0 + CARD_H - 1);
            }
            for (int i = 0; i < shown; i++) {
                uint16_t c = card_anim_color(&all[start + i], now);
                draw_card(i, &all[start + i], now, (int32_t)c, s_slot_x[i], s_slot_w);
                last_card_anim[i] = c;
            }
            draw_header(all, n, shown, page, pages, now);
            last_tile0_anim = mode_anim_color(all[0].lamp, mode_color(all[0].lamp),
                                              blend565(mode_color(all[0].lamp), COL_CARD, 140), now);
        } else {
            /* 每帧动画：每张卡各自的色条（working 呼吸 / 审批频闪） */
            for (int i = 0; i < shown; i++) {
                uint16_t c = card_anim_color(&all[start + i], now);
                if (c != last_card_anim[i]) {
                    int bx = s_slot_x[i];
                    fill_rect(bx, CARD_Y0, bx + s_slot_w - 1, CARD_Y0 + 2, c);
                    flush_region(bx, CARD_Y0, bx + s_slot_w - 1, CARD_Y0 + 2);
                    last_card_anim[i] = c;
                }
            }
            /* 头部第 1 格 mini-logo（最高优先级）：底色动画，只刷 16x16 小区域 */
            if (n > 0) {
                uint16_t hc = mode_anim_color(all[0].lamp, mode_color(all[0].lamp),
                                              blend565(mode_color(all[0].lamp), COL_CARD, 140), now);
                if (hc != last_tile0_anim) {
                    int x = tile_x(n, 0);
                    fill_rect(x, TILE_TOP, x + TILE - 1, TILE_TOP + TILE - 1, hc);
                    tile_glyph(&all[0], x, mode_color(all[0].lamp));
                    flush_region(x, TILE_TOP, x + TILE - 1, TILE_TOP + TILE - 1);
                    last_tile0_anim = hc;
                }
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

uint16_t app_display_pixel(int lx, int ly)
{
    if (lx < 0 || lx >= LW || ly < 0 || ly >= LH) {
        return 0;
    }
    return s_frame[lx * PW + (LH - 1 - ly)];
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
