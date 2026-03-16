#pragma once

static int64_t g_tg_offset = 0;
static char g_dc_last_msg_id[ALLOW_ID_LEN] = {0};

#if PERSIST_IMPL == 1
// ESP32: use Preferences (NVS)
static void cfg_save() {
  prefs.begin("femtoclaw", false);
  prefs.putString("wifi_ssid",        g_cfg.wifi_ssid);
  prefs.putString("wifi_pass",        g_cfg.wifi_pass);
  prefs.putString("llm_provider",     g_cfg.llm_provider);
  prefs.putString("llm_api_key",      g_cfg.llm_api_key);
  prefs.putString("llm_api_base",     g_cfg.llm_api_base);
  prefs.putString("llm_model",        g_cfg.llm_model);
  prefs.putUShort("max_tokens",       g_cfg.max_tokens);
  prefs.putFloat ("temperature",      g_cfg.temperature);
  prefs.putUChar ("max_tool_iters",   g_cfg.max_tool_iters);
  prefs.putUInt  ("heartbeat_ms",     g_cfg.heartbeat_ms);
  prefs.putBool  ("tg_enabled",       g_cfg.telegram.enabled);
  prefs.putString("tg_token",         g_cfg.telegram.token);
  prefs.putUChar ("tg_allow_count",   g_cfg.telegram.allow_count);
  for (uint8_t i = 0; i < g_cfg.telegram.allow_count; ++i) {
    char k[16]; snprintf(k, 16, "tg_allow_%u", i);
    prefs.putString(k, g_cfg.telegram.allow_from[i]);
  }
  prefs.putBool  ("dc_enabled",       g_cfg.discord.enabled);
  prefs.putString("dc_token",         g_cfg.discord.token);
  prefs.putString("dc_channel_id",    g_cfg.discord_channel_id);
  prefs.putUChar ("dc_allow_count",   g_cfg.discord.allow_count);
  for (uint8_t i = 0; i < g_cfg.discord.allow_count; ++i) {
    char k[16]; snprintf(k, 16, "dc_allow_%u", i);
    prefs.putString(k, g_cfg.discord.allow_from[i]);
  }
  // Save polling cursors so they persist across reboots
  prefs.putLong64("tg_offset", g_tg_offset);
  prefs.putString("dc_last_id", g_dc_last_msg_id);
  // Board config : stored as a NVS blob (up to 4 KB)
  prefs.putBool  ("board_loaded",  g_cfg.board_md_loaded);
  if (g_cfg.board_md_loaded)
    prefs.putBytes("board_md", g_cfg.board_md, strlen(g_cfg.board_md) + 1);
  prefs.end();
}

static void cfg_load() {
  // Set defaults first
  strlcpy(g_cfg.llm_provider,  "openrouter", 32);
  strlcpy(g_cfg.llm_api_base,  "https://openrouter.ai/api/v1", CFG_S);
  strlcpy(g_cfg.llm_model,     "meta-llama/llama-3.1-8b-instruct:free", 64);
  g_cfg.max_tokens     = 512;
  g_cfg.temperature    = 0.7f;
  g_cfg.max_tool_iters = 3;
  g_cfg.heartbeat_ms   = 0;
  g_cfg.telegram.enabled = false;
  g_cfg.telegram.allow_count = 0;
  g_cfg.discord.enabled = false;
  g_cfg.discord.allow_count = 0;

  prefs.begin("femtoclaw", true);
  prefs.getString("wifi_ssid",     g_cfg.wifi_ssid,        CFG_S);
  prefs.getString("wifi_pass",     g_cfg.wifi_pass,        CFG_S);
  prefs.getString("llm_provider",  g_cfg.llm_provider,     32);
  prefs.getString("llm_api_key",   g_cfg.llm_api_key,      LLM_KEY);
  prefs.getString("llm_api_base",  g_cfg.llm_api_base,     CFG_S);
  prefs.getString("llm_model",     g_cfg.llm_model,        64);
  g_cfg.max_tokens     = prefs.getUShort("max_tokens",     g_cfg.max_tokens);
  g_cfg.temperature    = prefs.getFloat ("temperature",    g_cfg.temperature);
  g_cfg.max_tool_iters = prefs.getUChar ("max_tool_iters", g_cfg.max_tool_iters);
  g_cfg.heartbeat_ms   = prefs.getUInt  ("heartbeat_ms",   g_cfg.heartbeat_ms);
  g_cfg.telegram.enabled = prefs.getBool("tg_enabled", false);
  prefs.getString("tg_token",      g_cfg.telegram.token,   CFG_S);
  g_cfg.telegram.allow_count = prefs.getUChar("tg_allow_count", 0);
  for (uint8_t i = 0; i < g_cfg.telegram.allow_count; ++i) {
    char k[16]; snprintf(k, 16, "tg_allow_%u", i);
    prefs.getString(k, g_cfg.telegram.allow_from[i], ALLOW_ID_LEN);
  }
  g_cfg.discord.enabled = prefs.getBool("dc_enabled", false);
  prefs.getString("dc_token",      g_cfg.discord.token,    CFG_S);
  prefs.getString("dc_channel_id", g_cfg.discord_channel_id, ALLOW_ID_LEN);
  g_cfg.discord.allow_count = prefs.getUChar("dc_allow_count", 0);
  for (uint8_t i = 0; i < g_cfg.discord.allow_count; ++i) {
    char k[16]; snprintf(k, 16, "dc_allow_%u", i);
    prefs.getString(k, g_cfg.discord.allow_from[i], ALLOW_ID_LEN);
  }
  // Restore polling cursors
  g_tg_offset = prefs.getLong64("tg_offset", 0);
  prefs.getString("dc_last_id", g_dc_last_msg_id, sizeof(g_dc_last_msg_id));
  g_cfg.board_md_loaded = prefs.getBool("board_loaded", false);
  if (g_cfg.board_md_loaded) {
    size_t bsz = prefs.getBytesLength("board_md");
    if (bsz > 0 && bsz < sizeof(g_cfg.board_md))
      prefs.getBytes("board_md", g_cfg.board_md, bsz);
    else
      g_cfg.board_md_loaded = false; // corrupt / oversized => ignore
  }
  prefs.end();
}

#elif PERSIST_IMPL == 2
// Pico W: LittleFS
static void cfg_save() {
  static char buf[2048];
  int n = snprintf(buf, sizeof(buf),
    "{"
      "\"wifi_ssid\":\"%s\","
      "\"wifi_pass\":\"%s\","
      "\"llm_provider\":\"%s\","
      "\"llm_api_key\":\"%s\","
      "\"llm_api_base\":\"%s\","
      "\"llm_model\":\"%s\","
      "\"max_tokens\":%u,"
      "\"temperature\":%.2f,"
      "\"max_tool_iters\":%u,"
      "\"heartbeat_ms\":%lu,"
      "\"tg_enabled\":%s,"
      "\"tg_token\":\"%s\","
      "\"tg_allow_count\":%u,"
      "\"tg_allow\":[",
    g_cfg.wifi_ssid, g_cfg.wifi_pass,
    g_cfg.llm_provider, g_cfg.llm_api_key, g_cfg.llm_api_base, g_cfg.llm_model,
    g_cfg.max_tokens, (double)g_cfg.temperature, g_cfg.max_tool_iters,
    (unsigned long)g_cfg.heartbeat_ms,
    g_cfg.telegram.enabled?"true":"false",
    g_cfg.telegram.token, g_cfg.telegram.allow_count);
  for (uint8_t i=0; i<g_cfg.telegram.allow_count; ++i) {
    n += snprintf(buf+n, sizeof(buf)-n, "%s\"%s\"", i?",":"", g_cfg.telegram.allow_from[i]);
  }
  n += snprintf(buf+n, sizeof(buf)-n,
    "],"
    "\"dc_enabled\":%s,"
    "\"dc_token\":\"%s\","
    "\"dc_channel_id\":\"%s\","
    "\"dc_allow_count\":%u,"
    "\"dc_allow\":[",
    g_cfg.discord.enabled?"true":"false",
    g_cfg.discord.token, g_cfg.discord_channel_id, g_cfg.discord.allow_count);
  for (uint8_t i=0; i<g_cfg.discord.allow_count; ++i) {
    n += snprintf(buf+n, sizeof(buf)-n, "%s\"%s\"", i?",":"", g_cfg.discord.allow_from[i]);
  }
  n += snprintf(buf+n, sizeof(buf)-n,
    "],"
    "\"tg_offset\":%lld,"
    "\"dc_last_id\":\"%s\""
    "}",
    (long long)g_tg_offset, g_dc_last_msg_id);

  if (n < 0 || n >= (int)sizeof(buf)) {
    Serial.printf("[cfg_save] ERROR: JSON too large (%d bytes) — not saved\r\n", n);
    return;
  }

  LittleFS.begin();
  File f = LittleFS.open("/femtoclaw.json", "w");
  if (f) { f.write((uint8_t*)buf, n); f.close(); }
  else Serial.println("[cfg_save] ERROR: file open failed");
  // Board config stored as a separate /control.md file (may exceed 2 KB JSON buf)
  if (g_cfg.board_md_loaded && g_cfg.board_md[0]) {
    File bm = LittleFS.open("/control.md", "w");
    if (bm) { bm.print(g_cfg.board_md); bm.close(); }
    else Serial.println("[cfg_save] ERROR: /control.md open failed");
  }
  LittleFS.end();
}

static void cfg_load() {
  strlcpy(g_cfg.llm_provider, "openrouter", 32);
  strlcpy(g_cfg.llm_api_base, "https://openrouter.ai/api/v1", CFG_S);
  strlcpy(g_cfg.llm_model,    "meta-llama/llama-3.1-8b-instruct:free", 64);
  g_cfg.max_tokens     = 512;
  g_cfg.temperature    = 0.7f;
  g_cfg.max_tool_iters = 3;
  g_cfg.heartbeat_ms   = 0;
  g_cfg.telegram.enabled = false;
  g_cfg.telegram.allow_count = 0;
  g_cfg.discord.enabled = false;
  g_cfg.discord.allow_count = 0;

  LittleFS.begin();
  if (!LittleFS.exists("/femtoclaw.json")) { LittleFS.end(); return; }
  File f = LittleFS.open("/femtoclaw.json", "r");
  if (!f) { LittleFS.end(); return; }
  static char jbuf[2048];
  size_t sz = f.readBytes(jbuf, sizeof(jbuf)-1);
  f.close(); LittleFS.end();
  jbuf[sz] = '\0';

  const char *v;
  if ((v=jfind(jbuf,"wifi_ssid")))      jstr(v, g_cfg.wifi_ssid,        CFG_S);
  if ((v=jfind(jbuf,"wifi_pass")))      jstr(v, g_cfg.wifi_pass,        CFG_S);
  if ((v=jfind(jbuf,"llm_provider")))   jstr(v, g_cfg.llm_provider,     32);
  if ((v=jfind(jbuf,"llm_api_key")))    jstr(v, g_cfg.llm_api_key,      LLM_KEY);
  if ((v=jfind(jbuf,"llm_api_base")))   jstr(v, g_cfg.llm_api_base,     CFG_S);
  if ((v=jfind(jbuf,"llm_model")))      jstr(v, g_cfg.llm_model,        64);
  if ((v=jfind(jbuf,"max_tokens")))     g_cfg.max_tokens     = (uint16_t)jint(v);
  if ((v=jfind(jbuf,"temperature")))    g_cfg.temperature    = (float)atof(v);
  if ((v=jfind(jbuf,"max_tool_iters"))) g_cfg.max_tool_iters = (uint8_t)jint(v);
  if ((v=jfind(jbuf,"heartbeat_ms")))   g_cfg.heartbeat_ms   = (uint32_t)jint(v);
  if ((v=jfind(jbuf,"tg_enabled")))     g_cfg.telegram.enabled = (*v=='t');
  if ((v=jfind(jbuf,"tg_token")))       jstr(v, g_cfg.telegram.token,   CFG_S);
  if ((v=jfind(jbuf,"tg_allow_count"))) g_cfg.telegram.allow_count = (uint8_t)jint(v);
  if ((v=jfind(jbuf,"tg_allow"))) {
    const char *p = strchr(v, '['); if (!p) goto dc_section;
    for (uint8_t i=0; i<g_cfg.telegram.allow_count; ++i) {
      p = strchr(p, '"'); if (!p) break; ++p;
      const char *e = strchr(p, '"'); if (!e) break;
      memcpy(g_cfg.telegram.allow_from[i], p, min((ptrdiff_t)(ALLOW_ID_LEN-1), e-p));
      g_cfg.telegram.allow_from[i][min((ptrdiff_t)(ALLOW_ID_LEN-1), e-p)] = '\0';
      p = e+1;
    }
  }
dc_section:
  if ((v=jfind(jbuf,"dc_enabled")))     g_cfg.discord.enabled = (*v=='t');
  if ((v=jfind(jbuf,"dc_token")))       jstr(v, g_cfg.discord.token,    CFG_S);
  if ((v=jfind(jbuf,"dc_channel_id")))  jstr(v, g_cfg.discord_channel_id, ALLOW_ID_LEN);
  if ((v=jfind(jbuf,"dc_allow_count"))) g_cfg.discord.allow_count = (uint8_t)jint(v);
  if ((v=jfind(jbuf,"dc_allow"))) {
    const char *p = strchr(v, '['); if (!p) goto cursors;
    for (uint8_t i=0; i<g_cfg.discord.allow_count; ++i) {
      p = strchr(p, '"'); if (!p) break; ++p;
      const char *e = strchr(p, '"'); if (!e) break;
      memcpy(g_cfg.discord.allow_from[i], p, min((ptrdiff_t)(ALLOW_ID_LEN-1), e-p));
      g_cfg.discord.allow_from[i][min((ptrdiff_t)(ALLOW_ID_LEN-1), e-p)] = '\0';
      p = e+1;
    }
  }
cursors:
  if ((v=jfind(jbuf,"tg_offset")))   g_tg_offset = jint(v);
  if ((v=jfind(jbuf,"dc_last_id"))) jstr(v, g_dc_last_msg_id, sizeof(g_dc_last_msg_id));
  // Board config : stored in a separate /control.md file.
  // LittleFS was closed after reading femtoclaw.json above; re-open it here.
  g_cfg.board_md_loaded = false;
  LittleFS.begin();
  if (LittleFS.exists("/control.md")) {
    File bm = LittleFS.open("/control.md", "r");
    if (bm) {
      size_t bsz = bm.readBytes(g_cfg.board_md, sizeof(g_cfg.board_md) - 1);
      g_cfg.board_md[bsz] = '\0';
      bm.close();
      g_cfg.board_md_loaded = true;
    }
  }
  LittleFS.end();
}
#endif