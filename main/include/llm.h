/*
 * ─────────────────────────────────────────────────────────────
 * FemtoClaw : LLM session management and chat.
 *
 * Depends on: http.h, config.h, json.h, board_parser.h
 * ─────────────────────────────────────────────────────────────
 */

#pragma once

/*
 * k_sys_prompt lives in .rodata (flash) on both ESP32 and Pico W — it costs
 * zero RAM at runtime.  It contains everything up to "## Board Configuration\n";
 * the board_md content is appended immediately after in llm_chat().
 */
static const char k_sys_prompt[] =
    "You are FemtoClaw, an AI assistant running on a microcontroller.\n"
    "You can hold normal conversations AND control real hardware.\n\n"

    "## Conversation Behaviour\n"
    "  \xE2\x80\xA2 Respond naturally to greetings, questions, and general topics.\n"
    "  \xE2\x80\xA2 On the very first message (hi / hello / start / hey), greet the user warmly,\n"
    "    introduce yourself briefly, and only if no board config is loaded gently\n"
    "    mention: 'If you\\'d like me to control hardware, please upload your board .md file.'\n"
    "  \xE2\x80\xA2 Do NOT mention hardware, actions, or the board config unless the user brings it up\n"
    "    or a board config is already loaded.\n"
    "  \xE2\x80\xA2 Answer general knowledge questions, help with reasoning, writing, math, etc.\n\n"

    "## Hardware Control\n"
    "When the user asks to control hardware you MUST ALWAYS embed the appropriate\n"
    "[ACTION:...] tag in your reply no exceptions, even if you think the hardware\n"
    "is already in the requested state. Skipping the action tag is never allowed.\n"
    "For gpio_set: value=1 means ON/HIGH, value=0 means OFF/LOW (logical values).\n"
    "The firmware handles any hardware-level inversion, you always use logical values.\n"
    "Never emit action tags during normal conversation.\n\n"

    "Available actions:\n"
    "  [ACTION:gpio_set     pin=<n>   value=<0|1>]\n"
    "  [ACTION:gpio_get     pin=<n>]\n"
    "  [ACTION:adc_read     pin=<n>]\n"
    "  [ACTION:serial_write port=<n>  data=<msg>]\n"
    "  [ACTION:serial_read  port=<n>]\n"
    "  [ACTION:delay_ms     ms=<n>]\n"
    "  [ACTION:servo_set    name=<n>  angle=<0-180>]\n"
    "  [ACTION:pwm_set      name=<n>  duty=<0-255>]\n"
    "  [ACTION:oled_print   bus=<n>   text=<msg> x=<n> y=<n>]\n"
    "  [ACTION:oled_clear   bus=<n>]\n"
    "  [ACTION:tft_print    bus=<n>   text=<msg> x=<n> y=<n> color=<hex>]\n"
    "  [ACTION:i2c_write    bus=<n>   reg=<hex>  data=<hex>]\n"
    "  [ACTION:i2c_read     bus=<n>   reg=<hex>  len=<n>]\n\n"

    "Action results come back as [RESULT:...] in the conversation.\n\n"

    "## Action Rules (only apply when executing hardware tasks)\n"
    "  \xE2\x80\xA2 Always refer to pins and buses by NAME from the board config below.\n"
    "  \xE2\x80\xA2 Never guess a pin name not listed in the board config.\n"
    "  \xE2\x80\xA2 If the user requests a hardware action but no board config is loaded,\n"
    "    reply: 'I need your board config to do that please upload your .md file.'\n"
    "  \xE2\x80\xA2 Clamp servo angles to the declared Min\xE2\x80\x93Max range.\n"
    "  \xE2\x80\xA2 PWM duty: 0 = off, 255 = full power.\n\n"

    "## Board Configuration\n";

// ─── Session (conversation history) ──────────────────────────────────────────
/*
 * Packed format: role \x01 content \x02 ... repeated.
 * session_append evicts the oldest message when the buffer is too full.
 */
static char     g_session[SESSION_S];
static uint16_t g_session_len = 0;

static void session_append(const char *role, const char *content) {
    uint16_t rlen = strlen(role), clen = strlen(content);
    uint16_t need = rlen + 1 + clen + 1;
    while (g_session_len + need >= SESSION_S && g_session_len > 0) {
        const char *nx = strchr(g_session, '\x02');
        if (!nx) { g_session_len = 0; g_session[0] = '\0'; break; }
        ++nx;
        uint16_t drop = (uint16_t)(nx - g_session);
        memmove(g_session, nx, g_session_len - drop + 1);
        g_session_len -= drop;
    }
    memcpy(g_session + g_session_len, role, rlen);  g_session_len += rlen;
    g_session[g_session_len++] = '\x01';
    memcpy(g_session + g_session_len, content, clen); g_session_len += clen;
    g_session[g_session_len++] = '\x02';
    g_session[g_session_len]   = '\0';
}

static void session_clear() { g_session_len = 0; g_session[0] = '\0'; }

// ─── llm_chat ─────────────────────────────────────────────────────────────────
static bool llm_chat(const char *user_prompt, char *out, uint16_t out_cap) {
    uint16_t pos = 0;

    // ── JSON envelope header ────────────────────────────────────────────────
    pos += snprintf(g_tx_body + pos, JSON_OUT_S - pos,
        "{\"model\":\"%s\",\"max_tokens\":%u,\"temperature\":%.2f,"
        "\"stream\":false,\"messages\":[",
        g_cfg.llm_model, g_cfg.max_tokens, (double)g_cfg.temperature);

    // ── System message — direct write, zero intermediate buffers ───────────
    //
    // Pattern: snprintf opens the JSON string literal, json_escape_into()
    // writes content byte-by-byte returning *actual* bytes written (never a
    // would-be count), snprintf closes it.
    //
    pos += snprintf(g_tx_body + pos, JSON_OUT_S - pos,
        "{\"role\":\"system\",\"content\":\"");
    pos += json_escape_into(g_tx_body + pos, JSON_OUT_S - pos, k_sys_prompt);
    const char *board_section = g_cfg.board_md_loaded
        ? g_cfg.board_md : "(No board configuration loaded yet.)";
    pos += json_escape_into(g_tx_body + pos, JSON_OUT_S - pos, board_section);
    pos += snprintf(g_tx_body + pos, JSON_OUT_S - pos, "\"}");

    // ── Session history ─────────────────────────────────────────────────────
    //
    // Guard: stop appending session entries when fewer than 64 bytes remain.
    // This leaves room for the closing user message + "]}".
    // json_escape_n_into() handles content delimited by \x02, not '\0'.
    //
    bool first = false;
    const char *p = g_session;
    while (*p && pos + 64 < JSON_OUT_S) {
        const char *re = strchr(p, '\x01'); if (!re) break;
        char role[12] = {0}; memcpy(role, p, min((ptrdiff_t)11, re - p)); p = re + 1;
        const char *ce = strchr(p, '\x02');
        uint16_t cl = ce ? (uint16_t)(ce - p) : (uint16_t)strlen(p);
        pos += snprintf(g_tx_body + pos, JSON_OUT_S - pos,
            "%s{\"role\":\"%s\",\"content\":\"", first ? "" : ",", role);
        pos += json_escape_n_into(g_tx_body + pos, JSON_OUT_S - pos, p, cl);
        pos += snprintf(g_tx_body + pos, JSON_OUT_S - pos, "\"}");
        first = false;
        p = ce ? ce + 1 : p + cl;
    }

    // ── User message ────────────────────────────────────────────────────────
    pos += snprintf(g_tx_body + pos, JSON_OUT_S - pos,
        "%s{\"role\":\"user\",\"content\":\"", first ? "" : ",");
    pos += json_escape_into(g_tx_body + pos, JSON_OUT_S - pos, user_prompt);
    pos += snprintf(g_tx_body + pos, JSON_OUT_S - pos, "\"}]}");

    // ── Overflow guard ──────────────────────────────────────────────────────
    //
    // pos >= JSON_OUT_S  : snprintf's return value pushed pos past the end
    //                      (can only happen for the tiny structural strings
    //                       if the buffer was already essentially full,
    //                       json_escape_into returns actual bytes, not would-be).
    // last byte != '\0'  : belt-and-suspenders buffer was completely filled.
    //
    if (pos >= JSON_OUT_S || g_tx_body[JSON_OUT_S - 1] != '\0') {
        session_clear();
        snprintf(out, out_cap, "[session overflow — cleared, retry]");
        return false;
    }

    char host[CFG_S];
    snprintf(g_tx_auth, sizeof(g_tx_auth), "Authorization: Bearer %s\r\n", g_cfg.llm_api_key);
    const char *hs = strstr(g_cfg.llm_api_base, "://");
    hs = hs ? hs + 3 : g_cfg.llm_api_base;
    const char *ps = strchr(hs, '/');
    if (ps) {
        uint16_t hl = (uint16_t)(ps - hs);
        memcpy(host, hs, hl); host[hl] = '\0';
        snprintf(g_tx_path, CFG_S, "%s/chat/completions", ps);
    } else {
        strlcpy(host, hs, CFG_S);
        strlcpy(g_tx_path, "/chat/completions", CFG_S);
    }

#ifdef BOARD_ESP32
    Serial.printf("[LLM] tx=%u B  free_heap=%lu B\r\n",
                  (unsigned)pos, (unsigned long)ESP.getFreeHeap());
    if (ESP.getFreeHeap() < 120000) {
        Serial.println("[WARN] Heap critically low — rebooting to prevent crash");
        delay(200);
        ESP.restart();
    }
#elif defined(BOARD_PICO_W)
    Serial.printf("[LLM] tx=%u B  free_heap=%lu B\r\n",
                  (unsigned)pos, (unsigned long)rp2040.getFreeHeap());
    if (rp2040.getFreeHeap() < 120000) {
        Serial.println("[WARN] Heap critically low — rebooting to prevent crash");
        delay(200);
        rp2040.reboot();
    }
#endif

    g_http_busy = true;
    int16_t code;
    if (strncmp(g_cfg.llm_api_base, "http://", 7) == 0)
        code = http_req(host, g_tx_path, g_tx_auth, g_tx_body, pos, g_http_resp, HTTP_RESP_S);
    else
        code = https_req(g_tls_llm, host, g_tx_path, g_tx_auth, g_tx_body, pos, g_http_resp, HTTP_RESP_S);
    g_http_busy = false;

    if (code != 200) {
        snprintf(out, out_cap, "[LLM %d] %.200s", code, g_http_resp);
        return false;
    }

    char *json_start = g_http_resp;
    if (json_start[0] != '{') {
        char *brace = strchr(g_http_resp, '{');
        if (brace) {
            json_start = brace;
        } else {
            snprintf(out, out_cap, "[parse:no-json] %.120s", g_http_resp);
            Serial.printf("[LLM] parse fail — no JSON: %.200s\r\n", g_http_resp);
            return false;
        }
    }

    const char *ch = strstr(json_start, "\"choices\"");
    if (!ch) { snprintf(out, out_cap, "[parse:choices] %.120s", json_start); return false; }
    const char *mc = strstr(ch,  "\"message\"");
    if (!mc) { snprintf(out, out_cap, "[parse:message] %.120s", json_start); return false; }
    const char *cc = strstr(mc,  "\"content\"");
    if (!cc) { snprintf(out, out_cap, "[parse:content] %.120s", json_start); return false; }

    const char *buf_end = g_http_resp + HTTP_RESP_S;
    const char *vv = cc + strlen("\"content\"");
    while (*vv == ' ' || *vv == ':') ++vv;
    jstr(vv, out, out_cap, buf_end);

    // Fallback for thinking models
    if (out[0] == '\0') {
        const char *rc = strstr(mc, "\"reasoning_content\"");
        if (!rc) rc = strstr(mc, "\"reasoning\"");
        if (rc) {
            const char *rv = rc + (strncmp(rc, "\"reasoning_content\"", 19) == 0
                                   ? strlen("\"reasoning_content\"")
                                   : strlen("\"reasoning\""));
            while (*rv == ' ' || *rv == ':') ++rv;
            jstr(rv, out, out_cap, buf_end);
            Serial.println("[LLM] used reasoning field (thinking model)");
        }
    }
    if (out[0] == '\0') strlcpy(out, "[model returned empty response]", out_cap);
    return true;
}