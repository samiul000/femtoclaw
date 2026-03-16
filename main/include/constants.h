#pragma once

static constexpr uint32_t UART_BAUD         = 115200;
static constexpr uint32_t HTTP_TIMEOUT_MS   = 60000;
static constexpr uint32_t TG_POLL_MS        = 5000;
static constexpr uint32_t DC_POLL_MS        = 5000;
static constexpr uint16_t TG_MSG_CHUNK      = 3800;
static constexpr uint16_t DC_MSG_CHUNK      = 1800;
static constexpr uint16_t TLS_SETTLE_MS     = 100;
static constexpr uint16_t CHUNK             = 512;
static constexpr uint16_t CFG_S             = 128;
static constexpr uint16_t LLM_KEY           = 256;
static constexpr uint16_t RESP_S            = 2048;
static constexpr uint16_t PROMPT_S          = 1024;
static constexpr uint16_t JSON_OUT_S        = 8192;
static constexpr uint16_t HTTP_RESP_S       = 8192;  // raised if needed but not recommended for long responses + headers
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