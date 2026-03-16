/*
 * ─────────────────────────────────────────────────────────────
 * Agentic loop: built-in tools + multi-turn runner.
 *
 * Depends on: llm.h, actions.h, persist.h, config.h
 * ─────────────────────────────────────────────────────────────
 */

#pragma once

static char g_llm_out[RESP_S];
static char g_action_results[512];
static char g_tool_result[512];

// ─── tool_dispatch ────────────────────────────────────────────────────────────
// Execute a named built-in tool and store the result in g_tool_result.
static void tool_dispatch(const char *name, const char *args) {
    if (!strcmp(name, "message")) {
        Serial.printf("[agent] %s\r\n", args);
        strlcpy(g_tool_result, "sent", 512);

    } else if (!strcmp(name, "get_wifi_info")) {
        snprintf(g_tool_result, 512,
                 "{\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d}",
                 WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());

    } else if (!strcmp(name, "get_time")) {
        snprintf(g_tool_result, 512, "{\"uptime_ms\":%lu}", millis());

    } else if (!strcmp(name, "set_config")) {
        char key[48]={0}, val[LLM_KEY]={0};
        const char *kp = jfind(args, "key"), *vp = jfind(args, "value");
        if (kp) jstr(kp, key, 48);
        if (vp) jstr(vp, val, LLM_KEY);
        if      (!strcmp(key,"llm_model"))     strlcpy(g_cfg.llm_model,      val, 64);
        else if (!strcmp(key,"llm_api_key"))   strlcpy(g_cfg.llm_api_key,    val, LLM_KEY);
        else if (!strcmp(key,"llm_api_base"))  strlcpy(g_cfg.llm_api_base,   val, CFG_S);
        else if (!strcmp(key,"llm_provider"))  strlcpy(g_cfg.llm_provider,   val, 32);
        else if (!strcmp(key,"wifi_ssid"))     strlcpy(g_cfg.wifi_ssid,      val, CFG_S);
        else if (!strcmp(key,"wifi_pass"))     strlcpy(g_cfg.wifi_pass,      val, CFG_S);
        else if (!strcmp(key,"tg_token"))  {
            strlcpy(g_cfg.telegram.token, val, CFG_S);
            g_cfg.telegram.enabled = true;
        }
        else if (!strcmp(key,"dc_token"))  {
            strlcpy(g_cfg.discord.token, val, CFG_S);
            g_cfg.discord.enabled = true;
        }
        else if (!strcmp(key,"dc_channel_id")) strlcpy(g_cfg.discord_channel_id, val, ALLOW_ID_LEN);
        cfg_save();
        snprintf(g_tool_result, 512, "set %s ok", key);

    } else if (!strcmp(name, "get_config")) {
        snprintf(g_tool_result, 512,
                 "{\"model\":\"%s\",\"provider\":\"%s\","
                 "\"tg\":%s,\"dc\":%s,\"uptime\":%lu}",
                 g_cfg.llm_model, g_cfg.llm_provider,
                 g_cfg.telegram.enabled ? "true" : "false",
                 g_cfg.discord.enabled  ? "true" : "false",
                 millis());

    } else if (!strcmp(name, "reset_session")) {
        session_clear();
        strlcpy(g_tool_result, "cleared", 512);

    } else {
        snprintf(g_tool_result, 512, "[tool %s not on MCU]", name);
    }
}

// ─── Agent run ────────────────────────────────────────────────────────────────
/*
 * Multi-turn loop: call LLM, execute any [ACTION:...] or <tool:...> blocks,
 * feed results back, and repeat up to max_tool_iters times.
 */
static const char *agent_run(const char *user_input) {
    static char combined[PROMPT_S + 512];
    strlcpy(combined, user_input, sizeof(combined));

    for (uint8_t iter = 0; iter < g_cfg.max_tool_iters; ++iter) {
        if (!llm_chat(combined, g_llm_out, RESP_S)) return g_llm_out;
        session_append("user", iter == 0 ? user_input : "[action_results]");

        int n_actions = execute_actions_in_response(
            g_llm_out, g_action_results, sizeof(g_action_results));

        strip_action_tags(g_llm_out);
        session_append("assistant", g_llm_out);

        // ── Legacy <tool:...> built-in tool dispatch ──────────────────
        const char *tc = strstr(g_llm_out, "<tool:");
        if (!tc && n_actions == 0) return g_llm_out;

        if (tc) {
            const char *ns = tc + 6, *ne = strchr(ns, '>'); if (!ne) break;
            char tname[48] = {0};
            memcpy(tname, ns, min((ptrdiff_t)47, ne - ns));
            const char *as = ne + 1, *ae = strstr(as, "</tool>");
            uint16_t al = ae ? (uint16_t)(ae - as) : 0;
            static char targs[512];
            memcpy(targs, as, min(al, (uint16_t)511)); targs[min(al, (uint16_t)511)] = '\0';
            tool_dispatch(tname, targs);
            Serial.printf("[tool:%s] %s\r\n", tname, g_tool_result);
            snprintf(combined, sizeof(combined), "[Tool %s]: %s", tname, g_tool_result);
        } else if (n_actions > 0) {
            strlcpy(combined, g_action_results, sizeof(combined));
        } else {
            return g_llm_out;
        }
    }
    return g_llm_out;
}