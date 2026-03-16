/*
 * ─────────────────────────────────────────────────────────────
 * Hardware action executor + peripheral init.
 *
 * Handles all [ACTION:...] tags emitted by the LLM and provides
 * board_init_peripherals() for library-dependent hardware setup
 * (Wire, Servo.attach, ledcSetup) that cannot live in board_parser.h
 * because it requires optional library headers.
 *
 * Build flags that unlock optional hardware:
 *   -DBOARD_HAS_SERVO         → Servo motor support
 *   -DBOARD_HAS_OLED_SSD1306  → Adafruit SSD1306 OLED
 *   -DBOARD_HAS_TFT_ILI9341   → Adafruit ILI9341 TFT
 *   -DBOARD_HAS_TFT_ST7789    → Adafruit ST7789/ST7735 TFT
 *
 * GUI auto-injects these flags via _write_minimal_ini() based on
 * keyword detection in [CONTROL].md.
 *
 * Depends on: board_parser.h, http.h, constants.h
 * ─────────────────────────────────────────────────────────────
 */

#pragma once

// ── Optional library includes ─────────────────────────────────────────────────
#if defined(BOARD_HAS_SERVO)
  #ifdef BOARD_ESP32
    #include <ESP32Servo.h>    // madhephaestus/ESP32Servo : shares LEDC hardware
  #else
    #include <Servo.h>         // default
  #endif
#endif

#if defined(BOARD_HAS_OLED_SSD1306)
  #include <Wire.h>
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>
#elif defined(BOARD_HAS_TFT_ILI9341) || defined(BOARD_HAS_TFT_ST7789)
  #include <Wire.h>            // still needed for I2C sensors on same board
#else
  // Include Wire only if any I2C entries are expected at runtime
  #include <Wire.h>
#endif

#if defined(BOARD_HAS_TFT_ILI9341)
  #include <SPI.h>
  #include <Adafruit_ILI9341.h>
  #include <Adafruit_GFX.h>
#endif

#if defined(BOARD_HAS_TFT_ST7789)
  #include <SPI.h>
  #include <Adafruit_ST7789.h>
  #include <Adafruit_GFX.h>
#endif

// ── Static peripheral pools ───────────────────────────────────────────────────
#if defined(BOARD_HAS_SERVO)
static Servo s_servos[MAX_BOARD_SERVOS];
#endif

#if defined(BOARD_HAS_OLED_SSD1306)
// One SSD1306 instance per I2C entry (max MAX_BOARD_I2C = 4).
// Screen size defaults to 128×64; update OLED_W/H if you use a different panel.
static constexpr uint8_t OLED_W = 128;
static constexpr uint8_t OLED_H = 64;
static Adafruit_SSD1306 s_oled[MAX_BOARD_I2C] = {
    Adafruit_SSD1306(OLED_W, OLED_H, &Wire,  -1),
    Adafruit_SSD1306(OLED_W, OLED_H, &Wire,  -1),
    Adafruit_SSD1306(OLED_W, OLED_H, &Wire1, -1),
    Adafruit_SSD1306(OLED_W, OLED_H, &Wire1, -1),
};
static bool s_oled_ok[MAX_BOARD_I2C] = {false};
#endif

#if defined(BOARD_HAS_TFT_ILI9341)
static Adafruit_ILI9341 *s_tft_ili[MAX_BOARD_SPI] = {nullptr};
#endif

#if defined(BOARD_HAS_TFT_ST7789)
static Adafruit_ST7789 *s_tft_st7[MAX_BOARD_SPI] = {nullptr};
#endif

// ─── board_init_peripherals ───────────────────────────────────────────────────
/*
 * Initialise Wire, Servo, LEDC, and display libraries using the parsed
 * board config. Call from setup() AFTER board_init_hardware().
 *
 * On ESP32: LEDC channels are auto-assigned starting at channel 0.
 *   • Servos claim channels 0-7  (via ESP32Servo which uses LEDC internally)
 *   • PWM outputs claim the next available channels after that
 *   Maximum 16 LEDC channels on ESP32 —> a warning is printed if exceeded.
 *
 * On Pico W: Servo uses hardware PWM slices (auto-assigned by Servo.h).
 *   analogWrite() handles PWM directly —> no channel pre-allocation needed.
 */
static void board_init_peripherals() {
    // ── Wire (I2C) ───────────────────────────────────────────────────────
    // Multiple I2C entries may share the same bus, only call begin() once
    // per bus and use the FIRST entry's SDA/SCL for that bus.
    bool wire_begun[2]  = {false, false};
    for (uint8_t i = 0; i < g_board_i2c_count; ++i) {
        const BoardI2C &bi = g_board_i2c[i];
        uint8_t b = (bi.bus > 1) ? 1 : bi.bus;
        if (!wire_begun[b]) {
            TwoWire &w = (b == 0) ? Wire : Wire1;
#ifdef BOARD_ESP32
            w.begin(bi.sda, bi.scl);
#else
            w.setSDA(bi.sda); w.setSCL(bi.scl); w.begin();
#endif
            wire_begun[b] = true;
            Serial.printf("[Board] I2C%u  bus begun  SDA=GP%-2u  SCL=GP%-2u\r\n",
                          b, bi.sda, bi.scl);
        }

        // ── SSD1306 OLED init ─────────────────────────────────────────
#if defined(BOARD_HAS_OLED_SSD1306)
        if (i < MAX_BOARD_I2C) {
            uint8_t addr = bi.addr ? bi.addr : 0x3C;
            if (s_oled[i].begin(SSD1306_SWITCHCAPVCC, addr)) {
                s_oled[i].clearDisplay();
                s_oled[i].display();
                s_oled_ok[i] = true;
                Serial.printf("[Board] OLED '%s'  addr=0x%02X  ok\r\n", bi.name, addr);
            } else {
                Serial.printf("[Board] OLED '%s'  addr=0x%02X  FAILED\r\n", bi.name, addr);
            }
        }
#endif
    }

    // ── SPI ─────────────────────────────────────────────────────────────
#if defined(BOARD_HAS_TFT_ILI9341) || defined(BOARD_HAS_TFT_ST7789)
    for (uint8_t i = 0; i < g_board_spi_count; ++i) {
        const BoardSPI &bs = g_board_spi[i];
#if defined(BOARD_HAS_TFT_ILI9341)
        if (i < MAX_BOARD_SPI) {
            s_tft_ili[i] = new Adafruit_ILI9341(bs.cs, bs.sck, bs.mosi);
            s_tft_ili[i]->begin();
            s_tft_ili[i]->fillScreen(ILI9341_BLACK);
            Serial.printf("[Board] ILI9341 '%s'  CS=%-2u  SCK=%-2u  MOSI=%-2u\r\n",
                          bs.name, bs.cs, bs.sck, bs.mosi);
        }
#endif
#if defined(BOARD_HAS_TFT_ST7789)
        if (i < MAX_BOARD_SPI) {
            s_tft_st7[i] = new Adafruit_ST7789(bs.cs, bs.sck, bs.mosi);
            s_tft_st7[i]->init(240, 320);
            s_tft_st7[i]->fillScreen(ST77XX_BLACK);
            Serial.printf("[Board] ST7789 '%s'  CS=%-2u  SCK=%-2u  MOSI=%-2u\r\n",
                          bs.name, bs.cs, bs.sck, bs.mosi);
        }
#endif
    }
#endif

    // ── Servo ────────────────────────────────────────────────────────────
#if defined(BOARD_HAS_SERVO)
    for (uint8_t i = 0; i < g_board_servo_count; ++i) {
        s_servos[i].attach(g_board_servos[i].pin, 544, 2400);
        s_servos[i].write(g_board_servos[i].min_angle);
        Serial.printf("[Board] Servo '%s'  pin=%-2u  range=%u-%u\r\n",
                      g_board_servos[i].name, g_board_servos[i].pin,
                      g_board_servos[i].min_angle, g_board_servos[i].max_angle);
    }
#endif

    // ── PWM (LEDC on ESP32, analogWrite on Pico W) ───────────────────────
    // ESP32: first servo channels, then PWM. Warn if total exceeds 16.
    // Pico W: analogWriteFreq/analogWriteResolution per-pin, no channel needed.
#ifdef BOARD_ESP32
    uint8_t ledc_next = g_board_servo_count; // Servo already claimed 0..(servo_count-1)
    for (uint8_t i = 0; i < g_board_pwm_count; ++i) {
        if (ledc_next >= 16) {
            Serial.printf("[Board] WARNING: PWM '%s' — no LEDC channel available (max 16 total)\r\n",
                          g_board_pwm[i].name);
            continue;
        }
        g_board_pwm[i].channel = ledc_next++;
        ledcSetup(g_board_pwm[i].channel, g_board_pwm[i].freq, g_board_pwm[i].resolution);
        ledcAttachPin(g_board_pwm[i].pin, g_board_pwm[i].channel);
        ledcWrite(g_board_pwm[i].channel, 0);
        Serial.printf("[Board] PWM   '%s'  pin=%-2u  freq=%luHz  res=%ubits  ch=%u\r\n",
                      g_board_pwm[i].name, g_board_pwm[i].pin,
                      (unsigned long)g_board_pwm[i].freq,
                      g_board_pwm[i].resolution, g_board_pwm[i].channel);
    }
#else
    for (uint8_t i = 0; i < g_board_pwm_count; ++i) {
        analogWriteFreq(g_board_pwm[i].freq);
        analogWriteResolution(g_board_pwm[i].resolution);
        analogWrite(g_board_pwm[i].pin, 0);
        Serial.printf("[Board] PWM   '%s'  pin=%-2u  freq=%luHz  res=%ubits\r\n",
                      g_board_pwm[i].name, g_board_pwm[i].pin,
                      (unsigned long)g_board_pwm[i].freq, g_board_pwm[i].resolution);
    }
#endif
}

// ─── board_reset_peripherals ──────────────────────────────────────────────────
// Detach servos and release PWM channels. Called from shell 'board reset'.
static void board_reset_peripherals() {
#if defined(BOARD_HAS_SERVO)
    for (uint8_t i = 0; i < g_board_servo_count; ++i)
        if (s_servos[i].attached()) s_servos[i].detach();
#endif

#ifdef BOARD_ESP32
    for (uint8_t i = 0; i < g_board_pwm_count; ++i) {
        if (g_board_pwm[i].active) {
            ledcWrite(g_board_pwm[i].channel, 0);
            ledcDetachPin(g_board_pwm[i].pin);
        }
    }
#else
    for (uint8_t i = 0; i < g_board_pwm_count; ++i)
        if (g_board_pwm[i].active) analogWrite(g_board_pwm[i].pin, 0);
#endif
}

// ─── strip_action_tags ────────────────────────────────────────────────────────
// Remove all [ACTION:...] substrings from buf in-place.
static void strip_action_tags(char *buf) {
    char *dst = buf;
    const char *src = buf;
    while (*src) {
        const char *tag = strstr(src, "[ACTION:");
        if (!tag) { while (*src) *dst++ = *src++; break; }
        while (src < tag) *dst++ = *src++;
        const char *end = strchr(tag, ']');
        src = end ? end + 1 : tag + 1;
    }
    *dst = '\0';
}

// ─── execute_actions_in_response ─────────────────────────────────────────────
/*
 * Scan llm_response for [ACTION:...] tags, execute each recognised action,
 * and append [RESULT:...] lines to result_buf for feeding back to the LLM.
 *
 * Safety constraints :
 *   gpio_set   — silently refused for INPUT-mode pins
 *   adc_read   — only pins declared in ## ADC Pins
 *   delay_ms   — hard-capped at 5000 ms, with USB-CDC keepalive loop
 *   serial_*   — only declared ## Serial Ports
 *   servo_set  — angle clamped to declared min–max range
 *   pwm_set    — duty clamped to 0–255
 *
 * Returns number of actions executed.
 */
static int execute_actions_in_response(const char *llm_response,
                                       char *result_buf, uint16_t result_cap) {
    const char *p = llm_response;
    int count = 0;
    result_buf[0] = '\0';
    uint16_t rpos = 0;

    while ((p = strstr(p, "[ACTION:")) != nullptr) {
        const char *end = strchr(p, ']');
        if (!end) break;

        char action_buf[160] = {0};
        size_t alen = (size_t)(end - p) - 8;
        if (alen >= sizeof(action_buf)) { p = end + 1; continue; }
        memcpy(action_buf, p + 8, alen);

        char result[160] = "[RESULT:unknown]\n";

        // ── gpio_set ──────────────────────────────────────────────────
        if (strncmp(action_buf, "gpio_set", 8) == 0) {
            int pin = board_resolve_action_pin(action_buf, "pin");
            int val = board_parse_action_int(action_buf, "value");
            if (pin < 0)
                snprintf(result, sizeof(result), "[RESULT:gpio_set error=pin_not_found]\n");
            else if (!board_is_output_pin(pin))
                snprintf(result, sizeof(result), "[RESULT:gpio_set pin=%d error=not_output_pin]\n", pin);
            else {
                digitalWrite(pin, val ? HIGH : LOW);
                snprintf(result, sizeof(result), "[RESULT:gpio_set pin=%d value=%d ok=1]\n", pin, val ? 1 : 0);
            }

        // ── gpio_get ──────────────────────────────────────────────────
        } else if (strncmp(action_buf, "gpio_get", 8) == 0) {
            int pin = board_resolve_action_pin(action_buf, "pin");
            if (pin < 0)
                snprintf(result, sizeof(result), "[RESULT:gpio_get error=pin_not_found]\n");
            else{
                bool inv = false;
                for(uint8_t i = 0; i < g_board_pin_count; ++i)
                {
                    if(g_board_pins[i].pin == (uint8_t)pin)
                    {
                        inv = g_board_pins[i].inverted;
                        break;
                    }
                }
                int phy = digitalRead(pin);
                int logic = inv ? !phy : phy;
                snprintf(result, sizeof(result), "[RESULT:gpio_get pin=%d value=%d]\n",
                         pin, logic);
            }
        // ── adc_read ──────────────────────────────────────────────────
        } else if (strncmp(action_buf, "adc_read", 8) == 0) {
            int pin = board_resolve_action_pin(action_buf, "pin");
            if (pin < 0)
                snprintf(result, sizeof(result), "[RESULT:adc_read error=pin_not_found]\n");
            else if (!board_is_adc_pin(pin))
                snprintf(result, sizeof(result), "[RESULT:adc_read pin=%d error=not_declared_adc_pin]\n", pin);
            else
                snprintf(result, sizeof(result), "[RESULT:adc_read pin=%d value=%d]\n",
                         pin, analogRead(pin));

        // ── serial_write ──────────────────────────────────────────────
        } else if (strncmp(action_buf, "serial_write", 12) == 0) {
            char port_name[32]; char data[96] = {};
            board_parse_action_str(action_buf, "port", port_name, sizeof(port_name));
            board_parse_action_str(action_buf, "data", data, sizeof(data));
            int si = board_find_serial_by_name(port_name);
            if (si < 0)
                snprintf(result, sizeof(result), "[RESULT:serial_write port=%s error=not_declared]\n", port_name);
            else {
                int written = board_serial_write(si, data);
                if (written < 0)
                    snprintf(result, sizeof(result), "[RESULT:serial_write port=%s error=uart_unavailable]\n", port_name);
                else
                    snprintf(result, sizeof(result), "[RESULT:serial_write port=%s bytes=%u ok=1]\n",
                             port_name, (unsigned)strlen(data));
            }

        // ── serial_read ───────────────────────────────────────────────
        } else if (strncmp(action_buf, "serial_read", 11) == 0) {
            char port_name[32];
            board_parse_action_str(action_buf, "port", port_name, sizeof(port_name));
            int si = board_find_serial_by_name(port_name);
            if (si < 0)
                snprintf(result, sizeof(result), "[RESULT:serial_read port=%s error=not_declared]\n", port_name);
            else {
                char rbuf[96] = {};
                // No explicit timeout — default 150 ms in board_serial_read (Bug #2 / #6 fix)
                board_serial_read(si, rbuf, sizeof(rbuf));
                snprintf(result, sizeof(result), "[RESULT:serial_read port=%s data=\"%.80s\"]\n",
                         port_name, rbuf);
            }

        // ── delay_ms ──────────────────────────────────────────────────
        } else if (strncmp(action_buf, "delay_ms", 8) == 0) {
            int ms = board_parse_action_int(action_buf, "ms");
            if (ms < 0) ms = 0;
            if (ms > 5000) ms = 5000;  // hard cap
            /*
             * usb_keepalive() is not called while the normal
             * network stack runs here (g_http_busy=false). Drip a null byte
             * every 200 ms so the ESP32-C3 USB-CDC driver doesn't drop the
             * COM port during a long delay action.
             */
            uint32_t remaining = (uint32_t)ms;
            unsigned long last_ka = millis();
            while (remaining > 0) {
                uint32_t step = (remaining > 1) ? 1 : remaining;
                delay(step);
                remaining -= step;
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
                unsigned long now = millis();
                if (now - last_ka >= 200) {
                    last_ka = now;
                    Serial.write((uint8_t)0x00);
                    Serial.flush();
                }
#else
                (void)last_ka;
#endif
            }
            snprintf(result, sizeof(result), "[RESULT:delay_ms ms=%d ok=1]\n", ms);

        // ── servo_set ─────────────────────────────────────────────────
        } else if (strncmp(action_buf, "servo_set", 9) == 0) {
#if defined(BOARD_HAS_SERVO)
            char name[32];
            board_parse_action_str(action_buf, "name", name, sizeof(name));
            int angle = board_parse_action_int(action_buf, "angle");
            int si = board_find_servo_by_name(name);
            if (si < 0) {
                snprintf(result, sizeof(result), "[RESULT:servo_set error=not_found name=%s]\n", name);
            } else {
                angle = max((int)g_board_servos[si].min_angle,
                            min((int)g_board_servos[si].max_angle,
                                angle < 0 ? 0 : angle));
                uint8_t step = g_board_servos[si].servo_step;
                if(step <= 1){
                    s_servos[si].write(angle);
                } else{
                    int current = s_servos[si].read();
                    int dir = (angle > current) ? 1 : -1;
                    uint16_t time = g_board_servos[si].step_delay_ms;
                    for(int pos = current; pos != angle; pos += dir * step)
                    {
                        if((dir > 0 && pos > angle) || (dir < 0 && pos < angle))
                        {
                            pos = angle;
                        }
                        s_servos[si].write(pos);
                        if(time > 0) delay(time);
                    }
                    s_servos[si].write(angle);
                }

                snprintf(result, sizeof(result), "[RESULT:servo_set name=%s angle=%d ok=1]\n",
                         name, angle);
            }
#else
            snprintf(result, sizeof(result), "[RESULT:servo_set error=servo_not_built]\n");
#endif

        // ── pwm_set ───────────────────────────────────────────────────
        } else if (strncmp(action_buf, "pwm_set", 7) == 0) {
            char name[32];
            board_parse_action_str(action_buf, "name", name, sizeof(name));
            int duty = board_parse_action_int(action_buf, "duty");
            if (duty < 0) duty = 0;
            if (duty > 255) duty = 255;
            int pi = board_find_pwm_by_name(name);
            if (pi < 0) {
                snprintf(result, sizeof(result), "[RESULT:pwm_set error=not_found name=%s]\n", name);
            } else {
#ifdef BOARD_ESP32
                ledcWrite(g_board_pwm[pi].channel, (uint32_t)duty);
#else
                analogWrite(g_board_pwm[pi].pin, duty);
#endif
                snprintf(result, sizeof(result), "[RESULT:pwm_set name=%s duty=%d ok=1]\n",
                         name, duty);
            }

        // ── oled_print ────────────────────────────────────────────────
        } else if (strncmp(action_buf, "oled_print", 10) == 0) {
#if defined(BOARD_HAS_OLED_SSD1306)
            char bus_name[32]; char text[96];
            board_parse_action_str(action_buf, "bus",  bus_name, sizeof(bus_name));
            board_parse_action_str(action_buf, "text", text,     sizeof(text));
            int x = board_parse_action_int(action_buf, "x");
            int y = board_parse_action_int(action_buf, "y");
            if (x < 0) x = 0; if (y < 0) y = 0;
            int bi = board_find_i2c_by_name(bus_name);
            if (bi < 0 || !s_oled_ok[bi]) {
                snprintf(result, sizeof(result), "[RESULT:oled_print bus=%s error=not_found]\n", bus_name);
            } else {
                s_oled[bi].setCursor(x, y);
                s_oled[bi].setTextColor(SSD1306_WHITE);
                s_oled[bi].setTextSize(1);
                s_oled[bi].print(text);
                s_oled[bi].display();
                snprintf(result, sizeof(result), "[RESULT:oled_print bus=%s ok=1]\n", bus_name);
            }
#else
            snprintf(result, sizeof(result), "[RESULT:oled_print error=oled_not_built]\n");
#endif

        // ── oled_clear ────────────────────────────────────────────────
        } else if (strncmp(action_buf, "oled_clear", 10) == 0) {
#if defined(BOARD_HAS_OLED_SSD1306)
            char bus_name[32];
            board_parse_action_str(action_buf, "bus", bus_name, sizeof(bus_name));
            int bi = board_find_i2c_by_name(bus_name);
            if (bi < 0 || !s_oled_ok[bi]) {
                snprintf(result, sizeof(result), "[RESULT:oled_clear bus=%s error=not_found]\n", bus_name);
            } else {
                s_oled[bi].clearDisplay();
                s_oled[bi].display();
                snprintf(result, sizeof(result), "[RESULT:oled_clear bus=%s ok=1]\n", bus_name);
            }
#else
            snprintf(result, sizeof(result), "[RESULT:oled_clear error=oled_not_built]\n");
#endif

        // ── tft_print ─────────────────────────────────────────────────
        } else if (strncmp(action_buf, "tft_print", 9) == 0) {
#if defined(BOARD_HAS_TFT_ILI9341) || defined(BOARD_HAS_TFT_ST7789)
            char bus_name[32]; char text[96]; char color_s[16];
            board_parse_action_str(action_buf, "bus",   bus_name, sizeof(bus_name));
            board_parse_action_str(action_buf, "text",  text,     sizeof(text));
            board_parse_action_str(action_buf, "color", color_s,  sizeof(color_s));
            int x = board_parse_action_int(action_buf, "x");
            int y = board_parse_action_int(action_buf, "y");
            if (x < 0) x = 0; if (y < 0) y = 0;
            // Parse color: "white" | "red" | "green" | "blue" | hex (e.g. "0xFFFF")
            uint16_t color = 0xFFFF; // default white
            if      (!strcmp(color_s,"red"))   color = 0xF800;
            else if (!strcmp(color_s,"green")) color = 0x07E0;
            else if (!strcmp(color_s,"blue"))  color = 0x001F;
            else if (!strcmp(color_s,"black")) color = 0x0000;
            else if (color_s[0])               color = (uint16_t)strtol(color_s, nullptr, 0);
            int bi = board_find_spi_by_name(bus_name);
            if (bi < 0) {
                snprintf(result, sizeof(result), "[RESULT:tft_print bus=%s error=not_found]\n", bus_name);
            } else {
#if defined(BOARD_HAS_TFT_ILI9341)
                if (s_tft_ili[bi]) {
                    s_tft_ili[bi]->setCursor(x, y);
                    s_tft_ili[bi]->setTextColor(color);
                    s_tft_ili[bi]->setTextSize(1);
                    s_tft_ili[bi]->print(text);
                    snprintf(result, sizeof(result), "[RESULT:tft_print bus=%s ok=1]\n", bus_name);
                }
#elif defined(BOARD_HAS_TFT_ST7789)
                if (s_tft_st7[bi]) {
                    s_tft_st7[bi]->setCursor(x, y);
                    s_tft_st7[bi]->setTextColor(color);
                    s_tft_st7[bi]->setTextSize(1);
                    s_tft_st7[bi]->print(text);
                    snprintf(result, sizeof(result), "[RESULT:tft_print bus=%s ok=1]\n", bus_name);
                }
#endif
            }
#else
            snprintf(result, sizeof(result), "[RESULT:tft_print error=tft_not_built]\n");
#endif

        // ── i2c_write (raw) ───────────────────────────────────────────
        } else if (strncmp(action_buf, "i2c_write", 9) == 0) {
            char bus_name[32]; char reg_s[8]; char data_s[32];
            board_parse_action_str(action_buf, "bus",  bus_name, sizeof(bus_name));
            board_parse_action_str(action_buf, "reg",  reg_s,    sizeof(reg_s));
            board_parse_action_str(action_buf, "data", data_s,   sizeof(data_s));
            int bi = board_find_i2c_by_name(bus_name);
            if (bi < 0) {
                snprintf(result, sizeof(result), "[RESULT:i2c_write bus=%s error=not_found]\n", bus_name);
            } else {
                uint8_t addr = g_board_i2c[bi].addr;
                uint8_t reg  = (uint8_t)strtol(reg_s,  nullptr, 0);
                uint8_t dat  = (uint8_t)strtol(data_s, nullptr, 0);
                TwoWire &w = (g_board_i2c[bi].bus == 0) ? Wire : Wire1;
                w.beginTransmission(addr);
                w.write(reg); w.write(dat);
                uint8_t err = w.endTransmission();
                snprintf(result, sizeof(result), "[RESULT:i2c_write bus=%s err=%u ok=%u]\n",
                         bus_name, err, err == 0 ? 1 : 0);
            }

        // ── i2c_read (raw) ────────────────────────────────────────────
        } else if (strncmp(action_buf, "i2c_read", 8) == 0) {
            char bus_name[32]; char reg_s[8];
            board_parse_action_str(action_buf, "bus", bus_name, sizeof(bus_name));
            board_parse_action_str(action_buf, "reg", reg_s,    sizeof(reg_s));
            int len = board_parse_action_int(action_buf, "len");
            if (len <= 0 || len > 16) len = 1;
            int bi = board_find_i2c_by_name(bus_name);
            if (bi < 0) {
                snprintf(result, sizeof(result), "[RESULT:i2c_read bus=%s error=not_found]\n", bus_name);
            } else {
                uint8_t addr = g_board_i2c[bi].addr;
                uint8_t reg  = (uint8_t)strtol(reg_s, nullptr, 0);
                TwoWire &w = (g_board_i2c[bi].bus == 0) ? Wire : Wire1;
                w.beginTransmission(addr);
                w.write(reg);
                w.endTransmission(false);
                uint8_t n = w.requestFrom(addr, (uint8_t)len);
                char hex[48] = {};
                uint8_t hw = 0;
                for (uint8_t i = 0; i < n && hw + 3 < sizeof(hex); ++i)
                    hw += snprintf(hex + hw, sizeof(hex) - hw, "%02X", (uint8_t)w.read());
                snprintf(result, sizeof(result), "[RESULT:i2c_read bus=%s data=0x%s]\n",
                         bus_name, hex);
            }

        } else {
            snprintf(result, sizeof(result), "[RESULT:unknown_action]\n");
        }

        Serial.printf("[Action] %s", result);

        uint16_t rlen = (uint16_t)strlen(result);
        if (rpos + rlen + 1 < result_cap) {
            memcpy(result_buf + rpos, result, rlen);
            rpos += rlen;
            result_buf[rpos] = '\0';
        }

        p = end + 1;
        ++count;
    }
    return count;
}