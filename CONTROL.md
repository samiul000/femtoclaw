# FemtoClaw : Hardware Skill System
**Markdown-Driven Hardware Control via AI**

---

## Overview

This document describes the full design for adding natural-language hardware control (GPIO, serial ports, I2C, SPI, Servo and PWM) to FemtoClaw,
using a markdown-based board configuration system that is pushed from the GUI and read by the AI at initialization to self-configure its capabilities.

---

### Key Hardware Notes

**ESP32-C3 ADC restriction:** ADC2 is disabled when WiFi is active. Only GPIO pins **0–4** (ADC1) are safely readable via `adc_read`. Declaring an ADC pin above 4 on an ESP32-C3 will print a warning at parse time.

**Pico W ADC restriction:** Only **GP26, GP27, GP28, GP29** are ADC-capable. All other GP numbers are digital-only. The parser warns if an out-of-range ADC pin is declared.

**ESP32-C3 UART restriction:** Only **UART1** (Serial1) is user-accessible. UART0 is consumed by the USB-CDC console. Declaring UART2 in `## Serial Ports` will log a warning and be ignored.

**ESP32 LEDC channels:** ESP32/S3 have 16 LEDC channels total. Servo motors claim channels 0–(N-1) first; PWM outputs take the next available. A warning is printed if the total exceeds 16.

**Inverted logic:** Add `inverted` to the Logic column of a GPIO pin to flip its logical value. The AI always writes logical `1 = ON`, `0 = OFF`; the firmware handles the inversion transparently. Useful for active-LOW relays.

### Optional Hardware Libraries (build flags)

| Feature        | Build flag                | Library required             | Notes                                        |
|----------------|---------------------------|------------------------------|----------------------------------------------|
| Servo motors   | `-DBOARD_HAS_SERVO`       | `ESP32Servo` (ESP32 only)    | Pico W uses built-in `Servo.h`, no lib needed|
| OLED SSD1306   | `-DBOARD_HAS_OLED_SSD1306`| `Adafruit SSD1306` + GFX     | 128×64, I2C; supports `oled_print/oled_clear`|
| TFT ILI9341    | `-DBOARD_HAS_TFT_ILI9341` | `Adafruit ILI9341` + GFX     | SPI; supports `tft_print`                    |
| TFT ST7789     | `-DBOARD_HAS_TFT_ST7789`  | `Adafruit ST7789` + GFX      | SPI; supports `tft_print`                    |
| PSRAM          | `-DBOARD_HAS_PSRAM`       | built-in `esp_psram.h`       | ESP32 WROVER only; auto-enables extra RAM     |

The Python GUI auto-detects these keywords in your `[CONTROL].md` and injects the correct build flags when compiling.

### Per-Board Hardware Limits

| Resource   | ESP32 / S3 | ESP32-C3 | Pico W |
|------------|-----------|----------|--------|
| GPIO pins  | 32 max    | 32 max   | 30 max |
| UART ports | 2 (1+2)   | 1 (1)    | 2 (1+2)|
| ADC pins   | 8 max     | 4 (0-4)  | 4 (GP26-GP29) |
| I2C buses  | 4 max     | 4 max    | 4 max  |
| SPI buses  | 2 max     | 2 max    | 2 max  |
| Servos     | 8 max     | 8 max    | 8 max  |
| PWM outputs| 8 max     | 8 max    | 8 max  |

---

## 1. Architecture:

```
┌─────────────────────────────────────────────────────────┐
│                    femtoclaw.py (GUI)                   │
│                                                         │
│  [Flash] [Terminal] [LLM/WiFi] [Channels] [Control]     │
│                                              ↑          │
│                         Markdown editor + push button   │
└────────────────────────────┬────────────────────────────┘
                             │  UART: `board push <md_b64>`
                             ▼
┌─────────────────────────────────────────────────────────┐
│                femtoclaw_mcu.cpp (MCU)                  │
│                                                         │
│  1. Stores [CONTROL].md in LittleFS / NVS               │
│  2. At init: parses md → configures GPIO/UART pins/     |
|                                     I2C/SPI/Servo/PWM   │
│  3. At LLM init: injects board config into system prompt│
│  4. Parses [ACTION:...] tags from LLM responses         │
│  5. Executes hardware actions, feeds results back to LLM│
└─────────────────────────────────────────────────────────┘
                             │
                     ↓ WiFi HTTPS ↓
                    ┌────────────┐
                    │  LLM API   │
                    │(knows the  │
                    │ hardware   │
                    │  layout)   │
                    └────────────┘
```

---

## 2. The `.md` File Formats

The user creates a `[CONTROL].md` file and pushes it to the board from the GUI.
This file is the single source of truth for hardware configuration.

### Full Example

```markdown
# My Smart Home Controller
Board: ESP32-C3 Super Mini
Purpose: Controls lighting, reads sensors, talks to GPS module

## GPIO Pins

| Pin | Mode         | Name         | Logic    | Description                        |
|-----|--------------|--------------|----------|------------------------------------|
| 2   | OUTPUT       | led_builtin  |          | Built-in LED, active HIGH          |
| 5   | OUTPUT       | relay_lamp   | inverted | Relay for main lamp, HIGH=ON       |
| 6   | OUTPUT       | relay_fan    | inverted | Relay for ceiling fan, HIGH=ON     |
| 4   | INPUT        | pir_motion   |          | PIR sensor, HIGH=motion detected   |
| 3   | INPUT_PULLUP | btn_reset    |          | Push button, LOW=pressed           |
| 7   | INPUT        | door_sensor  |          | Reed switch, HIGH=open             |

## Serial Ports

| Port  | Baud  | RX Pin | TX Pin | Name       | Description              |
|-------|-------|--------|--------|------------|--------------------------|
| UART1 | 9600  | 16     | 17     | gps        | NEO-6M GPS module        |
| UART2 | 115200| 18     | 19     | aux_mcu    | Secondary MCU for display|

## ADC Pins

| Pin | Name        | Description                      |
|-----|-------------|----------------------------------|
| 0   | ldr         | Light sensor, 0=dark 4095=bright |
| 1   | temp_sensor | NTC thermistor voltage divider   |

## I2C Buses

| Bus  | SDA | SCL | Address | Name    | Description               |
|------|-----|-----|---------|---------|---------------------------|
| I2C0 | 21  | 22  | 0x3C    | oled    | SSD1306 128×64 OLED       |
| I2C0 | 21  | 22  | 0x76    | bme280  | BME280 temp/humidity/press|

## SPI Buses

| Bus  | MOSI | MISO | SCK | CS | Name   | Description         |
|------|------|------|-----|----|--------|---------------------|
| SPI0 | 23   | 19   | 18  | 5  | screen | ILI9341 TFT display |

## Servos

| Pin | Name    | Min | Max | Step | Delay | Description            |
|-----|---------|-----|-----|------|-------|------------------------|
| 13  | pan     | 0   | 180 | 1    | 15    | Pan servo (smooth)     |
| 14  | tilt    | 30  | 150 | 2    | 20    | Tilt servo (clamped)   |

## PWM Outputs

| Pin | Name   | Freq  | Resolution | Description          |
|-----|--------|-------|------------|----------------------|
| 25  | pump   | 1000  | 8          | Water pump speed     |
| 26  | fan    | 25000 | 8          | Cooling fan speed    |

## Workflows

- When pir_motion goes HIGH, turn on led_builtin and relay_lamp for 30 seconds
- If btn_reset is pressed (LOW), turn off all outputs
- If door_sensor is HIGH (open), notify via Telegram or Discord
- Read gps every 60 seconds and store last known location

## Safety Rules

- Never turn on relay_lamp and relay_fan at the same time
- relay_lamp maximum ON duration: 2 hours
- Always turn all outputs OFF on reboot
```

### Section Table Formats

#### `## GPIO Pins`

| Column      | Required   | Values                                              | Notes                                                                                     |
|-------------|------------|-----------------------------------------------------|-------------------------------------------------------------------------------------------|
| Pin         | ✅          | GPIO number                                         |                                                                                           |
| Mode        | ✅          | `OUTPUT`, `INPUT`, `INPUT_PULLUP`, `INPUT_PULLDOWN` | `INPUT_PULLDOWN` is Pico W only; maps to `INPUT` on ESP32 with a warning                  |
| Name        | ✅          | alphanumeric identifier                             | Used by AI in `[ACTION:gpio_set pin=<name>]`                                              |
| Logic       |            | `inverted` or blank                                 | `inverted` = active-LOW; AI always uses logical 1/0, firmware inverts the physical signal |
| Description |            | free text                                           | Injected into AI context verbatim                                                         |

#### `## Serial Ports`

| Column      | Required | Values                  | Notes                                                    |
|-------------|----------|-------------------------|----------------------------------------------------------|
| Port        | ✅        | `UART1`, `UART2`        | ESP32-C3: only UART1; ESP32/S3: UART1+2; Pico W: UART1+2 |
| Baud        | ✅        | integer (e.g. `9600`)   | Standard baud rate                                       |
| RX Pin      | ✅        | GPIO number             |                                                          |
| TX Pin      | ✅        | GPIO number             |                                                          |
| Name        | ✅        | alphanumeric identifier | Used in `[ACTION:serial_read port=<name>]`               |
| Description |          | free text               |                                                          |

#### `## ADC Pins`

| Column      | Required | Values                  | Notes                                      |
|-------------|----------|-------------------------|--------------------------------------------|
| Pin         | ✅        | GPIO number             | ESP32-C3: 0–4 only; Pico W: GP26–GP29 only |
| Name        | ✅        | alphanumeric identifier | Used in `[ACTION:adc_read pin=<name>]`     |
| Description |          | free text               | 0–4095 range (12-bit)                      |

#### `## I2C Buses`

| Column      | Required | Values                       | Notes                                                             |
|-------------|----------|------------------------------|-------------------------------------------------------------------|
| Bus         | ✅        | `I2C0`, `I2C1`, `0`, `1`     | 0 = Wire, 1 = Wire1                                               |
| SDA         | ✅        | GPIO number                  |                                                                   |
| SCL         | ✅        | GPIO number                  |                                                                   |
| Address     | ✅        | hex (e.g. `0x3C`) or decimal | 7-bit I2C device address                                          |
| Name        | ✅        | alphanumeric identifier      | Used in `[ACTION:oled_print bus=<name>]`, `i2c_write`, `i2c_read` |
| Description |          | free text                    |                                                                   |

Multiple devices can share the same bus (same SDA/SCL). `Wire.begin()` is called once per bus using the first entry's pins.

#### `## SPI Buses`

| Column      | Required | Values                   | Notes                                   |
|-------------|----------|--------------------------|-----------------------------------------|
| Bus         | ✅        | `SPI0`, `SPI1`, `0`, `1` |                                         |
| MOSI        | ✅        | GPIO number              |                                         |
| MISO        |          | GPIO number              | Optional for write-only devices         |
| SCK         | ✅        | GPIO number              |                                         |
| CS          | ✅        | GPIO number              | Chip select                             |
| Name        | ✅        | alphanumeric identifier  | Used in `[ACTION:tft_print bus=<name>]` |
| Description |          | free text                |                                         |

#### `## Servos`

| Column      | Required | Values                  | Notes                                                        |
|-------------|----------|-------------------------|--------------------------------------------------------------|
| Pin         | ✅        | GPIO number             | Must be a PWM-capable pin                                    |
| Name        | ✅        | alphanumeric identifier | Used in `[ACTION:servo_set name=<name> angle=<n>]`           |
| Min         |          | angle (default `0`)     | Minimum clamped angle in degrees                             |
| Max         |          | angle (default `180`)   | Maximum clamped angle in degrees                             |
| Step        |          | degrees (default `1`)   | Degrees per step for smooth motion; `1` = instant (no sweep) |
| Delay       |          | ms (default `20`)       | Delay between each step in smooth motion mode                |
| Description |          | free text               |                                                              |

**Smooth motion:** When `Step > 1`, `servo_set` sweeps from the current angle to the target angle in increments of `Step` degrees, 
pausing `Delay` ms between each step. Set `Step = 1` (or leave blank) for instant movement.

#### `## PWM Outputs`

| Column      | Required | Values                  | Notes                                               |
|-------------|----------|-------------------------|-----------------------------------------------------|
| Pin         | ✅        | GPIO number             | Must be PWM-capable                                 |
| Name        | ✅        | alphanumeric identifier | Used in `[ACTION:pwm_set name=<name> duty=<0-255>]` |
| Freq        |          | Hz (default `1000`)     | PWM frequency                                       |
| Resolution  |          | bits (default `8`)      | Duty cycle resolution; 8 = range 0–255              |
| Description |          | free text               |                                                     |

### Key Design Principles

- **Human-readable first**: Users write plain markdown —> no JSON, no code.
- **AI-readable second**: The markdown is injected into the LLM system prompt verbatim, so the AI understands every pin by name.
- **Firmware-parseable third**: The firmware parses the tables to initialize hardware (GPIO modes, UART ports, I2C Buses, SPI Buses, ADC channels, Servos, PWM).

---

## 3. UART Commands

```
board show                   — print stored board config
board reset                  — clear config, set all outputs LOW
gpio get <pin>               — read GPIO (0 or 1)
gpio set <pin> <0|1>         — set GPIO output
gpio mode <pin> <mode>       — change pin mode
adc read <pin>               — read ADC (0-4095)
serial write <n> <data>      — write to named serial port
serial read <n>              — read from named serial port
servo set <n> <angle>        — set servo angle
pwm set <n> <duty>           — set PWM duty (0-255)
```

### 3.1 AI System Prompt Injection

At LLM initialization (when the first message is sent), the board config is appended to the system prompt:

```cpp
static const char k_sys_prompt[] =
    "You are FemtoClaw, an AI assistant running on a microcontroller.\n"
    "You can hold normal conversations AND control real hardware.\n\n"

    "## Conversation Behaviour\n"
    "  • Respond naturally to greetings, questions, and general topics.\n"
    "  • On the very first message (hi / hello / start / hey), greet the user warmly,\n"
    "    introduce yourself briefly, and only if no board config is loaded gently\n"
    "    mention: 'If you'd like me to control hardware, please upload your board .md file.'\n"
    "  • Do NOT mention hardware, actions, or the board config unless the user brings it up\n"
    "    or a board config is already loaded.\n"
    "  • Answer general knowledge questions, help with reasoning, writing, math, etc.\n\n"

    "## Hardware Control\n"
    "When the user asks to control hardware you MUST ALWAYS embed the appropriate\n"
    "[ACTION:...] tag in your reply no exceptions, even if you think the hardware\n"
    "is already in the requested state. Skipping the action tag is never allowed.\n"
    "For gpio_set: value=1 means ON/HIGH, value=0 means OFF/LOW (logical values).\n"
    "The firmware handles any hardware-level inversion, you always use logical values.\n"
    "Never emit action tags during normal conversation.\n\n"

    "Available actions:\n"
    "  [ACTION:gpio_set     pin=<n>   value=<0|1>]\n"
    "  [ACTION:gpio_get     pin=<n>]\n"
    "  [ACTION:adc_read     pin=<n>]\n"
    "  [ACTION:serial_write port=<n>  data=<msg>]\n"
    "  [ACTION:serial_read  port=<n>]\n"
    "  [ACTION:delay_ms     ms=<n>]\n"
    "  [ACTION:servo_set    name=<n>  angle=<0-180>]\n"
    "  [ACTION:pwm_set      name=<n>  duty=<0-255>]\n"
    "  [ACTION:oled_print   bus=<n>   text=<msg> x=<n> y=<n>]\n"
    "  [ACTION:oled_clear   bus=<n>]\n"
    "  [ACTION:tft_print    bus=<n>   text=<msg> x=<n> y=<n> color=<hex>]\n"
    "  [ACTION:i2c_write    bus=<n>   reg=<hex>  data=<hex>]\n"
    "  [ACTION:i2c_read     bus=<n>   reg=<hex>  len=<n>]\n\n"

    "Action results come back as [RESULT:...] in the conversation.\n\n"

    "## Action Rules (only apply when executing hardware tasks)\n"
    "  • Always refer to pins and buses by NAME from the board config below.\n"
    "  • Never guess a pin name not listed in the board config.\n"
    "  • If the user requests a hardware action but no board config is loaded,\n"
    "    reply: 'I need your board config to do that please upload your .md file.'\n"
    "  • Clamp servo angles to the declared Min–Max range.\n"
    "  • PWM duty: 0 = off, 255 = full power.\n\n"

    "## Board Configuration\n";
```

### 3.2 Action Reference

| Action         | Parameters                                        | Notes                                                                                                           |
|----------------|---------------------------------------------------|-----------------------------------------------------------------------------------------------------------------|
| `gpio_set`     | `pin=<name\|num>  value=<0\|1>`                   | Silently rejected if pin is not declared OUTPUT                                                                 |
| `gpio_get`     | `pin=<name\|num>`                                 | Returns `[RESULT:gpio_get pin=N value=V]`; respects `inverted`                                                  |
| `adc_read`     | `pin=<name\|num>`                                 | Only works for pins declared in `## ADC Pins`; returns 0–4095                                                   |
| `serial_write` | `port=<name>  data=<text>`                        | Only declared `## Serial Ports`; data capped at ~96 bytes                                                       |
| `serial_read`  | `port=<name>`                                     | Hard timeout: **150 ms** (USB-CDC keepalive safety)                                                             |
| `delay_ms`     | `ms=<n>`                                          | Hard cap: **5000 ms**; USB-CDC keepalive null-byte every 200 ms                                                 |
| `servo_set`    | `name=<name>  angle=<0-180>`                      | Clamped to declared Min–Max; sweeps smoothly if Step > 1                                                        |
| `pwm_set`      | `name=<name>  duty=<0-255>`                       | Clamped to 0–255; ESP32 uses LEDC, Pico W uses analogWrite                                                      |
| `oled_print`   | `bus=<name>  text=<msg>  x=<n>  y=<n>`            | Requires `-DBOARD_HAS_OLED_SSD1306`; appends to current display                                                 |
| `oled_clear`   | `bus=<name>`                                      | Requires `-DBOARD_HAS_OLED_SSD1306`; clears display                                                             |
| `tft_print`    | `bus=<name>  text=<msg>  x=<n>  y=<n>  color=<c>` | Requires ILI9341 or ST7789 build flag; `color` = `white`, `red`, `green`, `blue`, `black`, or hex like `0xF800` |
| `i2c_write`    | `bus=<name>  reg=<hex>  data=<hex>`               | Writes one register byte; `reg` and `data` are hex strings like `0x3C`                                          |
| `i2c_read`     | `bus=<name>  reg=<hex>  len=<n>`                  | Reads `len` bytes (1–16) from register; returns hex string                                                      |

### 3.3 Action Result Feedback Loop

For actions that read values (`gpio_get`, `adc_read`, `serial_read`, `i2c_read`), the result is appended as a follow-up user message in the conversation context, so the AI can incorporate it into its next response:

```
User: "What is the light level right now?"
AI:   "Let me check... [ACTION:adc_read pin=ldr]"
MCU:  Executes read → appends "[RESULT:adc_read pin=0 value=312]" to context
AI:   (next token) "The light level is 312 out of 4095, which means it's fairly dim."
```

---

## 4. Push Protocol

The GUI sends the markdown file to the board via the existing UART connection:

```python
def push_board_config(self, md_content: str):
    import base64
    # Encode as base64 to safely pass through UART (avoids newline/pipe issues)
    b64 = base64.b64encode(md_content.encode()).decode()
    # Send in chunks of 200 chars (MCU buffer friendly)
    self.send_command("board push begin")
    for i in range(0, len(b64), 200):
        chunk = b64[i:i+200]
        self.send_command(f"board push chunk {chunk}")
        time.sleep(0.05)   # small delay for MCU processing
    self.send_command("board push end")
```

The MCU assembles the chunks, decodes base64, parses the markdown, initializes hardware,
and stores in NVS/LittleFS.

---

## 5. `.md` File Examples for Common Use Cases

### 5a. Basic LED & Button Board

```markdown
# Dev Board — LED Control
Board: ESP32-C3 Super Mini

## GPIO Pins

| Pin | Mode         | Name    | Logic | Description              |
|-----|--------------|---------|-------|--------------------------|
| 2   | OUTPUT       | led     |       | Onboard LED, HIGH=on     |
| 9   | INPUT_PULLUP | button  |       | Boot button, LOW=pressed |
```

**User says**: *"Blink the LED 3 times"*
**AI does**: Sends `[ACTION:gpio_set pin=led value=1]`, `[ACTION:delay_ms ms=500]`, `[ACTION:gpio_set pin=led value=0]` × 3

---

### 5b. Smart Relay Controller

```markdown
# Relay Controller
Board: ESP32

## GPIO Pins

| Pin | Mode   | Name    | Logic    | Description                  |
|-----|--------|---------|----------|------------------------------|
| 5   | OUTPUT | relay1  | inverted | Load 1 — desk lamp           |
| 18  | OUTPUT | relay2  | inverted | Load 2 — air purifier        |
| 19  | OUTPUT | relay3  | inverted | Load 3 — spare outlet        |
| 4   | INPUT  | pir     |          | PIR motion sensor            |

## Safety Rules

- Never energize more than 2 relays simultaneously
- All relays must be OFF at startup
```

**User says**: *"Turn on my desk lamp"*
**AI does**: `[ACTION:gpio_set pin=relay1 value=1]`

**User says**: *"Is anyone in the room?"*
**AI does**: `[ACTION:gpio_get pin=pir]` → returns 1 → *"Yes, motion is detected."*

---

### 5c. GPS Tracker

```markdown
# GPS Logger
Board: ESP32

## GPIO Pins

| Pin | Mode   | Name       | Logic | Description     |
|-----|--------|------------|-------|-----------------|
| 2   | OUTPUT | status_led |       | GPS fix LED     |

## Serial Ports

| Port  | Baud | RX Pin | TX Pin | Name | Description     |
|-------|------|--------|--------|------|-----------------|
| UART1 | 9600 | 16     | 17     | gps  | NEO-6M GPS unit |

## Workflows

- On startup, begin reading GPS NMEA sentences
- Flash status_led when a valid fix is obtained
```

**User says**: *"Where am I?"*
**AI does**: `[ACTION:serial_read port=gps]` → parses NMEA → *"You are at 23.5°N, 90.2°E (Dhaka, Bangladesh)."*

---

### 5d. Sensor Dashboard

```markdown
# Sensor Node

## GPIO Pins

| Pin | Mode   | Name      | Logic | Description           |
|-----|--------|-----------|-------|-----------------------|
| 2   | OUTPUT | led       |       | Status LED            |

## ADC Pins

| Pin | Name   | Description                    |
|-----|--------|--------------------------------|
| 0   | ldr    | Light sensor (0=dark,4095=day) |
| 1   | soil   | Soil moisture (0=wet,4095=dry) |
| 2   | temp   | NTC thermistor                 |
```

**User says**: *"Should I water my plant?"*
**AI does**: `[ACTION:adc_read pin=soil]` → gets 3800 → *"The soil moisture is 3800/4095, which is very dry. You should water your plant."*

---

### 5e. OLED Display Node

```markdown
# Display Controller
Board: ESP32

## GPIO Pins

| Pin | Mode   | Name | Logic | Description |
|-----|--------|------|-------|-------------|
| 2   | OUTPUT | led  |       | Status LED  |

## I2C Buses

| Bus  | SDA | SCL | Address | Name  | Description          |
|------|-----|-----|---------|-------|----------------------|
| I2C0 | 21  | 22  | 0x3C    | oled0 | SSD1306 128×64 OLED  |
```

*Requires build flag: `-DBOARD_HAS_OLED_SSD1306`*

**User says**: *"Show 'Hello World' on the screen"*
**AI does**: `[ACTION:oled_clear bus=oled0]` → `[ACTION:oled_print bus=oled0 text=Hello World x=0 y=0]`

---

### 5f. Pan-Tilt Camera Mount

```markdown
# Camera Mount
Board: ESP32

## Servos

| Pin | Name | Min | Max | Step | Delay | Description              |
|-----|------|-----|-----|------|-------|--------------------------|
| 13  | pan  | 0   | 180 | 2    | 15    | Horizontal pan, smooth   |
| 14  | tilt | 30  | 150 | 2    | 20    | Vertical tilt, clamped   |
```

*Requires build flag: `-DBOARD_HAS_SERVO`*

**User says**: *"Center the camera"*
**AI does**: `[ACTION:servo_set name=pan angle=90]` + `[ACTION:servo_set name=tilt angle=90]`

---

## 6. Safety & Constraints

These constraints are enforced in firmware, not just by the AI:

| Constraint                                                | Enforcement                                     |
|-----------------------------------------------------------|-------------------------------------------------|
| Only declared OUTPUT pins can be written                  | Firmware validates mode at parse time           |
| Input pins cannot be written                              | `gpio_set` silently ignores INPUT-mode pins     |
| `delay_ms` capped at 5000 ms                              | Hard-coded cap prevents indefinite blocking     |
| `serial_read` capped at 150 ms timeout                    | Prevents USB-CDC keepalive starvation           |
| Serial writes are null-terminated + length-capped         | Prevents buffer overruns                        |
| Board config push requires a full valid markdown parse    | Partial/corrupt configs are rejected with error |
| ADC pins must be declared in `## ADC Pins` to be readable | Prevents arbitrary analog reads                 |
| Servo angles are clamped to declared Min–Max range        | Firmware enforces; AI prompt also instructs it  |
| PWM duty clamped to 0–255                                 | Firmware enforces                               |
| ESP32 LEDC: max 16 channels total (servos + PWM)          | Warning printed if exceeded; extra PWM skipped  |

---

## 7. Interaction Example: Full End-to-End

```
# User pushes relay_controller.md from GUI

femtoclaw> [Board Config pushed — 3 OUTPUT pins, 1 INPUT pin initialized]
femtoclaw> [AI initialized with hardware context]

# Telegram message from user:
User (Telegram): "Turn on the desk lamp"

Board → LLM: (system: "...relay1 = GPIO5, OUTPUT, inverted, desk lamp...")
             (user: "Turn on the desk lamp")

LLM → Board: "Sure! Turning on the desk lamp now. [ACTION:gpio_set pin=relay1 value=1]"

Board: executes digitalWrite(5, LOW)  ← inverted: logical 1 = physical LOW for active-LOW relay
Board → Telegram: "Sure! Turning on the desk lamp now."  (action tag stripped)

# 10 minutes later
User (Telegram): "Is the lamp still on?"

LLM → Board: "Let me check... [ACTION:gpio_get pin=relay1]"
Board: reads GPIO5 → LOW, inverted → logical 1
Board appends to context: "[RESULT:gpio_get pin=5 value=1]"
LLM continues: "Yes, the desk lamp is currently ON."
Board → Telegram: "Yes, the desk lamp is currently ON."
```