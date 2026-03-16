/*
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * FemtoClaw : Markdown-driven GPIO / Serial / ADC configuration parser.
 *
 * Parses a [CONTROL].md file (pushed from the GUI) into static arrays that
 * drive hardware initialisation and AI action dispatch.
 *
 * Supported sections:
 *   ## GPIO Pins      — digital I/O
 *   ## Serial Ports   — UART
 *   ## ADC Pins       — analogue inputs
 *   ## I2C Buses      — I2C peripherals (OLED, sensors …)
 *   ## SPI Buses      — SPI peripherals (TFT, flash …)
 *   ## Servos         — servo motors
 *   ## PWM Outputs    — variable-duty PWM (fans, pumps, dimmers …)
 *
 * Supported platforms
 *   • ESP32(DevKit, S3, C3, C6)  — build flag: -DBOARD_ESP32
 *   • Raspberry Pi Pico W (RP2040+CYW43)   — build flag: -DBOARD_PICO_W
 *
 * Features:
 *   • Zero heap allocation — all state lives in static arrays.
 *   • Forgiving parser — unknown columns and out-of-range pins are skipped
 *     with a Serial warning rather than a hard fault.
 *   • Full Pico W UART support — setRX/setTX/begin handled correctly.
 *   • Single board_get_uart() helper centralises Serial1/Serial2 dispatch
 *     so both board_init_hardware() AND the action executor share one path.
 *
 * IMPORTANT: board_init_peripherals() in fc_actions.h must be called
 * AFTER board_init_hardware() to initialise Wire, Servo.attach(), and
 * LEDC channels which depend on optional library build flags.
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*/

#pragma once
#include <Arduino.h>

/*
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*                       Pico W core guard
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
* The earlephilhower Arduino-Pico core exposes SerialUART via Serial1/Serial2,
* but pin assignment must happen through setRX()/setTX() BEFORE begin().
* We also need the SerialUART type for the helper below.
*/
#ifdef BOARD_PICO_W
  #include <SerialUART.h>
#endif

/*
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*                           Compile-time limits
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
* Tuned per board family. Override by defining before including this header.
*/
#ifndef MAX_BOARD_PINS
  #if defined(BOARD_ESP32)
    #define MAX_BOARD_PINS   32
  #elif defined(BOARD_PICO_W)
    #define MAX_BOARD_PINS   30
  #else
    #define MAX_BOARD_PINS   16
  #endif
#endif

#ifndef MAX_BOARD_SERIALS
  #if defined(BOARD_ESP32) && (defined(CONFIG_IDF_TARGET_ESP32C3)) /* || \ defined(CONFIG_IDF_TARGET_ESP32C6) || \ defined(CONFIG_IDF_TARGET_ESP32H2))
                                                                    * uncomment this line to add C6 and H2 UART support & remove one ')' after _ESP32C3
                                                                    */
    // C3(/C6/H2): only UART1 is user-accessible (UART0 = USB/console)
    #define MAX_BOARD_SERIALS  1
  #elif defined(BOARD_PICO_W)
    // RP2040 has UART0 (Serial1) and UART1 (Serial2)
    #define MAX_BOARD_SERIALS  2
  #else
    #define MAX_BOARD_SERIALS  4
  #endif
#endif

#ifndef MAX_BOARD_ADC
  #if defined(BOARD_PICO_W)
    #define MAX_BOARD_ADC    4    // RP2040: GP26-GP29
  #else
    #define MAX_BOARD_ADC    8
  #endif
#endif

#ifndef MAX_BOARD_I2C
  #define MAX_BOARD_I2C    4
#endif

#ifndef MAX_BOARD_SPI
  #define MAX_BOARD_SPI    2
#endif

#ifndef MAX_BOARD_SERVOS
  #define MAX_BOARD_SERVOS 8
#endif

#ifndef MAX_BOARD_PWM
  #define MAX_BOARD_PWM    8
#endif

/*
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*                            Structs
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*/
struct BoardPin {
    uint8_t  pin;
    uint8_t  mode;      // INPUT / OUTPUT / INPUT_PULLUP / INPUT_PULLDOWN
    bool     inverted;  // true = active-low
    char     name[24];
    char     desc[64];
    bool     active;
};

struct BoardSerial {
    uint8_t  port_num;  // UART port number extracted from "UART1" → 1
    uint32_t baud;
    uint8_t  rx_pin;
    uint8_t  tx_pin;
    char     name[24];
    char     desc[64];
    bool     active;
};

struct BoardAdc {
    uint8_t  pin;
    char     name[24];
    char     desc[64];
    bool     active;
};

struct BoardI2C {
    uint8_t  bus;        // 0 = Wire, 1 = Wire1
    uint8_t  sda;
    uint8_t  scl;
    uint8_t  addr;       // 7-bit address (e.g. 0x3C)
    char     name[24];
    char     desc[64];
    bool     active;
};

struct BoardSPI {
    uint8_t  bus;        // 0 = SPI, 1 = SPI1
    uint8_t  mosi;
    uint8_t  miso;
    uint8_t  sck;
    uint8_t  cs;
    char     name[24];
    char     desc[64];
    bool     active;
};

struct BoardServo {
    uint8_t  pin;
    uint16_t min_angle;  // always 0 in practice
    uint16_t max_angle;  // 180 or custom
    uint8_t servo_step;
    uint16_t step_delay_ms;
    char     name[24];
    char     desc[64];
    bool     active;
};

struct BoardPWM {
    uint8_t  pin;
    uint32_t freq;
    uint8_t  resolution; // bits (8 = 0-255)
    uint8_t  channel;    // ESP32 LEDC channel (auto-assigned during init)
    char     name[24];
    char     desc[64];
    bool     active;
};

/*
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*                            Global state
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*/
static BoardPin    g_board_pins[MAX_BOARD_PINS];
static BoardSerial g_board_serials[MAX_BOARD_SERIALS];
static BoardAdc    g_board_adc[MAX_BOARD_ADC];
static BoardI2C    g_board_i2c[MAX_BOARD_I2C];
static BoardSPI    g_board_spi[MAX_BOARD_SPI];
static BoardServo  g_board_servos[MAX_BOARD_SERVOS];
static BoardPWM    g_board_pwm[MAX_BOARD_PWM];

static uint8_t g_board_pin_count    = 0;
static uint8_t g_board_serial_count = 0;
static uint8_t g_board_adc_count    = 0;
static uint8_t g_board_i2c_count    = 0;
static uint8_t g_board_spi_count    = 0;
static uint8_t g_board_servo_count  = 0;
static uint8_t g_board_pwm_count    = 0;

/*
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*                       Internal parser helpers
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
* _bp_is_separator: detect a markdown table separator row like |---|---|
* Returns true if the line consists only of |, -, :, and whitespace.
*/
static bool _bp_is_separator(const char *line) {
    while (*line == ' ' || *line == '|' || *line == '-' ||
           *line == ':' || *line == '\t') ++line;
    return (*line == '\0' || *line == '\r' || *line == '\n');
}

/*
* _bp_next_cell : extract the next pipe-delimited cell from *p.
* Advances *p past the trailing '|'. Returns false when line is exhausted.
*/
static bool _bp_next_cell(const char **p, char *out, uint8_t cap) {
    if (!*p || !**p) return false;
    while (**p == ' ' || **p == '\t') ++(*p);   // skip leading spaces
    if (**p == '|') ++(*p);                       // consume leading '|'
    while (**p == ' ' || **p == '\t') ++(*p);   // skip post-pipe spaces

    if (**p == '\0' || **p == '\r' || **p == '\n') return false;

    uint8_t n = 0;
    while (**p && **p != '|' && **p != '\r' && **p != '\n') {
        if (n + 1 < cap) out[n++] = **p;
        ++(*p);
    }
    // right-trim
    while (n > 0 && (out[n-1] == ' ' || out[n-1] == '\t')) --n;
    out[n] = '\0';
    return true;
}

/*
* _bp_parse_mode : map a mode string to an Arduino pin-mode constant.
*
* Supported tokens:
*   OUTPUT           → OUTPUT
*   INPUT            → INPUT          (floating)
*   INPUT_PULLUP     → INPUT_PULLUP   (both platforms)
*   INPUT_PULLDOWN   → INPUT_PULLDOWN (Pico W / RP2040 only)
*
* INPUT_PULLDOWN is silently mapped to INPUT on ESP32 because the
* Arduino-ESP32 core doesn't expose that constant by that name.
* Use INPUT_PULLUP with active-LOW signals on ESP32 instead.
*/
static uint8_t _bp_parse_mode(const char *m) {
    if (strncmp(m, "OUTPUT",         6)  == 0) return OUTPUT;
    if (strncmp(m, "INPUT_PULLUP",  12)  == 0) return INPUT_PULLUP;
#ifdef BOARD_PICO_W
    if (strncmp(m, "INPUT_PULLDOWN",14)  == 0) return INPUT_PULLDOWN;
#else
    if (strncmp(m, "INPUT_PULLDOWN",14)  == 0) {
        Serial.println("[Board] WARNING: INPUT_PULLDOWN not supported on ESP32 — using INPUT");
        return INPUT;
    }
#endif
    return INPUT;
}

/*
* _bp_mode_name — reverse of _bp_parse_mode, used for logging only.
*/
static const char *_bp_mode_name(uint8_t mode) {
    if (mode == OUTPUT)       return "OUTPUT";
    if (mode == INPUT_PULLUP) return "INPUT_PULLUP";
#ifdef BOARD_PICO_W
    if (mode == INPUT_PULLDOWN) return "INPUT_PULLDOWN";
#endif
    return "INPUT";
}

/* _bp_parse_hex8 — parse a hex or decimal byte (e.g. "0x3C" or "60").
 * Used for I2C addresses and bus indices.                               */
static uint8_t _bp_parse_hex8(const char *s) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return (uint8_t)strtol(s + 2, nullptr, 16);
    return (uint8_t)atoi(s);
}

/* _bp_parse_bus — extract numeric bus index from strings like "I2C0",
 * "SPI1", "0", "1".  Returns 0 as the default if no digit is found.    */
static uint8_t _bp_parse_bus(const char *s) {
    while (*s && !isdigit((unsigned char)*s)) ++s;
    return *s ? (uint8_t)atoi(s) : 0;
}

/*
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*                        UART port accessor
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
* board_get_uart: returns a pointer to the HardwareSerial object that
* corresponds to the given UART port number (1 or 2).
*
* NOTE: This is the single place where Serial1/Serial2 are referenced. Both
* board_init_hardware() and the action executor call this so there is no
* duplicated platform branching anywhere else.
*
* Returns nullptr if the port number is out of range for the board.
*
* Platform notes:
*   ESP32        — Serial1 and Serial2 accept pin numbers directly in begin().
*   ESP32-C3/C6  — Only Serial1 (UART1) is available; Serial2 returns nullptr.
*   Pico W       — Serial1 = UART0, Serial2 = UART1. Pin assignment must be
*                  done via setRX()/setTX() BEFORE calling begin(), which is
*                  handled in board_init_hardware().
*/
static HardwareSerial *board_get_uart(uint8_t port_num) {
#if defined(BOARD_ESP32)
  #if defined(CONFIG_IDF_TARGET_ESP32C3) /* || \
      defined(CONFIG_IDF_TARGET_ESP32C6) || \
      defined(CONFIG_IDF_TARGET_ESP32H2) */      // Uncomment for C6 & H2 support

    // Single-UART variants: only UART1 is user-accessible
    if (port_num == 1) return &Serial1;
    Serial.printf("[Board] WARNING: UART%u not available on this ESP32 variant, only UART1 exists\r\n",
                  port_num);
    return nullptr;
  #else
    // Full ESP32 / S3 : UART1 and UART2
    if (port_num == 1) return &Serial1;
    if (port_num == 2) return &Serial2;
    Serial.printf("[Board] WARNING: UART%u not supported, only UART1/UART2 on ESP32\r\n", port_num);
    return nullptr;
  #endif

#elif defined(BOARD_PICO_W)
    // RP2040: UART0 exposed as Serial1, UART1 exposed as Serial2
    if (port_num == 1) return &Serial1;
    if (port_num == 2) return &Serial2;
    Serial.printf("[Board] WARNING: UART%u not supported, only UART1/UART2 on Pico W\r\n", port_num);
    return nullptr;

#else
    (void)port_num;
    return nullptr;
#endif
}

/*
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*                        Public lookup API
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
* board_find_pin_by_name : GPIO pin lookup.
* Returns the physical pin number, or -1 if the name is not declared.
*/
static int board_find_pin_by_name(const char *name) {
    for (uint8_t i = 0; i < g_board_pin_count; ++i)
        if (strcmp(g_board_pins[i].name, name) == 0)
            return g_board_pins[i].pin;
    return -1;
}

/*
* board_find_adc_by_name : ADC pin lookup.
* Returns the physical pin number, or -1 if not declared.
*/
static int board_find_adc_by_name(const char *name) {
    for (uint8_t i = 0; i < g_board_adc_count; ++i)
        if (strcmp(g_board_adc[i].name, name) == 0)
            return g_board_adc[i].pin;
    return -1;
}

/*
* board_find_serial_by_name : UART port lookup.
* Returns index into g_board_serials[], or -1 if not declared.
*/
static int board_find_serial_by_name(const char *name) {
    for (uint8_t i = 0; i < g_board_serial_count; ++i)
        if (strcmp(g_board_serials[i].name, name) == 0)
            return i;
    return -1;
}

/*
* board_find_i2c_by_name : I2C port lookup.
* Returns index into g_board_serials[], or -1 if not declared.
*/
static int board_find_i2c_by_name(const char *name) {
    for (uint8_t i = 0; i < g_board_i2c_count; ++i)
        if (strcmp(g_board_i2c[i].name, name) == 0)
            return i;
    return -1;
}

/*
* board_find_spi_by_name : SPI port lookup.
* Returns index into g_board_serials[], or -1 if not declared.
*/
static int board_find_spi_by_name(const char *name) {
    for (uint8_t i = 0; i < g_board_spi_count; ++i)
        if (strcmp(g_board_spi[i].name, name) == 0)
            return i;
    return -1;
}

/*
* board_find_servo_by_name : Servo port lookup.
* Returns index into g_board_serials[], or -1 if not declared.
*/
static int board_find_servo_by_name(const char *name) {
    for (uint8_t i = 0; i < g_board_servo_count; ++i)
        if (strcmp(g_board_servos[i].name, name) == 0)
            return i;
    return -1;
}

/*
* board_find_pwm_by_name : PWM port lookup.
* Returns index into g_board_serials[], or -1 if not declared.
*/
static int board_find_pwm_by_name(const char *name) {
    for (uint8_t i = 0; i < g_board_pwm_count; ++i)
        if (strcmp(g_board_pwm[i].name, name) == 0)
            return i;
    return -1;
}


/*
* board_resolve_pin : resolve a value that may be a pin name OR a decimal
* number string. Searches GPIO names first, then ADC names and then parses as
* an integer. Returns -1 if none match.
*/
static int board_resolve_pin(const char *name_or_num) {
    int p = board_find_pin_by_name(name_or_num);
    if (p >= 0) return p;
    p = board_find_adc_by_name(name_or_num);
    if (p >= 0) return p;
    char *end;
    long n = strtol(name_or_num, &end, 10);
    return (end != name_or_num) ? (int)n : -1;
}

/*
* board_is_output_pin : is true if the named/numbered pin declared as OUTPUT.
* Prevents the action executor from writing to INPUT-mode pins.
*/
static bool board_is_output_pin(int pin_num) {
    for (uint8_t i = 0; i < g_board_pin_count; ++i)
        if (g_board_pins[i].pin == (uint8_t)pin_num)
            return g_board_pins[i].mode == OUTPUT;
    return false;
}

/*
* board_is_adc_pin : is true if the physical pin is declared in ## ADC Pins.
* Prevents arbitrary analogRead() on undeclared pins.
*/
static bool board_is_adc_pin(int pin_num) {
    for (uint8_t i = 0; i < g_board_adc_count; ++i)
        if (g_board_adc[i].pin == (uint8_t)pin_num) return true;
    return false;
}

/*
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*                       Action parameter helpers
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*
* Action buffers, e.g. "gpio_set pin=relay1 value=1"
*
* board_parse_action_str — extract a string value from key=value in buf.
* Value may be quoted ("hello world") or bare (up to next space).
*/
static bool board_parse_action_str(const char *buf, const char *key,
                                   char *out, uint8_t cap) {
    char needle[32];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(buf, needle);
    if (!p) { out[0] = '\0'; return false; }
    p += strlen(needle);
    uint8_t n = 0;
    if (*p == '"') {
        ++p;
        while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
    } else {
        while (*p && *p != ' ' && *p != '\t' && n + 1 < cap) out[n++] = *p++;
    }
    out[n] = '\0';
    return true;
}

/*
* board_parse_action_int : extract an integer value from key=value in buf.
* Returns -1 if key is absent or value is non-numeric.
*/
static int board_parse_action_int(const char *buf, const char *key) {
    char tmp[32];
    if (!board_parse_action_str(buf, key, tmp, sizeof(tmp))) return -1;
    char *end;
    long v = strtol(tmp, &end, 10);
    return (end != tmp) ? (int)v : -1;
}

/*
* board_resolve_action_pin : resolve a pin param that may be a name or
* number. Combines board_parse_action_str + board_resolve_pin.
*/
static int board_resolve_action_pin(const char *buf, const char *key) {
    char tmp[32];
    if (!board_parse_action_str(buf, key, tmp, sizeof(tmp))) return -1;
    return board_resolve_pin(tmp);
}

/*
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*                      Markdown parser
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
* Parse a CONTROL.md string into g_board_pins[], g_board_serials[] and
* g_board_adc[]. Resets all counters before parsing.
* Returns true if at least one pin/port was successfully parsed.
*
* Parser is deliberately forgiving:
*   • Unknown ## sections are silently skipped.
*   • Rows with missing mandatory cells (Pin, Name) are skipped.
*   • Rows beyond MAX_BOARD_* limits are skipped with warning.
*/
static bool board_parse_md(const char *md) {
    g_board_pin_count    = 0;
    g_board_serial_count = 0;
    g_board_adc_count    = 0;
    g_board_i2c_count    = 0;
    g_board_spi_count    = 0;
    g_board_servo_count  = 0;
    g_board_pwm_count    = 0;

    enum Section {
        SEC_NONE, SEC_GPIO, SEC_SERIAL, SEC_ADC,
        SEC_I2C, SEC_SPI, SEC_SERVO, SEC_PWM
    } section = SEC_NONE;

    bool header_seen    = false;
    bool separator_seen = false;

    const char *line = md;
    while (*line) {
        // ── Find end of current line ──────────────────────────────────
        const char *eol = line;
        while (*eol && *eol != '\n') ++eol;

        // Copy into mutable buffer and strip \r
        char lbuf[192];
        uint8_t llen = (uint8_t)min((ptrdiff_t)191, eol - line);
        memcpy(lbuf, line, llen);
        lbuf[llen] = '\0';
        char *cr = strchr(lbuf, '\r'); if (cr) *cr = '\0';

        // ── Section header detection ──────────────────────────────────
        if      (!strncmp(lbuf, "## GPIO",   7)) { section = SEC_GPIO;   header_seen = separator_seen = false; }
        else if (!strncmp(lbuf, "## Serial", 9)) { section = SEC_SERIAL; header_seen = separator_seen = false; }
        else if (!strncmp(lbuf, "## ADC",    6)) { section = SEC_ADC;    header_seen = separator_seen = false; }
        else if (!strncmp(lbuf, "## I2C",    6)) { section = SEC_I2C;    header_seen = separator_seen = false; }
        else if (!strncmp(lbuf, "## SPI",    6)) { section = SEC_SPI;    header_seen = separator_seen = false; }
        else if (!strncmp(lbuf, "## Servo",  8)) { section = SEC_SERVO;  header_seen = separator_seen = false; }
        else if (!strncmp(lbuf, "## PWM",    6)) { section = SEC_PWM;    header_seen = separator_seen = false; }
        else if (!strncmp(lbuf, "## ",       3)) { section = SEC_NONE; }

        // ── Table row processing ──────────────────────────────────────
        else if (section != SEC_NONE && lbuf[0] == '|') {
            if (_bp_is_separator(lbuf)) {
                separator_seen = true;
            } else if (!header_seen) {
                header_seen = true;   // first non-separator pipe row = column headers
            } else if (separator_seen) {
                // ── Data row ─────────────────────────────────────────
                const char *p = lbuf + 1; // skip leading '|'
                char c0[32]={}, c1[32]={}, c2[64]={}, c3[16]={}, c4[96]={};

                if (section == SEC_GPIO) {
                    // Columns: | Pin | Mode | Name | Logic | Description |
                    _bp_next_cell(&p, c0, sizeof(c0)); // Pin
                    _bp_next_cell(&p, c1, sizeof(c1)); // Mode
                    _bp_next_cell(&p, c2, sizeof(c2)); // Name
                    _bp_next_cell(&p, c3, sizeof(c3)); // Logic
                    _bp_next_cell(&p, c4, sizeof(c4)); // Description
                    if (c0[0] && c1[0] && c2[0]) {
                        if (g_board_pin_count >= MAX_BOARD_PINS) {
                            Serial.printf("[Board] WARNING: GPIO '%s' exceeds MAX_BOARD_PINS=%u — skipped\r\n",
                                          c2, MAX_BOARD_PINS);
                        } else {
                            BoardPin &bp = g_board_pins[g_board_pin_count++];
                            bp.pin    = (uint8_t)atoi(c0);
                            bp.mode   = _bp_parse_mode(c1);
                            bp.inverted = (strncasecmp(c3, "inverted", 8) == 0);
                            strlcpy(bp.name, c2, sizeof(bp.name));
                            strlcpy(bp.desc, c4, sizeof(bp.desc));
                            bp.active = true;

                        }
                    }

                } else if (section == SEC_SERIAL) {
                    // Columns: | Port | Baud | RX Pin | TX Pin | Name | Description |
                    char c4[32]={}, c5[96]={};
                    _bp_next_cell(&p, c0, sizeof(c0)); // Port  (e.g. "UART1")
                    _bp_next_cell(&p, c1, sizeof(c1)); // Baud
                    _bp_next_cell(&p, c2, sizeof(c2)); // RX Pin
                    _bp_next_cell(&p, c3, sizeof(c3)); // TX Pin
                    _bp_next_cell(&p, c4, sizeof(c4)); // Name
                    _bp_next_cell(&p, c5, sizeof(c5)); // Description
                    if (c0[0] && c4[0]) {
                        if (g_board_serial_count >= MAX_BOARD_SERIALS) {
                            Serial.printf("[Board] WARNING: serial port '%s' exceeds MAX_BOARD_SERIALS=%u — skipped\r\n",
                                          c4, MAX_BOARD_SERIALS);
                        } else {
                            BoardSerial &bs = g_board_serials[g_board_serial_count++];
                            // Extract numeric suffix: "UART1" → 1, "UART2" → 2
                            const char *dn = c0;
                            while (*dn && !isdigit((unsigned char)*dn)) ++dn;
                            bs.port_num = *dn ? (uint8_t)atoi(dn) : 1;
                            bs.baud     = (uint32_t)atol(c1);
                            bs.rx_pin   = (uint8_t)atoi(c2);
                            bs.tx_pin   = (uint8_t)atoi(c3);
                            strlcpy(bs.name, c4, sizeof(bs.name));
                            strlcpy(bs.desc, c5, sizeof(bs.desc));
                            bs.active   = true;

                            // ── single-UART guard ───────────
#if defined(BOARD_ESP32) && (defined(CONFIG_IDF_TARGET_ESP32C3) || \
                              defined(CONFIG_IDF_TARGET_ESP32C6) || \
                              defined(CONFIG_IDF_TARGET_ESP32H2))
                            if (bs.port_num != 1)
                                Serial.printf("[Board] WARNING: UART%u '%s' — only UART1 exists on this ESP32 variant\r\n",
                                              bs.port_num, bs.name);
#endif
                        }
                    }

                } else if (section == SEC_ADC) {
                    // Columns: | Pin | Name | Description |
                    _bp_next_cell(&p, c0, sizeof(c0)); // Pin
                    _bp_next_cell(&p, c1, sizeof(c1)); // Name
                    _bp_next_cell(&p, c2, sizeof(c2)); // Description
                    if (c0[0] && c1[0]) {
                        if (g_board_adc_count >= MAX_BOARD_ADC) {
                            Serial.printf("[Board] WARNING: ADC pin '%s' exceeds MAX_BOARD_ADC=%u — skipped\r\n",
                                          c1, MAX_BOARD_ADC);
                        } else {
                            BoardAdc &ba = g_board_adc[g_board_adc_count++];
                            ba.pin = (uint8_t)atoi(c0);
                            strlcpy(ba.name, c1, sizeof(ba.name));
                            strlcpy(ba.desc, c2, sizeof(ba.desc));
                            ba.active = true;

                            // ── Pico W: ADC pins must be GP26-GP29 ────
#ifdef BOARD_PICO_W
                            if (ba.pin < 26 || ba.pin > 29)
                                Serial.printf("[Board] WARNING: GP%u is not ADC-capable on Pico W (valid: GP26-GP29)\r\n",
                                              ba.pin);
#endif
                            // ── ESP32-C3: ADC2 unavailable with WiFi ──
#if defined(BOARD_ESP32) && defined(CONFIG_IDF_TARGET_ESP32C3)
                            if (ba.pin > 4)
                                Serial.printf("[Board] WARNING: ADC pin %u may be unavailable on ESP32-C3 while WiFi is active (use pins 0-4)\r\n",
                                              ba.pin);
#endif
                        }
                    }

                // ── I2C ──────────────────────────────────────────────
                } else if (section == SEC_I2C) {
                    // | Bus | SDA | SCL | Address | Name | Description |
                    char c4[32]={}, c5[96]={};
                    _bp_next_cell(&p, c0, sizeof(c0)); // Bus  (e.g. "I2C0")
                    _bp_next_cell(&p, c1, sizeof(c1)); // SDA
                    _bp_next_cell(&p, c2, sizeof(c2)); // SCL
                    _bp_next_cell(&p, c3, sizeof(c3)); // Address (e.g. "0x3C")
                    _bp_next_cell(&p, c4, sizeof(c4)); // Name
                    _bp_next_cell(&p, c5, sizeof(c5)); // Description
                    if (c1[0] && c2[0] && c4[0]) {
                        if (g_board_i2c_count >= MAX_BOARD_I2C) {
                            Serial.printf("[Board] WARNING: I2C '%s' exceeds MAX_BOARD_I2C=%u — skipped\r\n", c4, MAX_BOARD_I2C);
                        } else {
                            BoardI2C &bi = g_board_i2c[g_board_i2c_count++];
                            bi.bus  = _bp_parse_bus(c0);
                            bi.sda  = (uint8_t)atoi(c1);
                            bi.scl  = (uint8_t)atoi(c2);
                            bi.addr = _bp_parse_hex8(c3);
                            strlcpy(bi.name, c4, sizeof(bi.name));
                            strlcpy(bi.desc, c5, sizeof(bi.desc));
                            bi.active = true;
                        }
                    }

                // ── SPI ──────────────────────────────────────────────
                } else if (section == SEC_SPI) {
                    // | Bus | MOSI | MISO | SCK | CS | Name | Description |
                    char c4[16]={}, c5[32]={}, c6[96]={};
                    _bp_next_cell(&p, c0, sizeof(c0)); // Bus  (e.g. "SPI0")
                    _bp_next_cell(&p, c1, sizeof(c1)); // MOSI
                    _bp_next_cell(&p, c2, sizeof(c2)); // MISO
                    _bp_next_cell(&p, c3, sizeof(c3)); // SCK
                    _bp_next_cell(&p, c4, sizeof(c4)); // CS
                    _bp_next_cell(&p, c5, sizeof(c5)); // Name
                    _bp_next_cell(&p, c6, sizeof(c6)); // Description
                    if (c1[0] && c3[0] && c5[0]) {
                        if (g_board_spi_count >= MAX_BOARD_SPI) {
                            Serial.printf("[Board] WARNING: SPI '%s' exceeds MAX_BOARD_SPI=%u — skipped\r\n", c5, MAX_BOARD_SPI);
                        } else {
                            BoardSPI &bs = g_board_spi[g_board_spi_count++];
                            bs.bus  = _bp_parse_bus(c0);
                            bs.mosi = (uint8_t)atoi(c1);
                            bs.miso = (uint8_t)atoi(c2);
                            bs.sck  = (uint8_t)atoi(c3);
                            bs.cs   = (uint8_t)atoi(c4);
                            strlcpy(bs.name, c5, sizeof(bs.name));
                            strlcpy(bs.desc, c6, sizeof(bs.desc));
                            bs.active = true;
                        }
                    }

                // ── Servo ────────────────────────────────────────────
                } else if (section == SEC_SERVO) {
                    // | Pin | Name | Min | Max | Step | Delay | Description |
                    char c4[16]={}, c5[16]={}, c6[96]={};
                    _bp_next_cell(&p, c0, sizeof(c0)); // Pin
                    _bp_next_cell(&p, c1, sizeof(c1)); // Name
                    _bp_next_cell(&p, c2, sizeof(c2)); // Min
                    _bp_next_cell(&p, c3, sizeof(c3)); // Max
                    _bp_next_cell(&p, c4, sizeof(c4)); // Step
                    _bp_next_cell(&p, c5, sizeof(c5)); // Delay
                    _bp_next_cell(&p, c6, sizeof(c6)); // Description
                    if (c0[0] && c1[0]) {
                        if (g_board_servo_count >= MAX_BOARD_SERVOS) {
                            Serial.printf("[Board] WARNING: Servo '%s' exceeds MAX_BOARD_SERVOS=%u — skipped\r\n", c1, MAX_BOARD_SERVOS);
                        } else {
                            BoardServo &sv = g_board_servos[g_board_servo_count++];
                            sv.pin       = (uint8_t)atoi(c0);
                            sv.min_angle = c2[0] ? (uint16_t)atoi(c2) : 0;
                            sv.max_angle = c3[0] ? (uint16_t)atoi(c3) : 180;
                            sv.servo_step = c4[0] ? (uint8_t)atoi(c4) : 1;       // default step -> 1 degree
                            sv.step_delay_ms = c5[0] ? (uint16_t)atoi(c5) : 20;  // default delay -> 20 ms
                            strlcpy(sv.name, c1, sizeof(sv.name));
                            strlcpy(sv.desc, c6, sizeof(sv.desc));
                            sv.active = true;
                        }
                    }

                // ── PWM ──────────────────────────────────────────────
                } else if (section == SEC_PWM) {
                    // | Pin | Name | Freq | Resolution | Description |
                    char c4[96]={};
                    _bp_next_cell(&p, c0, sizeof(c0)); // Pin
                    _bp_next_cell(&p, c1, sizeof(c1)); // Name
                    _bp_next_cell(&p, c2, sizeof(c2)); // Freq
                    _bp_next_cell(&p, c3, sizeof(c3)); // Resolution
                    _bp_next_cell(&p, c4, sizeof(c4)); // Description
                    if (c0[0] && c1[0]) {
                        if (g_board_pwm_count >= MAX_BOARD_PWM) {
                            Serial.printf("[Board] WARNING: PWM '%s' exceeds MAX_BOARD_PWM=%u — skipped\r\n", c1, MAX_BOARD_PWM);
                        } else {
                            BoardPWM &pw = g_board_pwm[g_board_pwm_count++];
                            pw.pin        = (uint8_t)atoi(c0);
                            pw.freq       = c2[0] ? (uint32_t)atol(c2) : 1000;
                            pw.resolution = c3[0] ? (uint8_t)atoi(c3)  : 8;
                            pw.channel    = 0;   // assigned in board_init_peripherals()
                            strlcpy(pw.name, c1, sizeof(pw.name));
                            strlcpy(pw.desc, c4, sizeof(pw.desc));
                            pw.active = true;
                        }
                    }
                }
            }
        }

        line = (*eol == '\n') ? eol + 1 : eol;
    }

    Serial.printf("[Board] Parse — %u GPIO, %u UART, %u ADC, %u I2C, %u SPI, %u Servo, %u PWM\r\n",
                  g_board_pin_count, g_board_serial_count, g_board_adc_count,
                  g_board_i2c_count,  g_board_spi_count,
                  g_board_servo_count, g_board_pwm_count);

    return (g_board_pin_count + g_board_serial_count + g_board_adc_count +
            g_board_i2c_count  + g_board_spi_count   +
            g_board_servo_count + g_board_pwm_count) > 0;
}


/*
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*                        Hardware controls
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
* Configure all declared GPIO pins and UART ports.
* All OUTPUT pins are driven LOW at startup (safe default).
*
* ESP32 (all variants):
*   Serial1.begin(baud, SERIAL_8N1, rx, tx) — pins passed directly.
*
* Pico W (RP2040):
*   Pin assignment MUST happen via setRX()/setTX() BEFORE begin().
*   Calling begin() first locks the default pins and ignores later setRX/TX.
*   Serial1 = UART0, Serial2 = UART1.
* Wire, Servo.attach(), and ledcSetup() are NOT done here because they
* require optional library headers guarded by build flags.
*/
static void board_init_hardware() {
    // ── GPIO ─────────────────────────────────────────────────────────────────
    for (uint8_t i = 0; i < g_board_pin_count; ++i) {
        const BoardPin &bp = g_board_pins[i];
        pinMode(bp.pin, bp.mode);
        if (bp.mode == OUTPUT) digitalWrite(bp.pin, LOW);
        Serial.printf("[Board] GPIO %-2u  [%-14s]  '%s'\r\n",
                      bp.pin, _bp_mode_name(bp.mode), bp.name);
    }

    // ── UART ──────────────────────────────────────────────────────────────────
    for (uint8_t i = 0; i < g_board_serial_count; ++i) {
        const BoardSerial &bs = g_board_serials[i];
        HardwareSerial *hs = board_get_uart(bs.port_num);
        if (!hs) continue;

#if defined(BOARD_ESP32)
        // ESP32 Arduino core: pin numbers are passed directly into begin().
        hs->begin(bs.baud, SERIAL_8N1, bs.rx_pin, bs.tx_pin);

#elif defined(BOARD_PICO_W)
        // Pico W : MUST assign pins before begin().
        // setRX/setTX accept the GP number (0-29).
        SerialUART *su = static_cast<SerialUART *>(hs);
        su->setRX(bs.rx_pin);
        su->setTX(bs.tx_pin);
        su->begin(bs.baud);
#endif

        Serial.printf("[Board] UART%u  '%s'  baud=%-7lu  rx=GP%-2u  tx=GP%-2u\r\n",
                      bs.port_num, bs.name, (unsigned long)bs.baud,
                      bs.rx_pin, bs.tx_pin);
    }
}

/*
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*                         Serial Write
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
* Write a null-terminated string to a named UART port.
* Returns the number of bytes written, or -1 if the port is not declared
* or the underlying HardwareSerial object is unavailable.
*
* Called by execute_actions_in_response() for [ACTION:serial_write ...].
*/
static int board_serial_write(int serial_index, const char *data) {
    if (serial_index < 0 || serial_index >= g_board_serial_count) return -1;
    HardwareSerial *hs = board_get_uart(g_board_serials[serial_index].port_num);
    if (!hs) return -1;
    return (int)hs->print(data);
}

/*
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*                        Serial Read
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
* Read up to (cap-1) bytes from a named UART port into buf, waiting at most
* timeout_ms for data to arrive. Null-terminates the result.
* Returns number of bytes read.
*
***** USB-CDC keepalive *****
* On native USB boards (C3) the host drops the COM port after ~500 ms of
* USB silence. The keepalive (in femtoclaw_mcu.cpp) fires every 200 ms, but
* it cannot be called from here => board_parser.h is included before
* usb_keepalive is defined. Default and hard-cap are both 150 ms so this
* function can never starve the keepalive regardless of what's passed in.
*
* Called by execute_actions_in_response() for [ACTION:serial_read ...].
*/
static uint8_t board_serial_read(int serial_index, char *buf, uint8_t cap,
                                  uint32_t timeout_ms = 150) {
    // Hard cap: never block > 150 ms (USB-CDC keepalive safety)
    if (timeout_ms > 150) timeout_ms = 150;
    if (serial_index < 0 || serial_index >= g_board_serial_count ||
        cap == 0) { if (cap) buf[0] = '\0'; return 0; }

    HardwareSerial *hs = board_get_uart(g_board_serials[serial_index].port_num);
    if (!hs) { buf[0] = '\0'; return 0; }

    uint8_t n = 0;
    unsigned long t0 = millis();
    while ((millis() - t0) < timeout_ms && n + 1 < cap) {
        if (hs->available()) {
            buf[n++] = (char)hs->read();
        } else {
            delay(1);
        }
    }
    buf[n] = '\0';
    return n;
}

/*
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*                        Hardware reset
* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
* Drive all OUTPUT pins LOW and clear all parsed state.
* Servo detach and PWM channel release are handled in fc_actions.h
* (board_reset_peripherals) which is called from the shell reset handler.
*/
static void board_reset_hardware() {
    for (uint8_t i = 0; i < g_board_pin_count; ++i)
        if (g_board_pins[i].mode == OUTPUT)
            digitalWrite(g_board_pins[i].pin, LOW);

    g_board_pin_count    = 0;
    g_board_serial_count = 0;
    g_board_adc_count    = 0;
    g_board_i2c_count    = 0;
    g_board_spi_count    = 0;
    g_board_servo_count  = 0;
    g_board_pwm_count    = 0;

    Serial.println("[Board] Hardware reset — all outputs LOW, config cleared.");
}








