#pragma once

/*  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*                          HTTP / HTTPS POST / GET
*   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*
* Three dedicated TLS clients : one per remote host family.
*
*   g_tls_llm  — exclusively for LLM API calls (llm_chat)
*   g_tls_tg   — exclusively for Telegram API (tg_poll + tg_send)
*   g_tls_dc   — exclusively for Discord API  (dc_poll + dc_send)
*
*/
static WiFiClientSecure g_tls_llm;
static WiFiClientSecure g_tls_tg;
static WiFiClientSecure g_tls_dc;
static WiFiClient       g_tcp;

static char g_http_resp[HTTP_RESP_S];
static bool g_http_busy = false;            // true while any network I/O is in progress
static bool g_http_streaming = false;       // true while reading response body
static bool g_suppress_tls_logs = false;    // suppress TLS messages for background Telegram/Discord polling

// ─── TLS setInsecure helper ──────────────────────────────────────────────────
static void tls_set_insecure(WiFiClientSecure &tls) {
#ifdef BOARD_ESP32
  tls.setInsecure();              // Arduino-ESP32: skip certificate verification
#endif
#ifdef BOARD_PICO_W
  /* Pico W Arduino core (earlephilhower): WiFiClientSecure::setInsecure()
   * exists from core ≥ 3.x and maps to BearSSL trust-none mode.
   * If your core version doesn't have it, replace with:
   *   tls.setCACert(nullptr); tls.setVerification(WiFiClientSecure::NONE);
   */
  tls.setInsecure();
#endif
}

/*
* _stream_readline : read one CRLF-terminated line from a Stream.
* Times out after timeout_ms. Works on any Arduino Stream (WiFiClient,
* WiFiClientSecure) on any board without platform-specific dependencies.
*/

/*  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*                           USB-CDC keepalive
*   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*
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
  if (g_http_streaming) return; // don't inject null bytes during response streaming
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
  return total;     // (silent truncation - caller handles buffer sizing)
}

/*
*   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*                     Chunked transfer-encoding decoder
*   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*/
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
        prev_lf = false;           // non-CR/LF char resets bare-LF detector
      }
      // \r doesn't reset prev_lf : allows \r\n\r\n to trigger both paths

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
  return code;  // timeout or disconnect : return what we have
}

// _stream_send_req : send HTTP request header and body (if any).
// If body is nullptr or body_len is 0, sends a GET; otherwise POST.
template<typename T>
static void _stream_send_req(T &client, const char *host, const char *path,
                               const char *extra_headers,
                               const char *body, uint16_t body_len) {
  // USB keepalive during request assembly on ESP32-C3 native USB, the TX
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
  delay(TLS_SETTLE_MS);
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
  g_http_streaming = true;  // start blocking keepalive
  uint16_t blen = _stream_read_body(tls, out, out_cap);
  g_http_streaming = false; // resume keepalive
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
  // Pass 'host' (port stripped) to _stream_send_req, it's used for the Host:
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

/*
*   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*                           Shared TX buffers
*   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*/
static char g_tx_body[JSON_OUT_S];
static char g_tx_auth[LLM_KEY + 32];
static char g_tx_path[CFG_S];

/*
*   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*                           Base-64 decoder
*   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
*/
static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static uint16_t base64_decode(const char *in, uint16_t in_len,
                               char *out, uint16_t out_cap) {
    uint16_t w = 0;
    int val = 0, valb = -8;
    for (uint16_t i = 0; i < in_len && w + 1 < out_cap; ++i) {
        char c = in[i];
        if (c == '=') break;
        const char *pos = strchr(b64_table, c);
        if (!pos) continue;   // skip whitespace / newlines silently
        val  = (val << 6) + (int)(pos - b64_table);
        valb += 6;
        if (valb >= 0) {
            out[w++] = (char)((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    out[w] = '\0';
    return w;
}