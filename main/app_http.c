/*
 * app_http.c —— HTTP 服务器（阶段 2）
 *
 * 学习点：
 * 1. esp_http_server 的 URI 注册模型：httpd_uri_t {uri, method, handler}。
 *    收到匹配请求时，httpd 在它自己的任务（httpd_txrx）里回调 handler。
 * 2. handler 里"快进快出"：读 body → 解析 JSON → 入队列 → 立刻返回。
 *    耗时的画屏由显示任务异步做，HTTP 任务保持响应能力。
 *    这就是 FreeRTOS 队列解耦生产者/消费者的经典用法。
 * 3. curl 测试：
 *      curl http://<ip>/health
 *      curl -X POST http://<ip>/events -d '{"event_type":"prompt-submit","session_id":"s1"}'
 */
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include "ai_state.h"
#include "app_display.h"
#include "ai_sessions.h"
#include "app_http.h"

static const char *TAG = "http";

#define BODY_MAX_LEN 512   /* HookEvent 报文一般 <200 字节，512 留余量 */

/* GET /health —— 连通性探测，仿 ai-light 桌面版返回 "ok" */
static esp_err_t health_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /state —— 调试用：灯效 + 会话表明细（id前8位/状态/无事件秒数） */
static esp_err_t state_get(httpd_req_t *req)
{
    char body[1024];   /* 8会话 x ~90B = 720B, 512会截断成非法JSON */
    int n = snprintf(body, sizeof(body), "{\"lamp\":\"%s\",\"sessions\":%d,\"heap\":%u,\"table\":",
                     ai_sessions_mode_name(ai_sessions_aggregate()), ai_sessions_count(),
                     (unsigned)esp_get_free_heap_size());
    n += ai_sessions_dump_json(body + n, sizeof(body) - n);
    snprintf(body + n, sizeof(body) - n, "}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* 截断免疫的 JSON 字段提取：找 "key":"value" 直接拷贝（含简单反转义）。
 * 用于 512 字节截断的大 payload——所需字段都在开头，cJSON 整体解析必败(#025) */
static bool json_scan_string(const char *body, const char *key, char *out, int outlen)
{
    char pat[24];
    if (snprintf(pat, sizeof(pat), "\"%s\"", key) >= (int)sizeof(pat)) {
        return false;
    }
    const char *p = strstr(body, pat);
    if (p == NULL) {
        return false;
    }
    p = strchr(p + strlen(pat), ':');
    if (p == NULL) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    int n = 0;
    while (*p && *p != '"' && n < outlen - 1) {
        if (p[0] == '\\' && p[1]) {
            out[n++] = p[1];   /* 反转义 \ -> \ 等 */
            p += 2;
        } else {
            out[n++] = *p++;
        }
    }
    out[n] = '\0';
    return n > 0;
}

/* POST /events —— 核心入口，两种调用方式：
 *   A. curl 直连（hook 注册的就是这个，电脑侧零脚本）：
 *      curl -X POST "http://ip/events?event_type=pre-tool-use" --data-binary @-
 *      stdin 的 Claude 原始 payload 作为 body 透传（内含 session_id）
 *   B. 标准 JSON body（ai-light 协议，调试/兼容用）：
 *      curl -X POST http://ip/events -d '{"event_type":"stop","session_id":"x"}'
 * event_type 解析优先级：URL 参数 > body 的 event_type > body 的 hook_event_name
 */
static esp_err_t events_post(httpd_req_t *req)
{
    /* 局部声明区（查询解析要用 msg_tool） */
    char msg_tool[8] = "";
    char proj[32] = "";
    char msg_toolname[12] = "";
    char sid_param[48] = {0};
    char tool_param[16] = {0};
    int msg_tokens = 0;
    bool sid_is_unknown = true;

    /* 1) 从 URL query 取事件类型 */
    char ev_param[40] = {0};
    char query[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "event_type", ev_param, sizeof(ev_param));
        /* query 值是 URL 编码的，目前事件名只含字母和'-'，无需解码 */

        /* 测试加速：&ts=10 把所有超时判定阈值除以 10（动画周期不变）。
         * 例：心跳丢失 60s→6s、审批升级 2min→12s，用于快速验证灯效转移 */
        char ts_param[8] = {0};
        if (httpd_query_key_value(query, "ts", ts_param, sizeof(ts_param)) == ESP_OK) {
            ai_sessions_set_time_scale(atoi(ts_param));
        }
        /* token 用量：&tok=2700（Claude Stop 专用脚本附带） */
        char tok_param[12] = {0};
        if (httpd_query_key_value(query, "tok", tok_param, sizeof(tok_param)) == ESP_OK) {
            msg_tokens = atoi(tok_param);
        }
        /* sid/tool: 看门狗完整解析后传入的权威值（#033 治本——
         * Stop 类大载荷的 sessionId 可能在512字节截断线之后，板内扫描不到） */
        httpd_query_key_value(query, "sid", sid_param, sizeof(sid_param));
        httpd_query_key_value(query, "tool", tool_param, sizeof(tool_param));
        /* 工具来源：&src=claude / zcode（区分徽章） */
        char src_param[10] = {0};
        if (httpd_query_key_value(query, "src", src_param, sizeof(src_param)) == ESP_OK) {
            strlcpy(msg_tool, src_param, sizeof(msg_tool));
        }
    }

    /* 2) 读 body：可能为空，也可能很大（ZCode/Claude 的真实 payload 带
     * transcript 路径+完整工具参数，轻松超 1KB）。
     * 策略：第一块（<=BODY_MAX_LEN）进缓冲供解析——session_id/hook_event_name
     * 等关键字段都在 JSON 开头；剩余的流式读掉丢弃，保证连接状态干净。
     * 之前直接回 413 拒绝导致 ZCode 事件全军覆没（问题记录 #015）。 */
    char body[BODY_MAX_LEN + 1] = {0};
    char discard[BODY_MAX_LEN];
    {
        size_t left = req->content_len;
        bool first = true;
        while (left > 0) {
            int want = (left > BODY_MAX_LEN) ? BODY_MAX_LEN : (int)left;
            int r = httpd_req_recv(req, first ? body : discard, want);
            if (r <= 0) {
                break;
            }
            if (first) {
                body[r] = '\0';
                first = false;
            }
            left -= r;
        }
    }

    /* 3) 解析：event_type + session_id */
    ai_event_msg_t msg = {0};
    const char *ev_str = ev_param[0] ? ev_param : NULL;
    char sid[40] = "?";

    /* 关键字段用字符串扫描提取，不用 cJSON_Parse——
     * 大 payload(带完整工具参数)会被 512 字节读取截断，截断的 JSON 整体解析必败(#025)，
     * 而所需字段(session_id/cwd 等)都在 payload 开头，扫描对截断免疫 */
    char cwd_scan[96] = "";
    if (body[0] == '{') {
        if (ev_str == NULL) {
            static char ev_tmp[32];   /* 单线程 httpd handler 内使用 */
            if (json_scan_string(body, "event_type", ev_tmp, sizeof(ev_tmp))
                || json_scan_string(body, "hook_event_name", ev_tmp, sizeof(ev_tmp))
                || json_scan_string(body, "hookEventName", ev_tmp, sizeof(ev_tmp))) {
                ev_str = ev_tmp;
            }
        }
        if (sid_is_unknown) {
            char tmp[48];
            if (json_scan_string(body, "session_id", tmp, sizeof(tmp))
                || json_scan_string(body, "sessionId", tmp, sizeof(tmp))) {
                strlcpy(sid, tmp, sizeof(sid));
                sid_is_unknown = false;
            }
        }
        {   /* 最近调用的工具 */
            char tmp[16];
            if (json_scan_string(body, "tool_name", tmp, sizeof(tmp))
                || json_scan_string(body, "toolName", tmp, sizeof(tmp))) {
                strlcpy(msg_toolname, tmp, sizeof(msg_toolname));
            }
        }
        /* 项目名：cwd 的 basename（只留可打印 ASCII） */
        if (json_scan_string(body, "cwd", cwd_scan, sizeof(cwd_scan)) && cwd_scan[0]) {
            const char *base = cwd_scan;
            for (const char *p = cwd_scan; *p; p++) {
                if (*p == '\\' || *p == '/') {
                    base = p + 1;
                }
            }
            int n = 0;
            for (const char *p = base; *p && n < (int)sizeof(proj) - 1; p++) {
                if (*p >= 0x20 && *p < 0x7F) {
                    proj[n++] = *p;
                }
            }
            proj[n] = '\0';
        }
    }

    /* query 传参优先（权威值），body 扫描作兜底 */
    if (sid_param[0]) {
        strlcpy(sid, sid_param, sizeof(sid));
        sid_is_unknown = false;
    }
    if (tool_param[0]) {
        strlcpy(msg_toolname, tool_param, sizeof(msg_toolname));
    }

    msg.event = ai_event_from_str(ev_str);
    msg.state = ai_state_from_event(msg.event);
    strlcpy(msg.session_id, sid, sizeof(msg.session_id));
    if (strcmp(sid, "?") == 0) {
        /* 探针：无主事件（session 解析失败）——记录 body 开头定位字段差异 */
        ESP_LOGW(TAG, "无主事件 ev=%s body=%.100s", ai_event_name(msg.event), body);
    }
    strlcpy(msg.tool, msg_tool, sizeof(msg.tool));
    strlcpy(msg.last_tool, msg_toolname, sizeof(msg.last_tool));
    msg.tokens = msg_tokens;
    strlcpy(msg.project, proj, sizeof(msg.project));

    if (msg.event == AI_EV_UNKNOWN) {
        ESP_LOGW(TAG, "未知事件: %s (body %.60s)", ev_str ? ev_str : "<null>", body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown event_type");
        return ESP_FAIL;
    }

    /* Notification 细分（问题记录 #013）：
     * - 报文含 "permission"（如 "Claude needs your permission to use Bash"）
     *   → 真正需要人介入，红灯
     * - 其余（如 "Claude is waiting for your input" 闲置提醒）→ 视为完成（绿）。
     *   附带的自愈效果：若 Stop 事件丢失导致灯卡黄，闲置提醒最迟 60s 后把灯修复为绿 */
    if (msg.event == AI_EV_NOTIFICATION) {
        /* 报文含 permission -> 要权限(红)；否则视为完成/闲置(绿)。
         * 直接 strstr，同样对截断免疫 */
        if (strstr(body, "permission") != NULL) {
            msg.state = AI_STATE_ERROR;
        } else {
            msg.state = AI_STATE_DONE;
        }
        ESP_LOGI(TAG, "notification 细分 -> %s", ai_state_name(msg.state));
    }

    ESP_LOGI(TAG, "收到事件 %s (session=%s) -> %s",
             ai_event_name(msg.event), msg.session_id, ai_state_name(msg.state));

    xQueueSend(app_display_queue(), &msg, 0);   /* 队列满则丢弃：灯只关心最新状态 */

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* POST /sessions/clear?tool=<claude|zcode>&id=<前缀>
 * 看门狗专用：进程被杀后（OS 不产生任何事件）由主机侧巡检发现，调此接口清卡。
 * id 前缀优先；否则按 tool 清；都缺省=清全部。响应 {"removed":N} */
static esp_err_t sessions_clear_post(httpd_req_t *req)
{
    char query[128] = {0};
    char tool[12] = {0};
    char id_prefix[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "tool", tool, sizeof(tool));
        httpd_query_key_value(query, "id", id_prefix, sizeof(id_prefix));
    }
    int removed = ai_sessions_clear(tool, id_prefix);
    char body[32];
    snprintf(body, sizeof(body), "{\"removed\":%d}", removed);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* POST /sessions/tok?sid=<会话id>&tok=N —— 只更新 token 计数，不改状态。
 * 实时 token 监视器（看门狗）用：转录文件一有新 usage 就推（#043） */
static esp_err_t sessions_tok_post(httpd_req_t *req)
{
    char query[160] = {0};
    char sid[48] = {0};
    char tok_s[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "sid", sid, sizeof(sid));
        httpd_query_key_value(query, "tok", tok_s, sizeof(tok_s));
    }
    int updated = ai_sessions_set_tokens(sid, atoi(tok_s));
    char body[32];
    snprintf(body, sizeof(body), "{\"updated\":%d}", updated);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /dbg/row?y=<逻辑行> —— 导出该行像素(每2px一个RGB565十六进制)。
 * #047 排查"右半屏不显示"用：直接看帧缓冲真相，不再靠猜 */
static esp_err_t dbg_row_get(httpd_req_t *req)
{
    char query[64] = {0};
    char y_s[8] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "y", y_s, sizeof(y_s));
    }
    int ly = atoi(y_s);
    if (ly < 0 || ly >= 172) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad y");
        return ESP_FAIL;
    }
    char step_s[8] = {0};
    int step = 2;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "step", step_s, sizeof(step_s));
    }
    if (step_s[0] == '1') {
        step = 1;    /* 逐像素(OCR 小字用) */
    }
    static char out[320 * 5 + 8];
    int o = 0;
    for (int lx = 0; lx < 320 && o < (int)sizeof(out) - 8; lx += step) {
        o += snprintf(out + o, sizeof(out) - o, "%04X", app_display_pixel(lx, ly));
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /dbg/page?p=N —— 调试用切页（验证多页渲染） */
static esp_err_t dbg_page_get(httpd_req_t *req)
{
    char query[64] = {0};
    char p_s[8] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "p", p_s, sizeof(p_s));
    }
    app_display_set_page(atoi(p_s));
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t app_http_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();

    /* 优雅处理客户端突然断开（curl 被 Ctrl+C 等），避免 httpd 报错刷屏 */
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t health = { .uri = "/health", .method = HTTP_GET, .handler = health_get };
    static const httpd_uri_t state  = { .uri = "/state",  .method = HTTP_GET, .handler = state_get };
    static const httpd_uri_t events = { .uri = "/events", .method = HTTP_POST, .handler = events_post };
    static const httpd_uri_t clear  = { .uri = "/sessions/clear", .method = HTTP_POST, .handler = sessions_clear_post };
    static const httpd_uri_t stok   = { .uri = "/sessions/tok", .method = HTTP_POST, .handler = sessions_tok_post };
    static const httpd_uri_t dbgrow = { .uri = "/dbg/row", .method = HTTP_GET, .handler = dbg_row_get };
    static const httpd_uri_t dbgpage = { .uri = "/dbg/page", .method = HTTP_GET, .handler = dbg_page_get };

    httpd_register_uri_handler(server, &health);
    httpd_register_uri_handler(server, &state);
    httpd_register_uri_handler(server, &events);
    httpd_register_uri_handler(server, &clear);
    httpd_register_uri_handler(server, &stok);
    httpd_register_uri_handler(server, &dbgrow);
    httpd_register_uri_handler(server, &dbgpage);

    ESP_LOGI(TAG, "HTTP 服务已启动，端口 %d", cfg.server_port);
    return ESP_OK;
}
