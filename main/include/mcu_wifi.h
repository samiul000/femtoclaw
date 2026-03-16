#pragma once
#include <WiFi.h>

// ─── WiFi ────────────────────────────────────────────────────────────────────
static void wifi_connect(uint8_t retries = 20) {
  if (!g_cfg.wifi_ssid[0]) return;
  if (WiFi.status() == WL_CONNECTED) { Serial.println("[WiFi] already connected."); return; }

  Serial.printf("[WiFi] connecting to '%s' ...\r\n", g_cfg.wifi_ssid);
#ifndef ARDUINO_USB_CDC_ON_BOOT
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  delay(100);
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