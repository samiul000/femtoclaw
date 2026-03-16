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

#include "platform.h"           // Platform headers, build flag guards, LED_PIN
#include "constants.h"          // Compile-time buffer sizes and timing constants
#include "config.h"             // Config struct + global g_cfg
#include "board_parser.h"       // Hardware parser : structs, parse, GPIO/UART init helpers
#include "json.h"               // Zero-alloc JSON helpers : used by persist, llm, channels
#include "mcu_wifi.h"           // WiFi config
#include "persist.h"            // Persistent config: cfg_save / cfg_load
#include "http.h"               // HTTP/HTTPS transport: TLS clients, usb_keepalive, stream helpers,
#include "llm.h"                // LLM: system prompt, session management, llm_chat()
#include "actions.h"            // Action executor + optional peripheral init (Wire, Servo, LEDC, displays)
#include "agent.h"              // Agentic loop: tool_dispatch + agent_run
#include "telegram.h"           // Telegram long-polling channel
#include "discord.h"            // Discord HTTP REST channel
#include "heartbeat.h"          // Periodic heartbeat
#include "shell.h"              // UART shell + board push state machine

// ─── Arduino entry points ─────────────────────────────────────────────────────
void setup() {
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  // ── ESP32-C3/C6 native USB boot sequence ─────────────────────────────────
  /* WiFi.mode(WIFI_STA) MUST be called before Serial.begin()
  * on ESP32-C3 with native USB-CDC.
  *
  * Cause: the USB-Serial/JTAG controller and the WiFi RF subsystem share
  * the same internal clock domain on ESP32-C3. If WiFi is initialized while
  * USB-CDC is already active, the two subsystems conflict and the chip triggers
  * RTC_SW_SYS_RST (saved PC 0x403cf94c) producing an infinite boot loop.
  */

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
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

  bool board_need_peripherals = false;
  if (g_cfg.board_md_loaded) {
    if (board_parse_md(g_cfg.board_md)) {
      board_init_hardware();
      // board_init_peripherals();
      board_need_peripherals = true;
    } else {
      // Stored markdown is corrupt or empty, clear it so next boot is clean.
      g_cfg.board_md_loaded = false;
      g_cfg.board_md[0]     = '\0';
      cfg_save();
      Serial.println("[Board] WARNING: stored config parse failed! cleared from flash.");
    }
  }

  Serial.println(
    "\r\n\033[1;35m"
    "  ███████╗███████╗███╗   ███╗████████╗ ██████╗  ██████╗██╗      █████╗ ██╗    ██╗\r\n"
    "  ██╔════╝██╔════╝████╗ ████║╚══██╔══╝██╔═══██╗██╔════╝██║     ██╔══██╗██║    ██║\r\n"
    "  █████╗  █████╗  ██╔████╔██║   ██║   ██║   ██║██║     ██║     ███████║██║ █╗ ██║\r\n"
    "  ██╔══╝  ██╔══╝  ██║╚██╔╝██║   ██║   ██║   ██║██║     ██║     ██╔══██║██║███╗██║\r\n"
    "  ██║     ███████╗██║ ╚═╝ ██║   ██║   ╚██████╔╝╚██████╗███████╗██║  ██║╚███╔███╔╝\r\n"
    "  ╚═╝     ╚══════╝╚═╝     ╚═╝   ╚═╝    ╚═════╝  ╚═════╝╚══════╝╚═╝  ╚═╝ ╚══╝╚══╝\r\n"
    "\033[0m"
    "  FemtoClaw AI Assistant for MCU · " PLATFORM_NAME " · Telegram & Discord\r\n"
    "  Developed by: Al Mahmud Samiul\r\n"
    "  Type 'help' for commands.\r\n");

  if (g_cfg.wifi_ssid[0]) wifi_connect();
  else Serial.println("[!] No WiFi set. Use: wifi <ssid> <pass>  then  connect");

  if (board_need_peripherals) {
    board_init_peripherals();
    Serial.printf("[Board] Restored from flash : "
                  "%u GPIO, %u UART, %u ADC, %u I2C, %u SPI, %u Servo, %u PWM\r\n",
                  g_board_pin_count, g_board_serial_count, g_board_adc_count,
                  g_board_i2c_count,  g_board_spi_count,
                  g_board_servo_count, g_board_pwm_count);
  }

  if (g_cfg.telegram.enabled)
    Serial.printf("[Telegram] Enabled polling every %lus  allow_count=%u\r\n",
                  (unsigned long)(TG_POLL_MS/1000), (unsigned)g_cfg.telegram.allow_count);
  if (g_cfg.discord.enabled)
    Serial.println("[Discord]  Channel enabled polling started.");

  digitalWrite(LED_PIN, LOW);
  shell_prompt();
}

/*
 * loop() : cooperative main loop.
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
      // USB reconnected : settle the PHY, then decide how to re-prompt
      delay(50);
      if (g_http_busy) {
        // LLM / Telegram / Discord request is in-flight, tell the user
        // but do NOT print the normal prompt (it would appear mid-response)
        Serial.println("\r\n[femtoclaw] reconnected : waiting for network response...");
      } else if (g_cmd_len == 0) {
        // Idle and buffer is empty, safe to re-prompt normally
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
  while (Serial.available()) shell_byte((uint8_t)Serial.read());
#endif

  if (WiFi.status() == WL_CONNECTED && !g_http_busy) {
    tg_poll();
    dc_poll();
    heartbeat_check();
  }
  yield();
}
