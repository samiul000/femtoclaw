/*
 * ─────────────────────────────────────────────────────────────
 *                      UART shell
 *
 * SINGLE-TU HEADER — included from femtoclaw_mcu.cpp only.
 * All symbols must be static.
 * ─────────────────────────────────────────────────────────────
 */

#pragma once

// ─── Board push state machine ─────────────────────────────────────────────────
/*
 * The GUI sends [CONTROL].md base64-encoded in chunks:
 *   board push begin
 *   board push chunk <base64_fragment>   (repeated)
 *   board push end
 */
static char     g_push_buf[6144];
static uint16_t g_push_len   = 0;
static bool     g_push_active = false;

// ─── Shell state ──────────────────────────────────────────────────────────────
static char     g_cmd[CMD_S];
static uint16_t g_cmd_len = 0;

static void shell_prompt() {
    Serial.print("\r\n\033[1;32mfemtoclaw>\033[0m ");
}

// ─── shell_run ────────────────────────────────────────────────────────────────
static void shell_run(const char *line) {

    // ── Help ───────────────────────────────────────────────────────────
    if (!strcmp(line,"help") || !strcmp(line,"?")) {
        Serial.print(
            "\r\n┌─ FemtoClaw MCU Shell ─────────────────────────────────────────┐\r\n"
            "│  help / ?                     — this message                       │\r\n"
            "│  status                       — WiFi, channels, uptime            │\r\n"
            "│  wifi <ssid> <pw>             — save WiFi credentials             │\r\n"
            "│  connect                      — (re)connect WiFi                  │\r\n"
            "│  set <key> <value>            — update any config key             │\r\n"
            "│  show config                  — print all settings                │\r\n"
            "│  tg token <TOKEN>             — set Telegram bot token            │\r\n"
            "│  tg allow <user_id>           — add allowed Telegram user         │\r\n"
            "│  tg allow list                — show Telegram allow list          │\r\n"
            "│  tg allow clear               — clear Telegram allow list         │\r\n"
            "│  tg enable / tg disable       — toggle Telegram channel           │\r\n"
            "│  dc token <TOKEN>             — set Discord bot token             │\r\n"
            "│  dc channel <CHANNEL_ID>      — set Discord channel               │\r\n"
            "│  dc allow <user_id>           — add allowed Discord user          │\r\n"
            "│  dc enable / dc disable       — toggle Discord channel            │\r\n"
            "│  diag                         — LLM host/path/heap diagnostics    │\r\n"
            "│  chat <message>               — send to LLM agent                 │\r\n"
            "│  reset session                — clear conversation history         │\r\n"
            "│  reboot                       — restart MCU                       │\r\n"
            "├─ Board & Hardware ────────────────────────────────────────────────┤\r\n"
            "│  board push begin/chunk/end   — push [CONTROL].md (base64 chunks)  │\r\n"
            "│  board show                   — print stored board config          │\r\n"
            "│  board reset                  — clear config, set all outputs LOW  │\r\n"
            "│  gpio get <pin>               — read GPIO (0 or 1)                 │\r\n"
            "│  gpio set <pin> <0|1>         — set GPIO output                    │\r\n"
            "│  gpio mode <pin> <mode>       — change pin mode                    │\r\n"
            "│  adc read <pin>               — read ADC (0-4095)                  │\r\n"
            "│  serial write <n> <data>      — write to named serial port         │\r\n"
            "│  serial read <n>              — read from named serial port        │\r\n"
            "│  servo set <name> <angle>     — set servo angle                    │\r\n"
            "│  pwm set <name> <duty>        — set PWM duty (0-255)               │\r\n"
            "└────────────────────────────────────────────────────────────────────┘\r\n");

    // ── Status ─────────────────────────────────────────────────────────
    } else if (!strcmp(line,"status")) {
        Serial.printf(
            "\r\n  Board     : " PLATFORM_NAME "\r\n"
            "  WiFi      : %s / %s\r\n"
            "  IP        : %s  RSSI %d dBm\r\n"
            "  Provider  : %s  Model : %s\r\n"
            "  Telegram  : %s  (token: %s  allow: %u)\r\n"
            "  Discord   : %s  (channel: %s  allow: %u)\r\n"
            "  TG offset : %lld\r\n"
            "  GPIO/UART/ADC/I2C/SPI/Servo/PWM: %u/%u/%u/%u/%u/%u/%u\r\n"
            "  Uptime    : %lu ms\r\n",
            g_cfg.wifi_ssid[0] ? g_cfg.wifi_ssid : "(none)",
            WiFi.status()==WL_CONNECTED ? "connected" : "disconnected",
            WiFi.status()==WL_CONNECTED ? WiFi.localIP().toString().c_str() : "N/A",
            WiFi.status()==WL_CONNECTED ? WiFi.RSSI() : 0,
            g_cfg.llm_provider, g_cfg.llm_model,
            g_cfg.telegram.enabled ? "ENABLED" : "disabled",
            g_cfg.telegram.token[0] ? "set" : "(none)",
            (unsigned)g_cfg.telegram.allow_count,
            g_cfg.discord.enabled ? "ENABLED" : "disabled",
            g_cfg.discord_channel_id[0] ? g_cfg.discord_channel_id : "(none)",
            (unsigned)g_cfg.discord.allow_count,
            (long long)g_tg_offset,
            g_board_pin_count, g_board_serial_count, g_board_adc_count,
            g_board_i2c_count, g_board_spi_count,
            g_board_servo_count, g_board_pwm_count,
            millis());

    // ── WiFi ───────────────────────────────────────────────────────────
    } else if (!strncmp(line,"wifi ",5)) {
        char *rest=(char*)line+5, *sp=strchr(rest,' ');
        if (!sp) { Serial.println("Usage: wifi <ssid> <password>"); return; }
        *sp='\0';
        strlcpy(g_cfg.wifi_ssid, rest, CFG_S);
        strlcpy(g_cfg.wifi_pass, sp+1, CFG_S);
        cfg_save();
        Serial.println("Saved. Type 'connect' to apply.");

    } else if (!strcmp(line,"connect")) {
        wifi_connect();

    // ── Set config ─────────────────────────────────────────────────────
    } else if (!strncmp(line,"set ",4)) {
        char *rest=(char*)line+4, *sp=strchr(rest,' ');
        if (!sp) { Serial.println("Usage: set <key> <value>"); return; }
        *sp='\0';
        static char args[LLM_KEY+64];
        snprintf(args, sizeof(args), "{\"key\":\"%s\",\"value\":\"%s\"}", rest, sp+1);
        tool_dispatch("set_config", args);
        Serial.println(g_tool_result);

    } else if (!strcmp(line,"show config")) {
        Serial.printf(
            "\r\n  wifi_ssid    : %s\r\n"
            "  llm_provider : %s\r\n"
            "  llm_api_base : %s\r\n"
            "  llm_model    : %s\r\n"
            "  max_tokens   : %u\r\n"
            "  temperature  : %.2f\r\n"
            "  max_iters    : %u\r\n"
            "  heartbeat_ms : %lu\r\n"
            "  tg_enabled   : %s\r\n"
            "  tg_token     : %s\r\n"
            "  tg_allow_cnt : %u\r\n"
            "  dc_enabled   : %s\r\n"
            "  dc_channel   : %s\r\n"
            "  dc_allow_cnt : %u\r\n",
            g_cfg.wifi_ssid, g_cfg.llm_provider,
            g_cfg.llm_api_base, g_cfg.llm_model,
            g_cfg.max_tokens, (double)g_cfg.temperature,
            g_cfg.max_tool_iters, (unsigned long)g_cfg.heartbeat_ms,
            g_cfg.telegram.enabled?"yes":"no",
            g_cfg.telegram.token[0] ? "[set]" : "(none)",
            (unsigned)g_cfg.telegram.allow_count,
            g_cfg.discord.enabled?"yes":"no",
            g_cfg.discord_channel_id[0] ? g_cfg.discord_channel_id : "(none)",
            (unsigned)g_cfg.discord.allow_count);

    // ── Telegram sub-commands ──────────────────────────────────────────
    } else if (!strncmp(line,"tg token ",9)) {
        strlcpy(g_cfg.telegram.token, line+9, CFG_S);
        cfg_save(); Serial.println("Telegram token saved.");

    } else if (!strcmp(line,"tg allow list")) {
        if (g_cfg.telegram.allow_count == 0)
            Serial.println("Telegram allow list: (empty : all users accepted)");
        else {
            Serial.printf("Telegram allow list (%u):\r\n", g_cfg.telegram.allow_count);
            for (uint8_t i = 0; i < g_cfg.telegram.allow_count; ++i)
                Serial.printf("  [%u] %s\r\n", i, g_cfg.telegram.allow_from[i]);
        }
    } else if (!strcmp(line,"tg allow clear")) {
        g_cfg.telegram.allow_count = 0;
        cfg_save(); Serial.println("Telegram allow list cleared.");
    } else if (!strncmp(line,"tg allow ",9)) {
        const char *id_str = line + 9;
        if (g_cfg.telegram.allow_count >= ALLOW_LIST_MAX)
            Serial.println("Allow list full.");
        else if (strlen(id_str) >= ALLOW_ID_LEN)
            Serial.printf("[!] ID too long (%u chars, max %u)\r\n",
                          (unsigned)strlen(id_str), (unsigned)(ALLOW_ID_LEN - 1));
        else {
            strlcpy(g_cfg.telegram.allow_from[g_cfg.telegram.allow_count++], id_str, ALLOW_ID_LEN);
            cfg_save(); Serial.printf("Added Telegram allow: %s\r\n", id_str);
        }
    } else if (!strcmp(line,"tg enable"))  { g_cfg.telegram.enabled=true;  cfg_save(); Serial.println("Telegram enabled.");
    } else if (!strcmp(line,"tg disable")) { g_cfg.telegram.enabled=false; cfg_save(); Serial.println("Telegram disabled.");

    // ── Discord sub-commands ───────────────────────────────────────────
    } else if (!strncmp(line,"dc token ",9)) {
        strlcpy(g_cfg.discord.token, line+9, CFG_S);
        cfg_save(); Serial.println("Discord token saved.");
    } else if (!strncmp(line,"dc channel ",11)) {
        strlcpy(g_cfg.discord_channel_id, line+11, ALLOW_ID_LEN);
        cfg_save(); Serial.printf("Discord channel: %s\r\n", g_cfg.discord_channel_id);
    } else if (!strncmp(line,"dc allow ",9)) {
        const char *id_str = line + 9;
        if (g_cfg.discord.allow_count >= ALLOW_LIST_MAX)
            Serial.println("Allow list full.");
        else if (strlen(id_str) >= ALLOW_ID_LEN)
            Serial.printf("[!] ID too long (%u chars, max %u)\r\n",
                          (unsigned)strlen(id_str), (unsigned)(ALLOW_ID_LEN - 1));
        else {
            strlcpy(g_cfg.discord.allow_from[g_cfg.discord.allow_count++], id_str, ALLOW_ID_LEN);
            cfg_save(); Serial.printf("Added Discord allow: %s\r\n", id_str);
        }
    } else if (!strcmp(line,"dc enable"))  { g_cfg.discord.enabled=true;  cfg_save(); Serial.println("Discord enabled.");
    } else if (!strcmp(line,"dc disable")) { g_cfg.discord.enabled=false; cfg_save(); Serial.println("Discord disabled.");

    // ── Diagnostics ────────────────────────────────────────────────────
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

    // ── Chat ───────────────────────────────────────────────────────────
    } else if (!strncmp(line,"chat ",5)) {
        if (WiFi.status() != WL_CONNECTED) { Serial.println("[!] Not connected."); return; }
        if (g_http_busy) { Serial.println("[!] Network busy."); return; }
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

    // ── Board push ─────────────────────────────────────────────────────
    } else if (!strcmp(line, "board push begin")) {
        g_push_len    = 0;
        g_push_buf[0] = '\0';
        g_push_active = true;
        Serial.println("[Board] Push started : send 'board push chunk <b64>' then 'board push end'.");

    } else if (!strncmp(line, "board push chunk ", 17)) {
        if (!g_push_active) {
            Serial.println("[Board] ERROR: send 'board push begin' first.");
        } else {
            const char *chunk = line + 17;
            uint16_t clen = (uint16_t)strlen(chunk);
            if (g_push_len + clen + 1 < sizeof(g_push_buf)) {
                memcpy(g_push_buf + g_push_len, chunk, clen);
                g_push_len += clen;
                g_push_buf[g_push_len] = '\0';
            } else {
                Serial.println("[Board] ERROR: push buffer full --> aborting.");
                g_push_active = false;
                g_push_len    = 0;
            }
        }

    } else if (!strcmp(line, "board push end")) {
        if (!g_push_active) {
            Serial.println("[Board] ERROR: no push in progress.");
        } else {
            g_push_active = false;
            uint16_t mdlen = base64_decode(g_push_buf, g_push_len,
                                           g_cfg.board_md, sizeof(g_cfg.board_md) - 1);
            if (mdlen == 0) {
                Serial.println("[Board] ERROR: base64 decode empty --> config rejected.");
            } else {
                g_cfg.board_md[mdlen] = '\0';
                bool ok = board_parse_md(g_cfg.board_md);
                if (!ok) {
                    Serial.println("[Board] ERROR: no entries found --> config rejected.");
                    g_cfg.board_md[0]     = '\0';
                    g_cfg.board_md_loaded = false;
                } else {
                    g_cfg.board_md_loaded = true;
                    board_init_hardware();
                    board_init_peripherals();
                    cfg_save();
                    Serial.printf("[Board] Config accepted : "
                                  "%u GPIO, %u UART, %u ADC, %u I2C, %u SPI, %u Servo, %u PWM\r\n",
                                  g_board_pin_count, g_board_serial_count, g_board_adc_count,
                                  g_board_i2c_count,  g_board_spi_count,
                                  g_board_servo_count, g_board_pwm_count);
                }
            }
        }

    // ── Board show ─────────────────────────────────────────────────────
    } else if (!strcmp(line, "board show")) {
        if (!g_cfg.board_md_loaded) {
            Serial.println("[Board] No board config loaded.");
        } else {
            Serial.printf("\r\n[Board] GPIO (%u):\r\n", g_board_pin_count);
            for (uint8_t i = 0; i < g_board_pin_count; ++i)
                Serial.printf("  %-2u  %-14s  %-12s  %s\r\n",
                              g_board_pins[i].pin, _bp_mode_name(g_board_pins[i].mode),
                              g_board_pins[i].name, g_board_pins[i].desc);

            Serial.printf("[Board] UART (%u):\r\n", g_board_serial_count);
            for (uint8_t i = 0; i < g_board_serial_count; ++i)
                Serial.printf("  UART%u  %-10s  baud=%-7lu  rx=%-2u  tx=%-2u  %s\r\n",
                              g_board_serials[i].port_num, g_board_serials[i].name,
                              (unsigned long)g_board_serials[i].baud,
                              g_board_serials[i].rx_pin, g_board_serials[i].tx_pin,
                              g_board_serials[i].desc);

            Serial.printf("[Board] ADC (%u):\r\n", g_board_adc_count);
            for (uint8_t i = 0; i < g_board_adc_count; ++i)
                Serial.printf("  %-2u  %-12s  %s\r\n",
                              g_board_adc[i].pin, g_board_adc[i].name, g_board_adc[i].desc);

            Serial.printf("[Board] I2C (%u):\r\n", g_board_i2c_count);
            for (uint8_t i = 0; i < g_board_i2c_count; ++i)
                Serial.printf("  I2C%u  SDA=%-2u  SCL=%-2u  addr=0x%02X  %-12s  %s\r\n",
                              g_board_i2c[i].bus, g_board_i2c[i].sda, g_board_i2c[i].scl,
                              g_board_i2c[i].addr, g_board_i2c[i].name, g_board_i2c[i].desc);

            Serial.printf("[Board] SPI (%u):\r\n", g_board_spi_count);
            for (uint8_t i = 0; i < g_board_spi_count; ++i)
                Serial.printf("  SPI%u  MOSI=%-2u  MISO=%-2u  SCK=%-2u  CS=%-2u  %-10s  %s\r\n",
                              g_board_spi[i].bus, g_board_spi[i].mosi, g_board_spi[i].miso,
                              g_board_spi[i].sck, g_board_spi[i].cs,
                              g_board_spi[i].name, g_board_spi[i].desc);

            Serial.printf("[Board] Servo (%u):\r\n", g_board_servo_count);
            for (uint8_t i = 0; i < g_board_servo_count; ++i)
                Serial.printf("  pin=%-2u  %-12s  range=%u-%u  %s\r\n",
                              g_board_servos[i].pin, g_board_servos[i].name,
                              g_board_servos[i].min_angle, g_board_servos[i].max_angle,
                              g_board_servos[i].desc);

            Serial.printf("[Board] PWM (%u):\r\n", g_board_pwm_count);
            for (uint8_t i = 0; i < g_board_pwm_count; ++i)
                Serial.printf("  pin=%-2u  %-12s  freq=%luHz  res=%ubits  %s\r\n",
                              g_board_pwm[i].pin, g_board_pwm[i].name,
                              (unsigned long)g_board_pwm[i].freq, g_board_pwm[i].resolution,
                              g_board_pwm[i].desc);
        }

    // ── Board reset ────────────────────────────────────────────────────
    } else if (!strcmp(line, "board reset")) {
        board_reset_peripherals();
        board_reset_hardware();
        g_cfg.board_md[0]     = '\0';
        g_cfg.board_md_loaded = false;
        cfg_save();

    // ── GPIO commands ──────────────────────────────────────────────────
    } else if (!strncmp(line, "gpio get ", 9)) {
        int pin = atoi(line + 9);
        Serial.printf("GPIO %d = %d\r\n", pin, digitalRead(pin));

    } else if (!strncmp(line, "gpio set ", 9)) {
        char *rest = (char*)line + 9;
        char *sp   = strchr(rest, ' ');
        if (!sp) { Serial.println("Usage: gpio set <pin> <0|1>"); }
        else {
            int pin = atoi(rest); int val = atoi(sp + 1);
            if (!board_is_output_pin(pin))
                Serial.printf("[!] GPIO %d not declared OUTPUT in board config.\r\n", pin);
            else {
                digitalWrite(pin, val ? HIGH : LOW);
                Serial.printf("GPIO %d set to %d\r\n", pin, val ? 1 : 0);
            }
        }

    } else if (!strncmp(line, "gpio mode ", 10)) {
        char *rest = (char*)line + 10;
        char *sp   = strchr(rest, ' ');
        if (!sp) { Serial.println("Usage: gpio mode <pin> <in|out|in_pu>"); }
        else {
            int pin = atoi(rest); const char *m = sp + 1;
            uint8_t mode = !strcmp(m,"out")   ? OUTPUT :
                           !strcmp(m,"in_pu") ? INPUT_PULLUP : INPUT;
            pinMode(pin, mode);
            Serial.printf("GPIO %d mode set to %s\r\n", pin, m);
        }

    // ── ADC commands ───────────────────────────────────────────────────
    } else if (!strncmp(line, "adc read ", 9)) {
        int pin = atoi(line + 9);
        if (!board_is_adc_pin(pin))
            Serial.printf("[!] Pin %d not in ## ADC Pins reading anyway: %d\r\n",
                          pin, analogRead(pin));
        else
            Serial.printf("ADC %d = %d\r\n", pin, analogRead(pin));

    // ── Named serial commands ──────────────────────────────────────────
    } else if (!strncmp(line, "serial write ", 13)) {
        char *rest = (char*)line + 13;
        char *sp   = strchr(rest, ' ');
        if (!sp) { Serial.println("Usage: serial write <name> <data>"); }
        else {
            *sp = '\0';
            int si = board_find_serial_by_name(rest);
            if (si < 0) Serial.printf("[!] No serial port named '%s'\r\n", rest);
            else {
                int w = board_serial_write(si, sp + 1);
                if (w < 0) Serial.printf("[!] UART unavailable for '%s'\r\n", rest);
                else Serial.printf("serial '%s' ← '%s'\r\n", rest, sp + 1);
            }
        }

    } else if (!strncmp(line, "serial read ", 12)) {
        const char *name = line + 12;
        int si = board_find_serial_by_name(name);
        if (si < 0) Serial.printf("[!] No serial port named '%s'\r\n", name);
        else {
            char rbuf[128] = {};
            // No explicit timeout — default 150 ms (Bug #6 fix)
            board_serial_read(si, rbuf, sizeof(rbuf));
            Serial.printf("serial '%s' → %s\r\n", name, rbuf);
        }

    // ── Servo shell commands ───────────────────────────────────────────
    } else if (!strncmp(line, "servo set ", 10)) {
#if defined(BOARD_HAS_SERVO)
        char *rest = (char*)line + 10;
        char *sp   = strchr(rest, ' ');
        if (!sp) { Serial.println("Usage: servo set <name> <angle>"); }
        else {
            *sp = '\0'; int angle = atoi(sp + 1);
            int si = board_find_servo_by_name(rest);
            if (si < 0) Serial.printf("[!] No servo named '%s'\r\n", rest);
            else {
                angle = max((int)g_board_servos[si].min_angle,
                            min((int)g_board_servos[si].max_angle, angle));
                s_servos[si].write(angle);
                Serial.printf("Servo '%s' → %d°\r\n", rest, angle);
            }
        }
#else
        Serial.println("[!] Servo support not compiled in (no -DBOARD_HAS_SERVO).");
#endif

    // ── PWM shell commands ─────────────────────────────────────────────
    } else if (!strncmp(line, "pwm set ", 8)) {
        char *rest = (char*)line + 8;
        char *sp   = strchr(rest, ' ');
        if (!sp) { Serial.println("Usage: pwm set <name> <duty 0-255>"); }
        else {
            *sp = '\0'; int duty = atoi(sp + 1);
            duty = max(0, min(255, duty));
            int pi = board_find_pwm_by_name(rest);
            if (pi < 0) Serial.printf("[!] No PWM named '%s'\r\n", rest);
            else {
#ifdef BOARD_ESP32
                ledcWrite(g_board_pwm[pi].channel, (uint32_t)duty);
#else
                analogWrite(g_board_pwm[pi].pin, duty);
#endif
                Serial.printf("PWM '%s' duty=%d\r\n", rest, duty);
            }
        }

    } else if (line[0]) {
        Serial.printf("Unknown: '%s'  (type 'help')\r\n", line);
    }
}

// ─── shell_byte ───────────────────────────────────────────────────────────────
// IMPORTANT: bytes are ALWAYS consumed from the hardware FIFO so that the MCU
// USB-CDC / UART receive buffer never overflows during a network operation.
// Command *execution* is deferred until !g_http_busy; the partial line is
// kept in g_cmd so the MCU can complete it once the network call finishes.
static void shell_byte(uint8_t c) {
    if (c == '\n' || c == '\r') {
        g_cmd[g_cmd_len] = '\0';
        if (g_cmd_len > 0) {
            Serial.print("\r\n");
            if (!g_http_busy) {
                shell_run(g_cmd);
            }
            // else: board is mid-request drop silently; FIFO stays drained.
        }
        g_cmd_len = 0;
        if (!g_http_busy) shell_prompt();
    } else if (c == 127 || c == 8) {
        if (g_cmd_len > 0) { --g_cmd_len; if (!g_http_busy) Serial.print("\b \b"); }
    } else if (g_cmd_len + 1 < CMD_S) {
        g_cmd[g_cmd_len++] = (char)c;
        if (!g_http_busy) Serial.write(c);   // echo only when interactive
    }
}