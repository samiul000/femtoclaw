/**
 * femtoclaw_mcu.cpp
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * FemtoClaw — MCU based AI Assistant for WiFi-capable boards.
 * Targets  : ESP32 (DevKit, S3, C3) · Raspberry Pi Pico W (RP2040 + CYW43)
 * Developed by : Al Mahmud Samiul · amsamiul.dev@gmail.com
 *
 * Channels implemented:
 *   • UART shell      — always on (USB-CDC or hardware UART0)
 *   • Telegram        — long-polling via Bot API (getUpdates)
 *   • Discord         — HTTP REST (no WebSocket on MCU; polls /messages)
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 */

// ─── Platform headers ────────────────────────────────────────────────────────
#if !defined(BOARD_ESP32) && !defined(BOARD_PICO_W)
  #error "Define -DBOARD_ESP32 or -DBOARD_PICO_W in your build flags."
#endif

// ── Compile-time safety check for ESP32-C3 native USB ──
#ifdef BOARD_ESP32
  #ifndef ARDUINO_USB_CDC_ON_BOOT
    #warning "ARDUINO_USB_CDC_ON_BOOT not defined — set it to 1 for native USB ESP32-C3"
  #elif ARDUINO_USB_CDC_ON_BOOT == 0
    #warning "ARDUINO_USB_CDC_ON_BOOT=0 on ESP32-C3 may cause RTC_SW_SYS_RST boot loop"
  #endif
#endif

#ifdef BOARD_ESP32
  #include <WiFi.h>
  #include <WiFiClientSecure.h>
  #ifdef BOARD_HAS_PSRAM
    #include <esp_psram.h>
  #endif
  #include <Preferences.h>
  #define PLATFORM_NAME "ESP32"
  #define PERSIST_IMPL 1
  static Preferences prefs;
#elif defined(BOARD_PICO_W)
  #include <WiFi.h>
  #include <WiFiClientSecure.h>
  #include <LittleFS.h>
  #define PLATFORM_NAME "Pico W"
  #define PERSIST_IMPL 2
  //namespace rp2040 { extern void reboot(); }
#endif

// ─── Config constants ────────────────────────────────────────────────────────
#ifndef LED_PIN
  #if defined(LED_BUILTIN)
    #define LED_PIN LED_BUILTIN
  #elif defined(BOARD_ESP32)
    #define LED_PIN 2
  #elif defined(BOARD_PICO_W)
    #define LED_PIN LED_BUILTIN
  #else
    #define LED_PIN 13
  #endif
#endif

static constexpr uint32_t UART_BAUD         = 115200;
static constexpr uint32_t HTTP_TIMEOUT_MS   = 60000;
static constexpr uint32_t TG_POLL_MS        = 5000;
static constexpr uint32_t DC_POLL_MS        = 5000;
static constexpr uint16_t TG_MSG_CHUNK      = 3800;
static constexpr uint16_t DC_MSG_CHUNK      = 1800;
static constexpr uint16_t TLS_SETTLE_MS     = 20;
static constexpr uint16_t CHUNK             = 512;
static constexpr uint16_t CFG_S             = 128;
static constexpr uint16_t RESP_S            = 2048;
static constexpr uint16_t PROMPT_S          = 1024;
static constexpr uint16_t JSON_OUT_S        = 4096;
static constexpr uint16_t HTTP_RESP_S       = 8192;  // raised if needed but not recommended for long OpenRouter responses + headers
static constexpr uint16_t CMD_S             = 256;
static constexpr uint16_t SESSION_S         = 4096;
static constexpr uint8_t  ALLOW_LIST_MAX    = 8;
/*
*   ID buffer size: must hold the largest possible string representation of any
*    platform ID plus a null terminator.
*      Telegram user IDs  : up to 10 digits (currently ~7.9B, max int64 = 19 digits)
*      Telegram chat IDs  : negative groups, up to -100XXXXXXXXXX (15 chars with sign)
*      Discord Snowflakes : 17-19 digits currently
*    32 bytes gives comfortable headroom for all current and near-future IDs.
*    NOTE: every local char[] used for IDs must use ALLOW_ID_LEN, not a
*    hardcoded literal, so a single change here propagates everywhere.
*/
static constexpr uint8_t  ALLOW_ID_LEN      = 32;

// ─── Config ──────────────────────────────────────────────────────────────────
struct ChannelCfg {
  bool    enabled;
  char    token[CFG_S];
  char    allow_from[ALLOW_LIST_MAX][ALLOW_ID_LEN];
  uint8_t allow_count;
};

struct Config {
  char wifi_ssid[CFG_S];
  char wifi_pass[CFG_S];
  char llm_provider[32];
  char llm_api_key[CFG_S];
  char llm_api_base[CFG_S];
  char llm_model[64];
  uint16_t max_tokens;
  float    temperature;
  uint8_t  max_tool_iters;
  uint32_t heartbeat_ms;
  ChannelCfg telegram;
  ChannelCfg discord;
  char discord_channel_id[ALLOW_ID_LEN];
};

static Config g_cfg;

// ─── Zero-alloc JSON helpers ─────────────────────────────────────────────────
static void json_escape(const char *s, uint16_t slen, char *out, uint16_t cap) {
  uint16_t w = 0;
  for (uint16_t i = 0; i < slen && w + 6 < cap; ++i) {
    switch ((uint8_t)s[i]) {
      case '"':  out[w++]='\\'; out[w++]='"';  break;
      case '\\': out[w++]='\\'; out[w++]='\\'; break;
      case '\n': out[w++]='\\'; out[w++]='n';  break;
      case '\r': out[w++]='\\'; out[w++]='r';  break;
      case '\t': out[w++]='\\'; out[w++]='t';  break;
      default:   out[w++]=s[i]; break;
    }
  }
  out[w] = '\0';
}

static const char *jfind(const char *j, const char *key) {
  char needle[52]; snprintf(needle, 52, "\"%s\"", key);
  const char *p = strstr(j, needle);
  if (!p) return nullptr;
  p += strlen(needle);
  while (*p == ' ' || *p == ':') ++p;
  return p;
}

static bool jstr(const char *p, char *out, uint16_t cap,
                  const char *buf_end = nullptr) {
  // buf_end: optional pointer to one-past-end of the source buffer.
  // If provided, jstr stops there even if no closing " is found —
  // prevents reading past a truncated HTTP response buffer into garbage memory.
  if (!p || *p != '"') return false;
  ++p;
  uint16_t w = 0;
  while (*p && w + 1 < cap) {
    if (buf_end && p >= buf_end) break;  // buffer boundary — stop safely
    if (*p == '\\') {
      ++p;
      if (buf_end && p >= buf_end) break;
      switch (*p) {
        case 'n': out[w++]='\n'; break;
        case 'r': out[w++]='\r'; break;
        case 't': out[w++]='\t'; break;
        default:  out[w++]=*p;  break;
      }
    } else if (*p == '"') break;
    else out[w++]=*p;
    ++p;
  }
  out[w] = '\0';
  return true;
}

/*
*   `jint` — parse an unquoted JSON integer into int64_t.
*   Skips leading whitespace only. Does NOT skip quotes — if the JSON value is
*   a string (e.g. Discord Snowflakes: "id": "12345"), jint returns 0 and the
*   caller must use id_from_str() instead. This prevents silent precision loss
*   from parsing large Snowflakes through strtoll (int64 max = 19 digits, and
*   some toolchains treat long long as 32-bit in embedded targets).
*/
static int64_t jint(const char *p) {
  if (!p) return 0;
  while (*p == ' ') ++p;           // skip whitespace only, NOT quotes
  if (*p == '"') return 0;         // caller should use id_from_str() for strings
  return strtoll(p, nullptr, 10);
}

static bool id_from_int64(int64_t val, char *out, uint8_t cap) {
  int n = snprintf(out, cap, "%lld", (long long)val);
  if (n < 0 || n >= cap) {
    // zero the buffer so is_allowed() fails safely (denies, not allows)
    out[0] = '\0';
    Serial.printf("[ID] OVERFLOW: int64 %lld does not fit in %u-byte buffer\r\n",
                  (long long)val, (unsigned)cap);
    return false;
  }
  return true;
}

// id_from_str — copy a string ID (Discord Snowflake from JSON string field)
// into a fixed buffer. Returns false and zeroes the buffer on truncation.
// IMPORTANT: tmp is a local (not static) — id_from_str may be called twice
// in the same loop iteration (once for msg_id, once for author_id) and a
// static buffer would be clobbered by the second call while the first result
// is still being used.
static bool id_from_str(const char *src_json_ptr, char *out, uint8_t cap) {
  // Parse into a local buffer one byte larger than cap so we can detect
  // truncation: if jstr fills all cap+1 bytes the ID was too long.
  char tmp[ALLOW_ID_LEN + 2];  // +2: 1 for extra char detection, 1 for null
  if (!jstr(src_json_ptr, tmp, sizeof(tmp))) {
    out[0] = '\0';
    return false;
  }
  if (strlen(tmp) >= cap) {
    out[0] = '\0';
    Serial.printf("[ID] OVERFLOW: string ID '%.*s...' does not fit in %u-byte buffer\r\n",
                  (int)(cap - 1), tmp, (unsigned)cap);
    return false;
  }
  strlcpy(out, tmp, cap);
  return true;
}

// ─── allow_from check ────────────────────────────────────────────────────────
static bool is_allowed(const ChannelCfg &ch, const char *sender_id) {
  if (ch.allow_count == 0) return true;
  for (uint8_t i = 0; i < ch.allow_count; ++i)
    if (!strcmp(ch.allow_from[i], sender_id)) return true;
  return false;
}

// ─── WiFi ────────────────────────────────────────────────────────────────────
static void wifi_connect(uint8_t retries = 20) {
  if (!g_cfg.wifi_ssid[0]) return;
  if (WiFi.status() == WL_CONNECTED) { Serial.println("[WiFi] already connected."); return; }

  Serial.printf("[WiFi] connecting to '%s' ...\r\n", g_cfg.wifi_ssid);
#ifndef ARDUINO_USB_CDC_ON_BOOT
  WiFi.mode(WIFI_STA);
#endif
  WiFi.begin(g_cfg.wifi_ssid, g_cfg.wifi_pass);

  for (uint8_t i = 0; i < retries && WiFi.status() != WL_CONNECTED; ++i) {
    Serial.print(".");
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\r\n[WiFi] connected → IP %s  RSSI %d dBm\r\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println("\r\n[WiFi] connect failed.");
  }
}

// ─── Persistent config ───────────────────────────────────────────────────────
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
  prefs.getString("llm_api_key",   g_cfg.llm_api_key,      CFG_S);
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
  if ((v=jfind(jbuf,"llm_api_key")))    jstr(v, g_cfg.llm_api_key,      CFG_S);
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
}
#endif

// ─── HTTP / HTTPS POST / GET ─────────────────────────────────────────────────
/*
* Three dedicated TLS clients : one per remote host family.
*
*   g_tls_llm  — exclusively for LLM API calls (llm_chat)
*   g_tls_tg   — exclusively for Telegram API (tg_poll + tg_send)
*   g_tls_dc   — exclusively for Discord API  (dc_poll + dc_send)
*
* Sharing a single WiFiClientSecure caused [LLM -1] after the 2nd Telegram
* message because ESP32 lwIP does not free the socket FD synchronously on
* stop(). With three independent objects each host's socket lifecycle is
* entirely decoupled. Same applies to Discord.
*/
static WiFiClientSecure g_tls_llm;
static WiFiClientSecure g_tls_tg;
static WiFiClientSecure g_tls_dc;
static WiFiClient       g_tcp;

static char g_http_resp[HTTP_RESP_S];
static bool g_http_busy = false;   // true while any network I/O is in progress

// Flag to suppress TLS messages for background Telegram/Discord polling
static bool g_suppress_tls_logs = false;

// ─── TLS setInsecure helper ──────────────────────────────────────────────────
// Pico W's WiFiClientSecure (from the pico-sdk WiFi library or ArduinoMbedTLS)
// uses a different API than ESP32's Arduino-ESP32 WiFiClientSecure.
// This wrapper centralises the difference so both boards compile cleanly.
static void tls_set_insecure(WiFiClientSecure &tls) {
#ifdef BOARD_ESP32
  tls.setInsecure();              // Arduino-ESP32: skip certificate verification
#endif
#ifdef BOARD_PICO_W
  // Pico W Arduino core (earlephilhower): WiFiClientSecure::setInsecure()
  // exists from core ≥ 3.x and maps to BearSSL trust-none mode.
  // If your core version doesn't have it, replace with:
  //   tls.setCACert(nullptr); tls.setVerification(WiFiClientSecure::NONE);
  tls.setInsecure();
#endif
}

/*
* _stream_readline — read one CRLF-terminated line from a Stream.
* Times out after timeout_ms. Works on any Arduino Stream (WiFiClient,
* WiFiClientSecure) on any board without platform-specific dependencies.
*/

// ─── USB-CDC keepalive ────────────────────────────────────────────────────────
/*
* ESP32-C3 native USB: the Windows USB-CDC driver drops the COM port if it
* sees no USB traffic for ~500ms. Serial.flush() on an empty TX buffer is a
* no-op at the USB level, no packet is sent, so it does not help.
*
* Write a null byte (0x00) every 200ms instead. Null bytes are
* invisible in all terminal emulators (PuTTY, minicom, the GUI terminal) but
* force the USB stack to actually submit a transfer to the host endpoint,
* resetting the driver idle timer. 200ms is well under the ~500ms OS dropout
* threshold and generates negligible traffic (5 bytes/sec).
*
* Guard to ARDUINO_USB_CDC_ON_BOOT only on Pico W / hardware-UART ESP32,
* Serial.write() blocks until the UART TX FIFO drains, which is wasteful.
*/
static inline void usb_keepalive(unsigned long &last_ms) {
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  if (g_http_busy) return; // prevent null bytes injection during response streaming
  unsigned long now = millis();
  if (now - last_ms >= 200) {
    last_ms = now;
    Serial.write((uint8_t)0x00);  // null byte
    Serial.flush();
  }
#else
  (void)last_ms;
#endif
}

template<typename T>
static uint16_t _stream_readline(T &client, char *buf, uint16_t cap, uint32_t timeout_ms) {
  uint16_t w = 0;
  unsigned long t0 = millis(), last_ka = t0;
  while ((millis() - t0) < timeout_ms && w + 1 < cap) {
    usb_keepalive(last_ka);
    if (client.available()) {
      int c = client.read();
      if (c < 0 || c == '\n') break;
      if (c != '\r') buf[w++] = (char)c;
    } else if (!client.connected()) {
      break;
    } else {
      delay(1);
    }
  }
  buf[w] = '\0';
  return w;
}

template<typename T>
static uint16_t _stream_read_body(T &client, char *out, uint16_t out_cap) {
  uint16_t total = 0;
  unsigned long t0 = millis(), last_ka = t0;
  while ((millis() - t0) < HTTP_TIMEOUT_MS && total + 1 < out_cap) {
    usb_keepalive(last_ka);
    if (client.available()) {
      int c = client.read();
      if (c < 0) break;
      out[total++] = (char)c;
    } else if (!client.connected()) {
      break;
    } else {
      delay(1);
    }
  }
  out[total] = '\0';
  // (silent truncation - caller handles buffer sizing)
  return total;
}

// ─── Chunked transfer-encoding decoder ───────────────────────────────────────
// HTTP/1.1 servers (OpenRouter/Cloudflare) use Transfer-Encoding: chunked.
static uint16_t unchunk(char *buf, uint16_t len) {
  if (!len) return 0;
  char c0 = buf[0];
  if (!((c0>='0'&&c0<='9')||(c0>='a'&&c0<='f')||(c0>='A'&&c0<='F'))) return len;
  char *src = buf, *dst = buf, *end = buf + len;
  while (src < end) {
    char *nl = (char*)memchr(src, '\n', end-src); if (!nl) break;
    *nl = '\0'; char *cr = nl-1; if (cr >= src && *cr == '\r') *cr = '\0';
    unsigned long sz = strtoul(src, nullptr, 16); src = nl+1;
    if (!sz) break;
    if (src+sz > end) sz = (unsigned long)(end-src);
    memmove(dst, src, sz); dst += sz; src += sz;
    if (src < end && *src == '\r') src++;
    if (src < end && *src == '\n') src++;
  }
  uint16_t n = (uint16_t)(dst-buf); buf[n] = '\0'; return n;
}

static int16_t _parse_status(const char *line) {
  if (strncmp(line, "HTTP/", 5) != 0 || strlen(line) < 12) return -1;
  char tmp[5]; memcpy(tmp, line + 9, 3); tmp[3] = '\0';
  return (int16_t)atoi(tmp);
}

static char g_line_buf[128];

/*
* _stream_drain_headers — consume all HTTP headers until the blank line.
*
* HTTP headers end with \r\n\r\n. We track a 4-byte state machine rather than
* reading line-by-line, so header lines of any length are handled correctly.
* The line-by-line approach with a fixed g_line_buf silently fails when any
* header exceeds the buffer size — the non-zero return from _stream_readline
* means the blank line is never detected and body bytes end up skipped or
* treated as headers, causing "header bytes leaked into body" warnings.
* [Fix needed here, i couldn't resolve it]
*/
// Returns the HTTP status code parsed from the first line (e.g. 200, 404, -1).
template<typename T>
static int16_t _stream_drain_headers(T &client, uint32_t timeout_ms) {
  // Read and parse the status line first (e.g. "HTTP/1.0 200 OK\r\n")
  uint16_t n = _stream_readline(client, g_line_buf, sizeof(g_line_buf), timeout_ms);
  int16_t code = (n > 0) ? _parse_status(g_line_buf) : -1;

/*
*  Drain remaining headers by scanning for the blank line byte-by-byte.
*  Handles both CRLF (\r\n\r\n) and bare-LF (\n\n) line endings.
*   Ollama's HTTP/1.0 server uses bare \n — without this the state machine
*  never finds \r\n\r\n and times out, leaving all headers in the stream.
*
*   State machine tracks last two "end-of-line" characters seen:
*     prev_lf = true  → last meaningful char was \n (end of a line)
*   If we see \n while prev_lf is true → blank line found (\n\n or \r\n\r\n)
*/
  uint8_t seq = 0;
  bool prev_lf = false;
  unsigned long t0 = millis(), last_ka = t0;
  while ((millis() - t0) < timeout_ms) {
    usb_keepalive(last_ka);
    if (client.available()) {
      char c = (char)client.read();

      // ── bare-LF path (Ollama HTTP/1.0) ──
      if (c == '\n') {
        if (prev_lf) return code;  // \n\n = blank line = end of headers
        prev_lf = true;
      } else if (c != '\r') {
        prev_lf = false;  // non-CR/LF char resets bare-LF detector
      }
      // \r does NOT reset prev_lf — allows \r\n\r\n to trigger both paths

      // ── CRLF path (standard HTTP/1.1) ──
      if (c == '\r')      seq = (seq == 2) ? 3 : 1;
      else if (c == '\n') seq = (seq == 1 || seq == 3) ? seq + 1 : 0;
      else                seq = 0;

      if (seq == 4) return code;  // \r\n\r\n = end of headers
    } else if (!client.connected()) {
      break;
    } else {
      delay(1);
    }
  }
  return code;  // timeout or disconnect — return what we have
}

// _stream_send_req — send HTTP request header and body (if any).
// If body is nullptr or body_len is 0, sends a GET; otherwise POST.
template<typename T>
static void _stream_send_req(T &client, const char *host, const char *path,
                               const char *extra_headers,
                               const char *body, uint16_t body_len) {
  // USB keepalive during request assembly — on ESP32-C3 native USB, the TX
  // buffer can take 100-200ms to drain, during which the USB bus is silent
  // and the host may drop the COM port. The keepalive fires every 200ms.
  unsigned long last_ka = millis();

  if (body && body_len > 0) {
    client.printf("POST %s HTTP/1.1\r\n", path);
    yield(); usb_keepalive(last_ka);
    client.printf("Host: %s\r\n", host);
    yield(); usb_keepalive(last_ka);
    client.printf("Content-Type: application/json\r\n");
    yield(); usb_keepalive(last_ka);
    if (extra_headers && extra_headers[0])
      client.print(extra_headers);
    yield(); usb_keepalive(last_ka);
    client.printf("Content-Length: %u\r\nConnection: close\r\n\r\n", body_len);
    yield(); usb_keepalive(last_ka);
    // Write body in CHUNK-sized pieces
    uint16_t sent = 0;
    while (sent < body_len) {
      uint16_t n = (body_len - sent > CHUNK) ? CHUNK : (body_len - sent);
      client.write((const uint8_t*)body + sent, n);
      sent += n;
      yield(); usb_keepalive(last_ka);
    }
  } else {
    client.printf("GET %s HTTP/1.1\r\n", path);
    yield(); usb_keepalive(last_ka);
    client.printf("Host: %s\r\n", host);
    yield(); usb_keepalive(last_ka);
    if (extra_headers && extra_headers[0])
      client.print(extra_headers);
    yield(); usb_keepalive(last_ka);
    client.print("Connection: close\r\n\r\n");
    yield(); usb_keepalive(last_ka);
  }
}

/*
* `https_req` takes explicit WiFiClientSecure reference.
*
* Callers pass the appropriate dedicated client:
*   g_tls_llm for LLM, g_tls_tg for Telegram, g_tls_dc for Discord.
*
* tls_set_insecure() is called every time right before connect() so TLS
* trust mode is set correctly even if the object was previously stop()ped
* and the internal state was reset by the underlying TCP stack.
*
* TLS connection messages suppressed when g_suppress_tls_logs is true.
*/
static int16_t https_req(WiFiClientSecure &tls,
                          const char *host, const char *path,
                          const char *extra_headers,
                          const char *body, uint16_t body_len,
                          char *out, uint16_t out_cap) {
  /*
   Always stop before reconnecting to ensure lwIP releases the socket FD.
   Without this, WiFiClientSecure leaks ~2-4KB TLS heap per call and after
   3-4 LLM responses the ESP32-C3's heap is exhausted, causing TLS connect
   failures and USB-CDC crashes. The 30ms settle gives lwIP time to free FDs.
  */
  tls.stop();
  delay(100);
  tls_set_insecure(tls);
  tls.setTimeout(HTTP_TIMEOUT_MS);

  // Only show TLS logs for direct LLM/chat operations, suppress for background polling
  if (!g_suppress_tls_logs) {
    Serial.printf("[TLS] connecting to %s ...\r\n", host);
  }

  if (!tls.connect(host, 443)) {
    if (!g_suppress_tls_logs) {
      Serial.printf("[TLS] connect failed: %s\r\n", host);
    }
    if (out && out_cap > 0) out[0] = '\0';
    Serial.flush();
    return -1;
  }

  if (!g_suppress_tls_logs) {
    Serial.printf("[TLS] connected — sending request\r\n");
  }

  yield();
  _stream_send_req(tls, host, path, extra_headers, body, body_len);

  // Sending null-byte keepalives until the first byte arrives.
  {
    unsigned long t0 = millis(), last_ka = t0;
    while (!tls.available() && tls.connected() &&
           (millis() - t0) < HTTP_TIMEOUT_MS) {
      usb_keepalive(last_ka);
      delay(1);
    }
  }

  int16_t code = _stream_drain_headers(tls, HTTP_TIMEOUT_MS);
  uint16_t blen = _stream_read_body(tls, out, out_cap);
  unchunk(out, blen);
  tls.stop();
  return code;
}

static int16_t http_req(const char *host_port, const char *path,
                         const char *extra_headers,
                         const char *body, uint16_t body_len,
                         char *out, uint16_t out_cap) {
  /*
  http_req is currently only called from llm_chat() under g_http_busy, but
  a local is safer and costs only 128 bytes of stack for the call duration.
  */
  char host[CFG_S];
  strlcpy(host, host_port, CFG_S);
  uint16_t port = 80;
  char *colon = strrchr(host, ':');
  if (colon) { port = (uint16_t)atoi(colon + 1); *colon = '\0'; }

  g_tcp.stop();
  delay(20);  // let lwIP release the FD cleanly
  Serial.flush();
  if (!g_tcp.connect(host, port)) return -1;
  g_tcp.setTimeout(HTTP_TIMEOUT_MS);
  // Pass 'host' (port stripped) to _stream_send_req — it's used for the Host:
  // header. Passing host_port would include the port number twice on some servers.
  _stream_send_req(g_tcp, host, path, extra_headers, body, body_len);
  g_tcp.flush();

  unsigned long t0 = millis();
  while (!g_tcp.available() && (millis()-t0) < HTTP_TIMEOUT_MS) { yield(); }
  int16_t code = _stream_drain_headers(g_tcp, HTTP_TIMEOUT_MS);
  uint16_t blen = _stream_read_body(g_tcp, out, out_cap);
  unchunk(out, blen);
  g_tcp.stop();
  return code;
}

// ─── Shared TX buffers ────────────────────────────────────────────────────────
static char g_tx_body[JSON_OUT_S];
static char g_tx_esc[JSON_OUT_S];
static char g_tx_auth[CFG_S + 32];
static char g_tx_path[CFG_S];

// ─── LLM chat ────────────────────────────────────────────────────────────────
static char g_session[SESSION_S];
static uint16_t g_session_len = 0;

static void session_append(const char *role, const char *content) {
  uint16_t rlen = strlen(role), clen = strlen(content);
  uint16_t need = rlen + 1 + clen + 1;
  while (g_session_len + need >= SESSION_S && g_session_len > 0) {
    const char *nx = strchr(g_session, '\x02');
    if (!nx) { g_session_len=0; g_session[0]='\0'; break; }
    ++nx; uint16_t drop=(uint16_t)(nx-g_session);
    memmove(g_session, nx, g_session_len-drop+1); g_session_len-=drop;
  }
  memcpy(g_session+g_session_len, role, rlen);  g_session_len+=rlen;
  g_session[g_session_len++]='\x01';
  memcpy(g_session+g_session_len, content, clen); g_session_len+=clen;
  g_session[g_session_len++]='\x02';
  g_session[g_session_len]='\0';
}

static void session_clear() { g_session_len=0; g_session[0]='\0'; }

static bool llm_chat(const char *user_prompt, char *out, uint16_t out_cap) {
  uint16_t pos = 0;
  pos += snprintf(g_tx_body+pos, JSON_OUT_S-pos,
    "{\"model\":\"%s\",\"max_tokens\":%u,\"temperature\":%.2f,\"stream\":false,\"messages\":[",
    g_cfg.llm_model, g_cfg.max_tokens, (double)g_cfg.temperature);

  bool first = true;
  const char *p = g_session;
  while (*p && pos+64 < JSON_OUT_S) {
    const char *re = strchr(p, '\x01'); if (!re) break;
    char role[12]={0}; memcpy(role, p, min((ptrdiff_t)11, re-p)); p=re+1;
    const char *ce = strchr(p, '\x02');
    uint16_t cl = ce ? (uint16_t)(ce-p) : (uint16_t)strlen(p);
    json_escape(p, cl, g_tx_esc, JSON_OUT_S);
    pos += snprintf(g_tx_body+pos, JSON_OUT_S-pos,
      "%s{\"role\":\"%s\",\"content\":\"%s\"}", first?"":",", role, g_tx_esc);
    first=false; p = ce ? ce+1 : p+cl;
  }
  json_escape(user_prompt, strlen(user_prompt), g_tx_esc, JSON_OUT_S);
  pos += snprintf(g_tx_body+pos, JSON_OUT_S-pos,
    "%s{\"role\":\"user\",\"content\":\"%s\"}]}", first?"":",", g_tx_esc);

  char host[CFG_S];  // local — llm_chat is always called under g_http_busy, but static is unnecessary
  snprintf(g_tx_auth, sizeof(g_tx_auth), "Authorization: Bearer %s\r\n", g_cfg.llm_api_key);
  const char *hs = strstr(g_cfg.llm_api_base, "://");
  hs = hs ? hs+3 : g_cfg.llm_api_base;
  const char *ps = strchr(hs, '/');
  if (ps) {
    uint16_t hl=(uint16_t)(ps-hs); memcpy(host,hs,hl); host[hl]='\0';
    snprintf(g_tx_path, CFG_S, "%s/chat/completions", ps);
  } else {
    strlcpy(host,hs,CFG_S); strlcpy(g_tx_path,"/chat/completions",CFG_S);
  }

  #ifdef BOARD_ESP32
    Serial.printf("[LLM] free_heap=%lu bytes\r\n", (unsigned long)ESP.getFreeHeap());
  #elif defined(BOARD_PICO_W)
    Serial.printf("[LLM] free_heap=%lu bytes\r\n", (unsigned long)rp2040.getFreeHeap());
  #endif

  g_http_busy = true;
  int16_t code;
  if (strncmp(g_cfg.llm_api_base, "http://", 7) == 0) {
    // Plain HTTP used for local Ollama; g_tcp is already its own object
    code = http_req(host, g_tx_path, g_tx_auth, g_tx_body, pos, g_http_resp, HTTP_RESP_S);
  } else {
    // HTTPS use the dedicated LLM TLS client (never shared with Telegram/Discord)
    code = https_req(g_tls_llm, host, g_tx_path, g_tx_auth, g_tx_body, pos, g_http_resp, HTTP_RESP_S);
  }
  g_http_busy = false;
  if (code != 200) { snprintf(out,out_cap,"[LLM %d] %.200s",code,g_http_resp); return false; }

  // Strip any leaked HTTP headers before the JSON body
  char *json_start = g_http_resp;
  if (json_start[0] != '{') {
    char *brace = strchr(g_http_resp, '{');
    if (brace) {
      json_start = brace;
      // (silent strip - data is still recovered correctly)
    } else {
      snprintf(out, out_cap, "[parse:no-json] %.120s", g_http_resp);
      Serial.printf("[LLM] parse fail — no JSON in response: %.200s\r\n", g_http_resp);
      return false;
    }
  }

  const char *ch = strstr(json_start,"\"choices\"");
  if (!ch) {
    snprintf(out, out_cap, "[parse:choices] %.120s", json_start);
    Serial.printf("[LLM] parse fail (choices): %.200s\r\n", json_start);
    return false;
  }
  const char *mc = strstr(ch,"\"message\"");
  if (!mc) {
    snprintf(out, out_cap, "[parse:message] %.120s", json_start);
    Serial.printf("[LLM] parse fail (message): %.200s\r\n", json_start);
    return false;
  }
  const char *cc = strstr(mc,"\"content\"");
  if (!cc) {
    snprintf(out, out_cap, "[parse:content] %.120s", json_start);
    Serial.printf("[LLM] parse fail (content): %.200s\r\n", json_start);
    return false;
  }
  /*
  `buf_end` guards `jstr()` against reading past a truncated response buffer.
  If HTTP_RESP_S is hit mid-JSON-string, jstr without this would walk
  into uninitialized memory looking for the closing " → crash.
  */
  const char *buf_end = g_http_resp + HTTP_RESP_S;
  const char *vv = cc + strlen("\"content\"");
  while (*vv==' '||*vv==':') ++vv;
  jstr(vv, out, out_cap, buf_end);

  // Fallback for thinking models
  if (out[0] == '\0') {
    const char *rc = strstr(mc, "\"reasoning_content\"");
    if (!rc) rc = strstr(mc, "\"reasoning\"");
    if (rc) {
      const char *rv = rc + (strncmp(rc, "\"reasoning_content\"", 19) == 0
                              ? strlen("\"reasoning_content\"")
                              : strlen("\"reasoning\""));
      while (*rv==' '||*rv==':') ++rv;
      jstr(rv, out, out_cap, buf_end);
      Serial.println("[LLM] used reasoning field (thinking model)");
    }
  }
  if (out[0] == '\0') strlcpy(out, "[model returned empty response]", out_cap);
  return true;
}

// ─── Built-in tools ───────────────────────────────────────────────────────────
static char g_tool_result[512];

static void tool_dispatch(const char *name, const char *args) {
  if (!strcmp(name,"message")) {
    Serial.printf("[agent] %s\r\n", args);
    strlcpy(g_tool_result,"sent",512);
  } else if (!strcmp(name,"get_wifi_info")) {
    snprintf(g_tool_result,512,"{\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d}",
      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else if (!strcmp(name,"get_time")) {
    snprintf(g_tool_result,512,"{\"uptime_ms\":%lu}",millis());
  } else if (!strcmp(name,"set_config")) {
    char key[48]={0}, val[CFG_S]={0};
    const char *kp=jfind(args,"key"), *vp=jfind(args,"value");
    if (kp) jstr(kp,key,48);
    if (vp) jstr(vp,val,CFG_S);
    if      (!strcmp(key,"llm_model"))      strlcpy(g_cfg.llm_model,val,64);
    else if (!strcmp(key,"llm_api_key"))    strlcpy(g_cfg.llm_api_key,val,CFG_S);
    else if (!strcmp(key,"llm_api_base"))   strlcpy(g_cfg.llm_api_base,val,CFG_S);
    else if (!strcmp(key,"llm_provider"))   strlcpy(g_cfg.llm_provider,val,32);
    else if (!strcmp(key,"wifi_ssid"))      strlcpy(g_cfg.wifi_ssid,val,CFG_S);
    else if (!strcmp(key,"wifi_pass"))      strlcpy(g_cfg.wifi_pass,val,CFG_S);
    else if (!strcmp(key,"tg_token"))  { strlcpy(g_cfg.telegram.token,val,CFG_S); g_cfg.telegram.enabled=true; }
    else if (!strcmp(key,"dc_token"))  { strlcpy(g_cfg.discord.token,val,CFG_S);  g_cfg.discord.enabled=true; }
    else if (!strcmp(key,"dc_channel_id"))  strlcpy(g_cfg.discord_channel_id,val,ALLOW_ID_LEN);
    cfg_save();
    snprintf(g_tool_result,512,"set %s ok",key);
  } else if (!strcmp(name,"get_config")) {
    snprintf(g_tool_result,512,
      "{\"model\":\"%s\",\"provider\":\"%s\","
      "\"tg\":%s,\"dc\":%s,\"uptime\":%lu}",
      g_cfg.llm_model, g_cfg.llm_provider,
      g_cfg.telegram.enabled?"true":"false",
      g_cfg.discord.enabled?"true":"false",
      millis());
  } else if (!strcmp(name,"reset_session")) {
    session_clear(); strlcpy(g_tool_result,"cleared",512);
  } else {
    snprintf(g_tool_result,512,"[tool %s not on MCU]",name);
  }
}

// ─── Agentic loop ────────────────────────────────────────────────────────────
static char g_llm_out[RESP_S];

static const char *agent_run(const char *user_input) {
  static char combined[PROMPT_S+512];
  strlcpy(combined, user_input, sizeof(combined));
  for (uint8_t iter=0; iter<g_cfg.max_tool_iters; ++iter) {
    if (!llm_chat(combined, g_llm_out, RESP_S)) return g_llm_out;
    session_append("user",      iter==0 ? user_input : "[tool_result]");
    session_append("assistant", g_llm_out);
    const char *tc = strstr(g_llm_out,"<tool:");
    if (!tc) return g_llm_out;
    const char *ns=tc+6, *ne=strchr(ns,'>'); if (!ne) break;
    char tname[48]={0}; memcpy(tname,ns,min((ptrdiff_t)47,ne-ns));
    const char *as=ne+1, *ae=strstr(as,"</tool>");
    uint16_t al=ae?(uint16_t)(ae-as):0;
    static char targs[512]; memcpy(targs,as,min(al,(uint16_t)511)); targs[min(al,(uint16_t)511)]='\0';
    tool_dispatch(tname,targs);
    Serial.printf("[tool:%s] %s\r\n",tname,g_tool_result);
    snprintf(combined,sizeof(combined),"[Tool %s]: %s",tname,g_tool_result);
  }
  return g_llm_out;
}

// ─── Telegram long-polling ───────────────────────────────────────────────────
static uint32_t g_tg_last_ms = 0;

static int16_t tg_send(const char *chat_id, const char *text) {
  static char tg_esc[JSON_OUT_S];
  static char tg_path[CFG_S];
  static char tg_body[JSON_OUT_S];

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

    // Suppress TLS logs for background Telegram operations
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

static void tg_poll() {
  if (!g_cfg.telegram.enabled || !g_cfg.telegram.token[0]) return;
  if ((millis() - g_tg_last_ms) < TG_POLL_MS) return;
  if (g_http_busy) return;
  g_tg_last_ms = millis();

  snprintf(g_tx_path, CFG_S, "/bot%s/getUpdates?offset=%lld&timeout=1&limit=5",
           g_cfg.telegram.token, (long long)g_tg_offset);

  // Suppress TLS logs for background polling
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
      // Use id_from_int64: detects overflow, zeros buffer on failure so
      // is_allowed() denies safely rather than matching a truncated ID.
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
      Serial.printf("[Telegram] BLOCKED — from_id=%s not in allow list (count=%u)\r\n",
                    from_id, g_cfg.telegram.allow_count);
      ++p; continue;
    }

    // g_http_busy is set/cleared inside llm_chat() via agent_run()
    const char *reply = agent_run(text);
    Serial.printf("[Telegram] replying (%u chars) → chat %s\r\n",
                  (unsigned)strlen(reply), chat_id);

    /*
    Give lwIP a brief TIME_WAIT gap between closing the LLM TLS
    session (g_tls_llm) and opening the Telegram send session (g_tls_tg).
    Even though they are separate objects, the underlying TCP stack on both
    ESP32 and Pico W benefits from a small inter-connect delay when the
    two connections are opened within milliseconds of each other.
    */
    delay(TLS_SETTLE_MS);

    int16_t sc = tg_send(chat_id, reply);
    if (sc != 200) {
      Serial.printf("[Telegram] send FAILED code=%d resp=%.100s\r\n", sc, g_http_resp);
    }
    ++p;
  }
}

// ─── Discord HTTP polling ────────────────────────────────────────────────────
static uint32_t g_dc_last_ms = 0;

static int16_t dc_send(const char *text) {
  if (!g_cfg.discord_channel_id[0]) return 0;

  static char dc_esc[JSON_OUT_S];
  static char dc_auth[CFG_S + 32];
  static char dc_path[CFG_S];
  static char dc_body[JSON_OUT_S];

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

    // Suppress TLS logs for background Discord operations
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

  // Suppress TLS logs for background polling
  g_suppress_tls_logs = true;
  // Discord poll uses its own dedicated TLS client
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
    while (*id_v==' '||*id_v==':') ++id_v;
    // id_from_str zeroes msg_id on truncation — strcmp then treats it as empty,
    // which makes is_new=false and the message is skipped rather than mishandled.
    id_from_str(id_v, msg_id, sizeof(msg_id));

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

    // TLS_SETTLE_MS gap as Telegram — give lwIP time between
    // closing the LLM TLS session (g_tls_llm) and opening the Discord send
    // session (g_tls_dc).
    delay(TLS_SETTLE_MS);

    dc_send(reply);
    ++p;
  }
}

// ─── Heartbeat ───────────────────────────────────────────────────────────────
static uint32_t g_hb_last = 0;

static void heartbeat_check() {
  if ((millis() - g_hb_last) < g_cfg.heartbeat_ms) return;
  g_hb_last = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[heartbeat] WiFi lost — attempting reconnect...");
    wifi_connect(10);
    return;
  }

  Serial.println("[heartbeat] Running...");
  const char *r = agent_run(
    "You are a scheduled heartbeat on an MCU. Report uptime and WiFi status in one short sentence.");
  Serial.printf("[heartbeat] %s\r\n", r);
}

// ─── UART shell ───────────────────────────────────────────────────────────────
static char     g_cmd[CMD_S];
static uint16_t g_cmd_len = 0;

static void shell_prompt() { Serial.print("\r\n\033[1;32mfemtoclaw>\033[0m "); }

static void shell_run(const char *line) {
  if (!strcmp(line,"help")||!strcmp(line,"?")) {
    Serial.print(
      "\r\n┌─ FemtoClaw MCU Shell ─────────────────────────────────────┐\r\n"
      "│  help / ?                  — Show this message                  │\r\n"
      "│  status                    — WiFi, channels, uptime        │\r\n"
      "│  wifi <ssid> <pw>          — save WiFi credentials         │\r\n"
      "│  connect                   — (re)connect WiFi              │\r\n"
      "│  set <key> <value>         — update any config key         │\r\n"
      "│  show config               — print all settings            │\r\n"
      "│  tg token <BOT_TOKEN>      — set Telegram bot token        │\r\n"
      "│  tg allow <user_id>        — add allowed Telegram user     │\r\n"
      "│  tg allow list             — show current allow list       │\r\n"
      "│  tg allow clear            — clear Telegram allow list     │\r\n"
      "│  tg enable / tg disable    — toggle Telegram channel       │\r\n"
      "│  dc token <BOT_TOKEN>      — set Discord bot token         │\r\n"
      "│  dc channel <CHANNEL_ID>   — set Discord channel to watch  │\r\n"
      "│  dc allow <user_id>        — add allowed Discord user      │\r\n"
      "│  dc enable / dc disable    — toggle Discord channel        │\r\n"
      "│  diag                      — show parsed LLM host/path/heap │\r\n"
      "│  chat <message>            — send to LLM agent via UART    │\r\n"
      "│  reset session             — clear conversation history     │\r\n"
      "│  reboot                    — restart MCU                   │\r\n"
      "└────────────────────────────────────────────────────────────┘\r\n");

  } else if (!strcmp(line,"status")) {
    Serial.printf(
      "\r\n  Board     : " PLATFORM_NAME "\r\n"
      "  WiFi      : %s / %s\r\n"
      "  IP        : %s  RSSI %d dBm\r\n"
      "  Provider  : %s  Model : %s\r\n"
      "  Telegram  : %s  (token: %s  allow: %u entries)\r\n"
      "  Discord   : %s  (channel: %s  allow: %u entries)\r\n"
      "  TG offset : %lld\r\n"
      "  Uptime    : %lu ms\r\n",
      g_cfg.wifi_ssid[0] ? g_cfg.wifi_ssid : "(none)",
      WiFi.status()==WL_CONNECTED ? "connected":"disconnected",
      WiFi.status()==WL_CONNECTED ? WiFi.localIP().toString().c_str():"N/A",
      WiFi.status()==WL_CONNECTED ? WiFi.RSSI() : 0,
      g_cfg.llm_provider, g_cfg.llm_model,
      g_cfg.telegram.enabled ? "ENABLED":"disabled",
      g_cfg.telegram.token[0] ? "set":"(none)",
      (unsigned)g_cfg.telegram.allow_count,
      g_cfg.discord.enabled ? "ENABLED":"disabled",
      g_cfg.discord_channel_id[0] ? g_cfg.discord_channel_id:"(none)",
      (unsigned)g_cfg.discord.allow_count,
      (long long)g_tg_offset,
      millis());

  } else if (!strncmp(line,"wifi ",5)) {
    char *rest=(char*)line+5, *sp=strchr(rest,' ');
    if (!sp) { Serial.println("Usage: wifi <ssid> <password>"); return; }
    *sp='\0'; strlcpy(g_cfg.wifi_ssid,rest,CFG_S); strlcpy(g_cfg.wifi_pass,sp+1,CFG_S);
    cfg_save(); Serial.println("Saved. Type 'connect' to apply.");

  } else if (!strcmp(line,"connect")) {
    wifi_connect();

  } else if (!strncmp(line,"set ",4)) {
    char *rest=(char*)line+4, *sp=strchr(rest,' ');
    if (!sp) { Serial.println("Usage: set <key> <value>"); return; }
    *sp='\0';
    static char args[CFG_S+64];
    snprintf(args,sizeof(args),"{\"key\":\"%s\",\"value\":\"%s\"}",rest,sp+1);
    tool_dispatch("set_config",args);
    Serial.println(g_tool_result);

  } else if (!strcmp(line,"show config")) {
    Serial.printf(
      "\r\n  wifi_ssid    : %s\r\n"
      "  llm_provider : %s\r\n"
      "  llm_api_base : %s\r\n"
      "  llm_model    : %s\r\n"
      "  max_tokens   : %u\r\n"
      "  temperature  : %.2f\r\n"
      "  tg_enabled   : %s\r\n"
      "  tg_token     : %s\r\n"
      "  tg_allow_cnt : %u\r\n"
      "  dc_enabled   : %s\r\n"
      "  dc_channel   : %s\r\n"
      "  dc_allow_cnt : %u\r\n",
      g_cfg.wifi_ssid, g_cfg.llm_provider,
      g_cfg.llm_api_base, g_cfg.llm_model,
      g_cfg.max_tokens, (double)g_cfg.temperature,
      g_cfg.telegram.enabled?"yes":"no",
      g_cfg.telegram.token[0] ? "[set]":"(none)",
      (unsigned)g_cfg.telegram.allow_count,
      g_cfg.discord.enabled?"yes":"no",
      g_cfg.discord_channel_id[0] ? g_cfg.discord_channel_id:"(none)",
      (unsigned)g_cfg.discord.allow_count);

  // ── Telegram sub-commands ──────────────────────────────────────────
  } else if (!strncmp(line,"tg token ",9)) {
    strlcpy(g_cfg.telegram.token, line+9, CFG_S);
    cfg_save(); Serial.println("Telegram token saved.");
  } else if (!strcmp(line,"tg allow list")) {
    if (g_cfg.telegram.allow_count == 0) {
      Serial.println("Telegram allow list: (empty — all users accepted)");
    } else {
      Serial.printf("Telegram allow list (%u):\r\n", g_cfg.telegram.allow_count);
      for (uint8_t i = 0; i < g_cfg.telegram.allow_count; ++i)
        Serial.printf("  [%u] %s\r\n", i, g_cfg.telegram.allow_from[i]);
    }
  } else if (!strcmp(line,"tg allow clear")) {
    g_cfg.telegram.allow_count = 0;
    cfg_save(); Serial.println("Telegram allow list cleared (all users now accepted).");
  } else if (!strncmp(line,"tg allow ",9)) {
    if (g_cfg.telegram.allow_count < ALLOW_LIST_MAX) {
      const char *id_str = line + 9;
      if (strlen(id_str) >= ALLOW_ID_LEN) {
        Serial.printf("[!] ID too long (%u chars, max %u): '%s'\r\n",
                      (unsigned)strlen(id_str), (unsigned)(ALLOW_ID_LEN - 1), id_str);
      } else {
        strlcpy(g_cfg.telegram.allow_from[g_cfg.telegram.allow_count++], id_str, ALLOW_ID_LEN);
        cfg_save(); Serial.printf("Added Telegram allow: %s\r\n", id_str);
      }
    } else Serial.println("Allow list full.");
  } else if (!strcmp(line,"tg enable")) {
    g_cfg.telegram.enabled=true; cfg_save(); Serial.println("Telegram enabled.");
  } else if (!strcmp(line,"tg disable")) {
    g_cfg.telegram.enabled=false; cfg_save(); Serial.println("Telegram disabled.");

  // ── Discord sub-commands ───────────────────────────────────────────
  } else if (!strncmp(line,"dc token ",9)) {
    strlcpy(g_cfg.discord.token, line+9, CFG_S);
    cfg_save(); Serial.println("Discord token saved.");
  } else if (!strncmp(line,"dc channel ",11)) {
    strlcpy(g_cfg.discord_channel_id, line+11, ALLOW_ID_LEN);
    cfg_save(); Serial.printf("Discord channel: %s\r\n", g_cfg.discord_channel_id);
  } else if (!strncmp(line,"dc allow ",9)) {
    if (g_cfg.discord.allow_count < ALLOW_LIST_MAX) {
      const char *id_str = line + 9;
      if (strlen(id_str) >= ALLOW_ID_LEN) {
        Serial.printf("[!] ID too long (%u chars, max %u): '%s'\r\n",
                      (unsigned)strlen(id_str), (unsigned)(ALLOW_ID_LEN - 1), id_str);
      } else {
        strlcpy(g_cfg.discord.allow_from[g_cfg.discord.allow_count++], id_str, ALLOW_ID_LEN);
        cfg_save(); Serial.printf("Added Discord allow: %s\r\n", id_str);
      }
    } else Serial.println("Allow list full.");
  } else if (!strcmp(line,"dc enable")) {
    g_cfg.discord.enabled=true; cfg_save(); Serial.println("Discord enabled.");
  } else if (!strcmp(line,"dc disable")) {
    g_cfg.discord.enabled=false; cfg_save(); Serial.println("Discord disabled.");

  // ── Chat & session ─────────────────────────────────────────────────
  } else if (!strcmp(line,"diag")) {
    static char dhost[CFG_S];
    const char *hs = strstr(g_cfg.llm_api_base, "://");
    hs = hs ? hs+3 : g_cfg.llm_api_base;
    const char *ps = strchr(hs, '/');
    if (ps) {
      uint16_t hl=(uint16_t)(ps-hs); memcpy(dhost,hs,hl); dhost[hl]='\0';
    } else { strlcpy(dhost,hs,CFG_S); }
    bool is_http = strncmp(g_cfg.llm_api_base,"http://",7)==0;
    Serial.printf("\r\n  api_base : %s\r\n"
                  "  host     : %s\r\n"
                  "  path     : %s/chat/completions\r\n"
                  "  scheme   : %s\r\n"
                  "  wifi     : %s\r\n"
#ifdef BOARD_ESP32
                  "  free_heap: %lu bytes\r\n",
#else
                  "  free_heap: n/a (Pico W)\r\n",
#endif
      g_cfg.llm_api_base, dhost,
      ps ? ps : "/",
      is_http ? "HTTP (plain)" : "HTTPS (TLS)",
      WiFi.status()==WL_CONNECTED ? WiFi.localIP().toString().c_str() : "disconnected"
#ifdef BOARD_ESP32
      , (unsigned long)ESP.getFreeHeap()
#endif
      );

  } else if (!strncmp(line,"chat ",5)) {
    if (WiFi.status()!=WL_CONNECTED) { Serial.println("[!] Not connected."); return; }
    if (g_http_busy) { Serial.println("[!] Network busy, try again shortly."); return; }
    Serial.println("[LLM] Thinking...");
    const char *r = agent_run(line+5);
    Serial.printf("\r\n[femtoclaw] %s\r\n", r);
  } else if (!strcmp(line,"reset session")) {
    session_clear(); Serial.println("Session cleared.");
  } else if (!strcmp(line,"reboot")) {
    Serial.println("Rebooting..."); delay(200);
#ifdef BOARD_ESP32
    ESP.restart();
#elif defined(BOARD_PICO_W)
    rp2040.reboot();
#endif
  } else if (line[0]) {
    Serial.printf("Unknown: '%s'  (type 'help')\r\n", line);
  }
}

static void shell_byte(uint8_t c) {
  if (g_http_busy) return;
  if (c == '\n' || c == '\r') {
    g_cmd[g_cmd_len] = '\0';
    if (g_cmd_len > 0) { Serial.print("\r\n"); shell_run(g_cmd); }
    g_cmd_len = 0;
    shell_prompt();
  } else if (c == 127 || c == 8) {
    if (g_cmd_len > 0) { --g_cmd_len; Serial.print("\b \b"); }
  } else if (g_cmd_len + 1 < CMD_S) {
    g_cmd[g_cmd_len++] = (char)c;
    Serial.write(c);
  }
}

// ─── Arduino entry points ─────────────────────────────────────────────────────
void setup() {
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  // ── ESP32-C3/C6 native USB boot sequence ─────────────────────────────────
  // WiFi.mode(WIFI_STA) MUST be called before Serial.begin()
  // on ESP32-C3 with native USB-CDC.
  //
  // Cause: the USB-Serial/JTAG controller and the WiFi RF subsystem share
  // the same internal clock domain on ESP32-C3. If WiFi is initialized while
  // USB-CDC is already active, the two subsystems conflict and the chip triggers
  // RTC_SW_SYS_RST (saved PC 0x403cf94c) producing an infinite boot loop.
  //
  // Note: WiFi.mode() alone does not connect — it just configures the radio
  // mode. No SSID or password is used here.
  WiFi.mode(WIFI_STA);
#endif

  Serial.begin(UART_BAUD);

#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  // Wait up to 3s for host to open port; continue headless after timeout.
  // delay(10) ensures the FreeRTOS idle task runs each iteration.
  {
    uint32_t t = millis();
    while (!Serial && (millis() - t) < 3000) delay(10);
    delay(150);
  }
#else
  delay(300);
#endif

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  cfg_load();

  Serial.println(
    "\r\n\033[1;35m"
    "  ███████╗███████╗███╗   ███╗████████╗ ██████╗  ██████╗██╗      █████╗ ██╗    ██╗\r\n"
    "  ██╔════╝██╔════╝████╗ ████║╚══██╔══╝██╔═══██╗██╔════╝██║     ██╔══██╗██║    ██║\r\n"
    "  █████╗  █████╗  ██╔████╔██║   ██║   ██║   ██║██║     ██║     ███████║██║ █╗ ██║\r\n"
    "  ██╔══╝  ██╔══╝  ██║╚██╔╝██║   ██║   ██║   ██║██║     ██║     ██╔══██║██║███╗██║\r\n"
    "  ██║     ███████╗██║ ╚═╝ ██║   ██║   ╚██████╔╝╚██████╗███████╗██║  ██║╚███╔███╔╝\r\n"
    "  ╚═╝     ╚══════╝╚═╝     ╚═╝   ╚═╝    ╚═════╝  ╚═════╝╚══════╝╚═╝  ╚═╝ ╚══╝╚══╝\r\n"
    "\033[0m"
    "  FemtoClaw v29 — AI Assistant for MCU · " PLATFORM_NAME " · Telegram & Discord\r\n"
    "  Developed by: Al Mahmud Samiul\r\n"
    "  Type 'help' for commands.\r\n");

  if (g_cfg.wifi_ssid[0]) wifi_connect();
  else Serial.println("[!] No WiFi set. Use: wifi <ssid> <pass>  then  connect");

  if (g_cfg.telegram.enabled)
    Serial.printf("[Telegram] Enabled — polling every %lus  allow_count=%u\r\n",
                  (unsigned long)(TG_POLL_MS/1000), (unsigned)g_cfg.telegram.allow_count);
  if (g_cfg.discord.enabled)
    Serial.println("[Discord]  Channel enabled — polling started.");

  digitalWrite(LED_PIN, LOW);
  shell_prompt();
}

/*
 * loop() — cooperative main loop.
 *
 * USB-CDC reconnect handling (ESP32-C3 / C6 native USB):
 * ────────────────────────────────────────────────────────
 * The C3/C6 USB-CDC fires disconnect + reconnect on every terminal open/close
 * and after each flash. Without debouncing, shell_prompt() would fire mid-LLM
 * response, corrupting output and potentially executing a half-buffered command.
 *
 * v6 additions on top of the existing debounce logic:
 *   • On reconnect, shell_prompt() is only printed if !g_http_busy AND
 *     g_cmd_len == 0 — prevents the prompt appearing mid-response.
 *   • If g_http_busy at reconnect time, a "[busy]" status line is printed
 *     instead so the user knows the board is working, not hung.
 *   • RX bytes arriving during a busy reconnect window are discarded by
 *     shell_byte()'s own g_http_busy guard, so no phantom commands execute.
 *
 * Hardware UART boards (Pico W, ESP32 with hardware UART):
 *   The #else branch below runs unconditionally no debounce needed because
 *   hardware UARTs don't disconnect on open/close.
*/
void loop() {
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  static bool     s_usb_state       = false;
  static bool     s_usb_candidate   = false;
  static uint32_t s_usb_debounce_ms = 0;
  static constexpr uint32_t USB_DEBOUNCE_MS = 80;

  bool raw = (bool)Serial;
  if (raw != s_usb_candidate) {
    s_usb_candidate   = raw;
    s_usb_debounce_ms = millis();
  }

  if ((millis() - s_usb_debounce_ms) >= USB_DEBOUNCE_MS && s_usb_candidate != s_usb_state) {
    bool prev    = s_usb_state;
    s_usb_state  = s_usb_candidate;

    if (s_usb_state && !prev) {
      // USB reconnected — settle the PHY, then decide how to re-prompt
      delay(50);
      if (g_http_busy) {
        // LLM / Telegram / Discord request is in-flight, tell the user
        // but do NOT print the normal prompt (it would appear mid-response)
        Serial.println("\r\n[femtoclaw] reconnected — waiting for network response...");
      } else if (g_cmd_len == 0) {
        // Idle and buffer is empty — safe to re-prompt normally
        shell_prompt();
      }
      // If g_cmd_len > 0, the user had a partial command typed before
      // disconnect. The buffer is left intact and do not re-prompt so
      // they can continue typing (their previous chars are lost from the
      // terminal's perspective, but the MCU buffer still has them).
    } else {
      // USB disconnected : flush any pending TX so the host sees clean output
      Serial.flush();
    }
  }

  // Only process RX while USB is stably connected.
  // shell_byte() will additionally gate command execution on !g_http_busy.
  if (s_usb_state) {
    while (Serial.available()) shell_byte((uint8_t)Serial.read());
  }
#else
  // Hardware UART (Pico W, ESP32 with UART0): no disconnect events, read freely.
  // shell_byte() still guards execution on !g_http_busy.
  while (Serial.available()) shell_byte((uint8_t)Serial.read());
#endif

  if (WiFi.status() == WL_CONNECTED && !g_http_busy) {
    tg_poll();
    dc_poll();
    heartbeat_check();
  }
  yield();
}
