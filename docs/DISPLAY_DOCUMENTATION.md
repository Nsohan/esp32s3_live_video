# 📺 2.4" ILI9341 SPI TFT Display & RoboEyes Documentation

Comprehensive reference documentation for the **ILI9341 2.4" / 2.8" SPI TFT LCD (320x240)** and the **RoboEyes Animation Engine** running on the **ESP32-S3** in native **ESP-IDF**.

---

## 📌 1. Hardware Specifications

| Property | Value |
|---|---|
| **Controller IC** | ILI9341 |
| **Interface** | 4-Wire SPI + D/C + Reset |
| **Native Resolution** | 320 × 240 pixels (Landscape) / 240 × 320 (Portrait) |
| **Color Depth** | 16-bit RGB565 (65,536 colors) |
| **Pixel Element Order** | BGR (`LCD_RGB_ELEMENT_ORDER_BGR`) |
| **Operating Voltage** | 3.3V (VCC & Logic) |
| **SPI Clock Speed** | 20 MHz (stable for breadboards & jumper wires) |
| **Display Bus Host** | `SPI2_HOST` |

---

## 🔌 2. Complete Pinout Mapping

| Display PCB Pin | Function | ESP32-S3 GPIO | Notes |
|---|---|---|---|
| **VCC** | Power | `3.3V` | Connect to stable 3.3V power rail |
| **GND** | Ground | `GND` | Common ground |
| **CS** | Chip Select | `GPIO 42` | Active LOW (`LCD_PIN_CS`) |
| **RESET / RST** | Hardware Reset | `GPIO 1` | Active LOW (`LCD_PIN_RST`) |
| **DC / RS** | Data / Command | `GPIO 2` | High = Data, Low = Command (`LCD_PIN_DC`) |
| **SDI / MOSI** | SPI Data In | `GPIO 41` | Master-Out Slave-In (`LCD_PIN_MOSI`) |
| **SCK / SCL** | SPI Clock | `GPIO 40` | Serial Clock (`LCD_PIN_SCK`) |
| **LED / BL** | Backlight | `3.3V` or `NC` | Connect to 3.3V for full brightness (`LCD_PIN_BCKL = -1`) |
| **SDO / MISO** | SPI Data Out | `GPIO 39` | Shared with Touch `T_DO` & SD `SD_MISO` |
| **T_CS** | Touch Chip Select | `GPIO 38` | Touch controller (`TOUCH_PIN_CS`) |
| **T_IRQ** | Touch Interrupt | `GPIO 3` | Pen down interrupt (`TOUCH_PIN_IRQ`) |
| **SD_CS** | SD Card Chip Select | `GPIO 14` | MicroSD Card slot CS on display PCB |
| **SD_MOSI** | SD Card Data In | `GPIO 41` | Shared with Display `MOSI` |
| **SD_MISO** | SD Card Data Out | `GPIO 39` | Shared with Touch `MISO` |
| **SD_SCK** | SD Card Clock | `GPIO 40` | Shared with Display `SCK` |

---

## 🔄 3. Display Rotation & Orientation Matrix

You can rotate the display in all 4 directions (0°, 90°, 180°, 270°) using the ESP-IDF `esp_lcd` API:

```c
esp_lcd_panel_swap_xy(s_panel_handle, swap_xy);
esp_lcd_panel_mirror(s_panel_handle, mirror_x, mirror_y);
```

### Orientation Reference Table:

| Mode | Rotation | Width × Height | `swap_xy` | `mirror_x` | `mirror_y` | Description |
|---|:---:|:---:|:---:|:---:|:---:|---|
| **Landscape (Default)** | 0° ↷ | **320 × 240** | `true` | `false` | `false` | USB/Header on Right side |
| **Landscape (Inverted)** | 180° ↺ | **320 × 240** | `true` | `true` | `true` | USB/Header on Left side |
| **Portrait (Standard)** | 90° ↷ | **240 × 320** | `false` | `false` | `false` | Header at Bottom |
| **Portrait (Inverted)** | 270° ↶ | **240 × 320** | `false` | `true` | `true` | Header at Top |

---

## 🎨 4. Color Encoding & Polarity

- **Color Format**: 16-bit RGB565 with Big-Endian byte swap (`SWAP_BYTES`).
- **Color Inversion**: `esp_lcd_panel_invert_color(s_panel_handle, true)` is enabled so that `0x0000` is **Deep Pitch Black** and `0xFFFF` is **Pure White**.
- **BGR Order**: `LCD_RGB_ELEMENT_ORDER_BGR` ensures that Blue and Red channels match their true primary color representation.

### Standard Color Constants (RGB565):
```c
#define COLOR_BLACK     0x0000
#define COLOR_WHITE     0xFFFF
#define COLOR_CYAN      0x07FF   // Default Electric Eye Color
#define COLOR_GREEN     0x07E0   // Neon Matrix Green
#define COLOR_YELLOW    0xFFE0   // Amber Gold
#define COLOR_RED       0xF800   // Warning / Angry Red
#define COLOR_BLUE      0x001F   // Deep Sapphire Blue
#define COLOR_MAGENTA   0xF81F   // Neon Pink / Purple
#define COLOR_ORANGE    0xFD20   // Warm Sunset Orange
```

---

## ⚡ 5. DMA Chunk Buffering & Thread Safety

To prevent SPI DMA memory race conditions and chunk tearing:
1. **6 Dedicated DMA Chunk Buffers**: Memory is divided into 6 vertical stripes ($40 \text{ lines} \times 320 \text{ px} = 25.6 \text{ KB}$ per chunk). Each chunk has its own dedicated DMA memory allocated at initialization.
2. **SPI Mutex (`s_display_mutex`)**: Synchronizes display writes across tasks, guaranteeing that background eye rendering and main application display calls never collide.
3. **Core Pinning**: RoboEyes animation runs on **Core 1**, leaving **Core 0** completely free for WiFi, HTTP MJPEG camera streaming, and network processing.

---

## 🤖 6. RoboEyes Animation Engine Guide

The animation system automatically cycles through **10 distinct moods and expressions every 15 seconds**:

```
[0: Default Idle] ➔ [1: Happy Mood] ➔ [2: Laughing] ➔ [3: Angry Mood] ➔ [4: Tired Mood]
       ▲                                                                      │
       │                                                                      ▼
[9: Winking] ◄── [8: Cyclops Mode] ◄── [7: Curious Look] ◄── [6: Sweating] ◄── [5: Confused]
```

### Expression Details:
1. **Default Idle (0–15s)**: Friendly rounded eyes with natural double blinks and smooth wandering gaze.
2. **Happy Mood (15–30s)**: Smiling upward curved bottom eyelids (`HAPPY`).
3. **Laughing Animation (30–45s)**: Rapid vertical vibration (`anim_laugh()`) with joyful smiling eyelids.
4. **Angry Mood (45–60s)**: Inward angled fierce eyelids (`ANGRY`) with a focused locked stare.
5. **Tired Mood (60–75s)**: Heavy droopy eyelids (`TIRED`) with slow, relaxed blinks.
6. **Confused Animation (75–90s)**: Side-to-side rapid shivering motion (`anim_confused()`).
7. **Sweating / Nervous (90–105s)**: Forehead sweat droplets forming and dripping down in real time.
8. **Curious Look (105–120s)**: Smoothly scans 8 cardinal directions (N, NE, E, SE, S, SW, W, NW) with dynamic outer eye enlargement (`setCuriosity(true)`).
9. **Cyclops Mode (120–135s)**: Single large centered robot eye ($100 \times 100 \text{ px}$).
10. **Playful Winking (135–150s)**: Alternating single-eye blinks (`close(true, false)` / `close(false, true)`).

### Eye Dimension Settings:
- **Eye Width**: `72 px` (Left) / `72 px` (Right)
- **Eye Height**: `92 px`
- **Corner Radius**: `20 px`
- **Space Between Eyes**: `30 px`
- **Safety Screen Padding**: `28 px` margin on all sides to prevent edge clipping.

---

## 🛠️ 7. Build, Flash & Monitor Commands

Using the ESP-IDF PowerShell environment:

```powershell
# 1. Build project
idf.py build

# 2. Flash to ESP32-S3 (replace COM7 with your port)
idf.py -p COM7 flash

# 3. View real-time serial logs & stream IP address
idf.py -p COM7 monitor
```
