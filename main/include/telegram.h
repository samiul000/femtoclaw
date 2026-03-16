/*
 * ─────────────────────────────────────────────────────────────
 * FemtoClaw : Telegram long-polling channel.
 *
 * Depends on: http.h, agent.h, config.h, json.h, persist.h
 * ─────────────────────────────────────────────────────────────
 */

#pragma once

static uint32_t g_tg_last_ms = 0;

// ─── tg_send ──────────────────────────────────────────────────────────────────
// Send text to Telegram chat, splitting into TG_MSG_CHUNK-byte chunks.
static int16_t tg_send(const char *chat_id, const char *text) {
    static char tg_esc[4096];
    static char tg_path[CFG_S];
    static char tg_body[4096];

    uint16_t tlen = strlen(text);
    int16_t last_code = 0;
    uint16_t sent = 0;
    while (sent < tlen) {
        uint16_t chunk = min((uint16_t)(tlen - sent), TG_MSG_CHUNK);
        json_escape(text + sent, chunk, tg_esc, JSON_OUT_S);
        sent += chunk;
        snprintf(tg_path, CFG_S, "/bot%s/sendMessage", g_cfg.telegram.token);
        snprintf(tg_body, JSON_OUT_S,
                 "{\"chat_id\":\"%s\",\"text\":\"%s\"}", chat_id, tg_esc);

        g_suppress_tls_logs = true;
        g_http_busy = true;
        last_code = https_req(g_tls_tg, "api.telegram.org", tg_path, nullptr,
                              tg_body, strlen(tg_body), g_http_resp, HTTP_RESP_S);
        g_http_busy = false;
        g_suppress_tls_logs = false;

        Serial.printf("[Telegram] sendMessage code=%d\r\n", last_code);
    }
    return last_code;
}

// ─── tg_poll ──────────────────────────────────────────────────────────────────
static void tg_poll() {
    if (!g_cfg.telegram.enabled || !g_cfg.telegram.token[0]) return;
    if ((millis() - g_tg_last_ms) < TG_POLL_MS) return;
    if (g_http_busy) return;
    g_tg_last_ms = millis();

    snprintf(g_tx_path, CFG_S, "/bot%s/getUpdates?offset=%lld&timeout=1&limit=5",
             g_cfg.telegram.token, (long long)g_tg_offset);

    g_suppress_tls_logs = true;
    g_http_busy = true;
    int16_t code = https_req(g_tls_tg, "api.telegram.org", g_tx_path, nullptr,
                              nullptr, 0, g_http_resp, HTTP_RESP_S);
    g_http_busy = false;
    g_suppress_tls_logs = false;

    if (code != 200) {
        Serial.printf("[Telegram] poll failed code=%d resp=%.150s\r\n", code, g_http_resp);
        return;
    }

    const char *p = g_http_resp;
    while ((p = strstr(p, "\"update_id\"")) != nullptr) {
        int64_t uid = jint(p + strlen("\"update_id\"") + 1);
        if (uid >= g_tg_offset) {
            g_tg_offset = uid + 1;
#if PERSIST_IMPL == 1
            prefs.begin("femtoclaw", false);
            prefs.putLong64("tg_offset", g_tg_offset);
            prefs.end();
#else
            cfg_save();
#endif
        }

        const char *msg_start = strstr(p, "\"message\"");
        if (!msg_start) { ++p; continue; }

        char from_id[ALLOW_ID_LEN] = {0};
        char chat_id[ALLOW_ID_LEN] = {0};
        char text[PROMPT_S]        = {0};

        const char *from_sec = strstr(msg_start, "\"from\"");
        if (from_sec) {
            const char *id_v = jfind(from_sec, "id");
            if (id_v) id_from_int64(jint(id_v), from_id, sizeof(from_id));
        }
        const char *chat_sec = strstr(msg_start, "\"chat\"");
        if (chat_sec) {
            const char *id_v = jfind(chat_sec, "id");
            if (id_v) id_from_int64(jint(id_v), chat_id, sizeof(chat_id));
        }
        const char *tv = jfind(msg_start, "text");
        if (tv) jstr(tv, text, PROMPT_S);

        Serial.printf("[Telegram] update_id=%lld from=%s chat=%s text='%s'\r\n",
                      (long long)uid, from_id, chat_id, text);

        if (!text[0]) { ++p; continue; }
        if (!is_allowed(g_cfg.telegram, from_id)) {
            Serial.printf("[Telegram] BLOCKED — from_id=%s not in allow list\r\n", from_id);
            ++p; continue;
        }

        const char *reply = agent_run(text);
        Serial.printf("[Telegram] replying (%u chars) → chat %s\r\n",
                      (unsigned)strlen(reply), chat_id);

        delay(TLS_SETTLE_MS);
        int16_t sc = tg_send(chat_id, reply);
        if (sc != 200)
            Serial.printf("[Telegram] send FAILED code=%d resp=%.100s\r\n", sc, g_http_resp);

        ++p;
    }
}