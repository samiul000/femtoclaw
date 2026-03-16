#pragma once


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

static uint16_t json_escape_into(char *dst, uint16_t cap, const char *s) {
  uint16_t w = 0;
  for (; *s && w + 6 < cap; ++s) {
    switch ((uint8_t)*s) {
      case '"':  dst[w++]='\\'; dst[w++]='"';  break;
      case '\\': dst[w++]='\\'; dst[w++]='\\'; break;
      case '\n': dst[w++]='\\'; dst[w++]='n';  break;
      case '\r': dst[w++]='\\'; dst[w++]='r';  break;
      case '\t': dst[w++]='\\'; dst[w++]='t';  break;
      default:   dst[w++]=*s;  break;
    }
  }
  dst[w] = '\0';
  return w;
}

/*
 * json_escape_n_into used by llm_chat() for session history
 * entries, whose content is bounded by '\x02' delimiters, not null bytes.
 */
static uint16_t json_escape_n_into(char *dst, uint16_t cap,
                                    const char *s, uint16_t slen) {
  uint16_t w = 0;
  for (uint16_t i = 0; i < slen && w + 6 < cap; ++i) {
    switch ((uint8_t)s[i]) {
      case '"':  dst[w++]='\\'; dst[w++]='"';  break;
      case '\\': dst[w++]='\\'; dst[w++]='\\'; break;
      case '\n': dst[w++]='\\'; dst[w++]='n';  break;
      case '\r': dst[w++]='\\'; dst[w++]='r';  break;
      case '\t': dst[w++]='\\'; dst[w++]='t';  break;
      default:   dst[w++]=s[i]; break;
    }
  }
  dst[w] = '\0';
  return w;
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
  // If provided, jstr stops there even if no closing " is found,
  // prevents reading past a truncated HTTP response buffer into garbage memory.
  if (!p || *p != '"') return false;
  ++p;
  uint16_t w = 0;
  while (*p && w + 1 < cap) {
    if (buf_end && p >= buf_end) break;  // buffer boundary —> stop safely
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
 *   `jint` : parse an unquoted JSON integer into int64_t.
 *   Skips leading whitespace only. Does NOT skip quotes, if the JSON value is
 *   a string (e.g. Discord Snowflakes: "id": "12345"), jint returns 0 and the
 *   caller must use id_from_str() instead. This prevents silent precision loss
 *   from parsing large Snowflakes through strtoll (int64 max = 19 digits, and
 *   some toolchains treat long long as 32-bit in embedded targets).
 */
static int64_t jint(const char *p) {
  if (!p) return 0;
  while (*p == ' ') ++p;           // skip whitespace only, not the quotes
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

// id_from_str : copy a string ID (Discord Snowflake from JSON string field)
// into a fixed buffer. Returns false and zeroes the buffer on truncation.
// IMPORTANT: tmp is a local (not static) : id_from_str may be called twice
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