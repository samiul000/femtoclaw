/*
 * ─────────────────────────────────────────────────────────────
 * FemtoClaw : Discord HTTP REST polling channel.
 *
 * Depends on: http.h, agent.h, config.h, json.h, persist.h
 * ─────────────────────────────────────────────────────────────
 */

#pragma once

static uint32_t g_dc_last_ms = 0;

// ─── dc_send ──────────────────────────────────────────────────────────────────
static int16_t dc_send(const char *text) {
    if (!g_cfg.discord_channel_id[0]) return 0;

    static char dc_esc[4096];
    static char dc_auth[CFG_S + 32];
    static char dc_path[CFG_S];
    static char dc_body[4096];

    snprintf(dc_auth, sizeof(dc_auth), "Authorization: Bot %s\r\n", g_cfg.discord.token);
    snprintf(dc_path, CFG_S, "/api/v10/channels/%s/messages", g_cfg.discord_channel_id);

    uint16_t tlen = strlen(text);
    int16_t last_code = 0;
    uint16_t sent = 0;
    while (sent < tlen) {
        uint16_t chunk = min((uint16_t)(tlen - sent), DC_MSG_CHUNK);
        json_escape(text + sent, chunk, dc_esc, JSON_OUT_S);
        sent += chunk;
        snprintf(dc_body, JSON_OUT_S, "{\"content\":\"%s\"}", dc_esc);

        g_suppress_tls_logs = true;
        g_http_busy = true;
        last_code = https_req(g_tls_dc, "discord.com", dc_path, dc_auth,
                              dc_body, strlen(dc_body), g_http_resp, HTTP_RESP_S);
        g_http_busy = false;
        g_suppress_tls_logs = false;

        Serial.printf("[Discord] send code=%d\r\n", last_code);
    }
    return last_code;
}

// ─── dc_poll ──────────────────────────────────────────────────────────────────
static void dc_poll() {
    if (!g_cfg.discord.enabled || !g_cfg.discord.token[0]) return;
    if (!g_cfg.discord_channel_id[0]) return;
    if ((millis() - g_dc_last_ms) < DC_POLL_MS) return;
    if (g_http_busy) return;
    g_dc_last_ms = millis();

    static char dc_poll_auth[CFG_S + 32];
    static char dc_poll_path[CFG_S];
    snprintf(dc_poll_auth, sizeof(dc_poll_auth), "Authorization: Bot %s\r\n", g_cfg.discord.token);

    if (g_dc_last_msg_id[0])
        snprintf(dc_poll_path, CFG_S, "/api/v10/channels/%s/messages?after=%s&limit=5",
                 g_cfg.discord_channel_id, g_dc_last_msg_id);
    else
        snprintf(dc_poll_path, CFG_S, "/api/v10/channels/%s/messages?limit=1",
                 g_cfg.discord_channel_id);

    g_suppress_tls_logs = true;
    g_http_busy = true;
    int16_t code = https_req(g_tls_dc, "discord.com", dc_poll_path, dc_poll_auth,
                              nullptr, 0, g_http_resp, HTTP_RESP_S);
    g_http_busy = false;
    g_suppress_tls_logs = false;

    if (code != 200) {
        Serial.printf("[Discord] poll code=%d\r\n", code);
        return;
    }

    bool first_poll = !g_dc_last_msg_id[0];

    const char *p = g_http_resp;
    while ((p = strstr(p, "\"id\"")) != nullptr) {
        char msg_id[ALLOW_ID_LEN] = {0};
        const char *id_v = p + strlen("\"id\"");
        while (*id_v == ' ' || *id_v == ':') ++id_v;
        id_from_str(id_v, msg_id, sizeof(msg_id));

        // Discord IDs are Snowflakes : compare as strings (fixed-width numeric)
        bool is_new = (strlen(msg_id) > strlen(g_dc_last_msg_id) ||
                       strcmp(msg_id, g_dc_last_msg_id) > 0);

        if (is_new && msg_id[0]) {
            strlcpy(g_dc_last_msg_id, msg_id, sizeof(g_dc_last_msg_id));
#if PERSIST_IMPL == 1
            prefs.begin("femtoclaw", false);
            prefs.putString("dc_last_id", g_dc_last_msg_id);
            prefs.end();
#else
            cfg_save();
#endif
        }

        if (first_poll || !is_new) { ++p; continue; }

        const char *auth_sec = strstr(p, "\"author\"");
        char author_id[ALLOW_ID_LEN] = {0};
        if (auth_sec) {
            const char *ai = jfind(auth_sec, "id");
            if (ai) id_from_str(ai, author_id, sizeof(author_id));
        }

        const char *cv = jfind(p, "content");
        char content[PROMPT_S] = {0};
        if (cv) jstr(cv, content, PROMPT_S);

        Serial.printf("[Discord] msg_id=%s author=%s content='%s'\r\n",
                      msg_id, author_id, content);

        if (!content[0]) { ++p; continue; }
        if (!is_allowed(g_cfg.discord, author_id)) {
            Serial.printf("[Discord] BLOCKED — author=%s not in allow list\r\n", author_id);
            ++p; continue;
        }

        const char *reply = agent_run(content);
        delay(TLS_SETTLE_MS);
        dc_send(reply);
        ++p;
    }
}