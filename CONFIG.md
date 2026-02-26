# 📝 FemtoClaw Config System

## Config Storage Architecture

### Two Storage Backends

| Platform | Storage | Location | Format |
|---|---|---|---|
| **ESP32** | NVS (key-value) | Flash memory | Individual keys |
| **Pico W** | LittleFS | `/femtoclaw.json` | Single JSON file |

Both behave the **same way from the user's perspective** partial updates.

---

## How Config Push Works (Step-by-Step)

### From GUI → Board

When you click **"📤 Send to Board"** in the GUI:

```python
# GUI sends individual UART commands (lines 1200-1210 in femtoclaw_qt.py)
def _push_cfg(self):
    cmds = []
    if self._wifi_ssid.text():
        cmds.append(f"set wifi_ssid {self._wifi_ssid.text()}")
    if self._wifi_pass.text():
        cmds.append(f"set wifi_pass {self._wifi_pass.text()}")
    if self._llm_key.text():
        cmds.append(f"set llm_api_key {self._llm_key.text()}")
    # ... etc
    
    self._send_cmds(cmds)  # Sends over serial
```

**Key point:** Only sends fields that have values and empty fields are skipped.

---

### Board Receives Commands

Each `set key value` command goes through the UART shell → `tool_dispatch("set_config", ...)`:

```cpp
// Lines 476-489 in femtoclaw_mcu.cpp
else if (!strcmp(name,"set_config")) {
    char key[48]={0}, val[CFG_S]={0};
    const char *kp=jfind(args,"key"), *vp=jfind(args,"value");
    if (kp) jstr(kp,key,48);
    if (vp) jstr(vp,val,CFG_S);
    
    // IMPORTANT: Only updates ONE field at a time
    if      (!strcmp(key,"llm_model"))      strlcpy(g_cfg.llm_model,val,64);
    else if (!strcmp(key,"llm_api_key"))    strlcpy(g_cfg.llm_api_key,val,CFG_S);
    else if (!strcmp(key,"wifi_ssid"))      strlcpy(g_cfg.wifi_ssid,val,CFG_S);
    // ...
    
    cfg_save();  // Saves ENTIRE config to storage
}
```

**Key behavior:**
1. Only the specified key is updated in RAM (`g_cfg`)
2. Then `cfg_save()` writes the **entire** `g_cfg` struct to storage
3. Other fields remain untouched

---

## Example Scenario

### Initial State (on board)
```json
{
  "wifi_ssid": "MyHomeWiFi",
  "wifi_pass": "secret123",
  "llm_provider": "openrouter",
  "llm_api_key": "sk-old-key",
  "llm_model": "gpt-4o-mini"
}
```

### You Push from GUI
```python
# Only send these two:
set llm_api_key sk-new-key
set llm_model deepseek-chat
```

### Result on Board
```json
{
  "wifi_ssid": "MyHomeWiFi",      // ✅ UNCHANGED
  "wifi_pass": "secret123",        // ✅ UNCHANGED
  "llm_provider": "openrouter",    // ✅ UNCHANGED
  "llm_api_key": "sk-new-key",     // ⬅️ UPDATED
  "llm_model": "deepseek-chat"     // ⬅️ UPDATED
}
```

---

## How cfg_save() Works

### ESP32 (NVS Key-Value Store)

```cpp
// Lines 208-226 in femtoclaw_mcu.cpp
static void cfg_save() {
  prefs.begin("femtoclaw", false);
  prefs.putString("wifi_ssid",    g_cfg.wifi_ssid);      // Overwrites ONLY this key
  prefs.putString("wifi_pass",    g_cfg.wifi_pass);      // Overwrites ONLY this key
  prefs.putString("llm_api_key",  g_cfg.llm_api_key);    // Overwrites ONLY this key
  prefs.putString("llm_model",    g_cfg.llm_model);      // Overwrites ONLY this key
  // ... all other keys
  prefs.end();
}
```

**NVS behavior:**
- Each key is stored separately in flash
- `putString("wifi_ssid", value)` **only updates that one key**
- Other keys remain unchanged

### Pico W (LittleFS JSON File)

```cpp
// Lines 252-266 in femtoclaw_mcu.cpp
static void cfg_save() {
  static char buf[1536];
  snprintf(buf, sizeof(buf),
    "{\"wifi_ssid\":\"%s\",\"wifi_pass\":\"%s\","
    "\"llm_api_key\":\"%s\",\"llm_model\":\"%s\","
    // ... all fields
    "}",
    g_cfg.wifi_ssid, g_cfg.wifi_pass,
    g_cfg.llm_api_key, g_cfg.llm_model);
  
  File f = LittleFS.open(CONFIG_FILE,"w");  // "w" = OVERWRITES entire file
  if (!f) return; 
  f.print(buf); 
  f.close();
}
```

**LittleFS behavior:**
- Writes the **entire JSON file** every time
- But only includes current values from `g_cfg`
- Since `g_cfg` is in RAM and persists, unchanged fields remain

---

## Config Persistence Flow

```
┌─────────────────────────────────────────────────────────────┐
│ 1. Board boots                                              │
│    → cfg_load() reads storage → populates g_cfg            │
└──────────────────┬──────────────────────────────────────────┘
                   │
                   v
┌─────────────────────────────────────────────────────────────┐
│ 2. GUI sends: set wifi_ssid NewNetwork                     │
│    → Updates ONLY g_cfg.wifi_ssid in RAM                   │
│    → Calls cfg_save() → writes ENTIRE g_cfg to storage     │
└──────────────────┬──────────────────────────────────────────┘
                   │
                   v
┌─────────────────────────────────────────────────────────────┐
│ 3. GUI sends: set llm_model gpt-4o                         │
│    → Updates ONLY g_cfg.llm_model in RAM                   │
│    → Calls cfg_save() → writes ENTIRE g_cfg to storage     │
│    → wifi_ssid still "NewNetwork" (preserved)              │
└──────────────────┬──────────────────────────────────────────┘
                   │
                   v
┌─────────────────────────────────────────────────────────────┐
│ 4. Board reboots                                            │
│    → cfg_load() reads storage                               │
│    → Both wifi_ssid AND llm_model have latest values       │
└─────────────────────────────────────────────────────────────┘
```

**Key insight:** The in-memory `g_cfg` struct is the **single source of truth**. Storage is just persistence.

---

## During Reboot

### Scenario: Partial config push, then reboot

**Before reboot:**
```json
{
  "wifi_ssid": "OldWiFi",
  "llm_model": "gpt-4o-mini"
}
```

**You push:**
```
set wifi_ssid NewWiFi
```

**Immediately after (in RAM):**
```json
{
  "wifi_ssid": "NewWiFi",       // ⬅️ Updated
  "llm_model": "gpt-4o-mini"    // ✅ Still there
}
```

**After reboot:**
```json
{
  "wifi_ssid": "NewWiFi",       // ✅ Persisted!
  "llm_model": "gpt-4o-mini"    // ✅ Still there!
}
```

**Conclusion:** Reboots are safe — all config persists.

---

## Edge Cases

### Case 1: Push empty string

```
set wifi_ssid ""
```

**Result:** `wifi_ssid` becomes an empty string (not deleted). The key still exists.

### Case 2: Push same value twice

```
set llm_model gpt-4o
set llm_model gpt-4o  # Again
```

**Result:** Second command is redundant but harmless. Storage is written twice with the same value.

### Case 3: Channel config (Telegram/Discord)

```
tg token 123456:ABC...
tg enable
```

**Result:**
- `telegram.token` is updated
- `telegram.enabled` is set to `true`
- All other config (WiFi, LLM) remains unchanged

---

## Comparison: GUI vs Direct UART

| Method | Behavior |
|---|---|
| **GUI "Send to Board"** | Sends `set key value` for each field → Partial update |
| **UART `set key value`** | Directly calls `tool_dispatch` → Partial update |
| **UART `show config`** | Reads from `g_cfg` → Shows current merged state |

*NOTE:* Both methods do the **same thing**.

---

## Best Practices

### ✅ DO: Update only what you need
```
set llm_api_key sk-new-key     # Only updates API key
```

### ✅ DO: Push multiple keys in sequence
```
set wifi_ssid NewNetwork
set wifi_pass NewPassword
set llm_model deepseek-chat
```
Each command updates its key, then saves. Final result has all three changes.

### ✅ DO: Use `show config` to verify
```
show config
```
Shows the current merged state of all keys.

### ❌ DON'T: Assume you need to re-send unchanged keys
```
# Bad (unnecessary):
set wifi_ssid OldNetwork       # Already set
set llm_model gpt-4o-mini      # Already set
set llm_api_key sk-new-key     # Only this needed

# Good:
set llm_api_key sk-new-key     # Just send what changed
```

---

## Summary Table

| Aspect | Behavior |
|---|---|
| **Update type** | Partial (merge) |
| **Unsent keys** | Remain unchanged |
| **Storage write** | After each `set` command |
| **Persistence** | Immediate (survives reboot) |
| **ESP32 vs Pico W** | Same behavior |
| **Empty values** | Set to empty string (not deleted) |
| **Duplicate sends** | Harmless redundancy |

---


## Testing It Yourself

### Step 1: Check initial config
```
femtoclaw> show config
wifi_ssid: OldWiFi
llm_model: gpt-4o-mini
```

### Step 2: Update one field
```
femtoclaw> set llm_model deepseek-chat
set llm_model ok
```

### Step 3: Verify merge
```
femtoclaw> show config
wifi_ssid: OldWiFi           ← Still there!
llm_model: deepseek-chat     ← Updated!
```

### Step 4: Reboot and verify persistence
```
femtoclaw> reboot
[... board reboots ...]
femtoclaw> show config
wifi_ssid: OldWiFi           ← Survived reboot!
llm_model: deepseek-chat     ← Survived reboot!
```

