/*
 * ─────────────────────────────────────────────────────────────
 * Periodic heartbeat runner.
 *
 * Depends on: agent.h, mcu_wifi.h, config.h
 * ─────────────────────────────────────────────────────────────
 */

#pragma once

static uint32_t g_hb_last = 0;

static void heartbeat_check() {
    if (!g_cfg.heartbeat_ms) return;
    if ((millis() - g_hb_last) < g_cfg.heartbeat_ms) return;
    g_hb_last = millis();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[heartbeat] WiFi lost : attempting reconnect...");
        wifi_connect(10);
        return;
    }

    Serial.println("[heartbeat] Running...");
    session_clear();
    const char *r = agent_run(
        "You are a scheduled heartbeat on an MCU. "
        "Report uptime and WiFi status in one short sentence.");
    Serial.printf("[heartbeat] %s\r\n", r);
}