#pragma once

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
  // namespace rp2040 { extern void reboot(); }
#endif

// ─── LED pin ────────────────────────────────────────────────────────
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