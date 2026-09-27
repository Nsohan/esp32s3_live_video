# 🤖 PetBot (Jarvis) — System Status, Pin Map & Roadmap

This document serves as the single source of truth for the current hardware bring-up, active GPIO pin configurations in firmware, implemented subsystems, and the roadmap for remaining features.

---

## 📊 1. Implementation Status Overview

### ✅ Implemented & Working in Firmware
- **Display Driver (ILI9341 2.4" SPI TFT, 320×240)**:
  - Hardware SPI with DMA chunk buffering (6 dedicated bands, 25.6 KB each) in [`main/drivers/display/`](file:///e:/PetBot/PetBot/pet-bot/loona_petbot/main/drivers/display/).
  - Thread-safe rendering with FreeRTOS mutexes.
- **Touch Controller Driver (XPT2046)**:
  - Calibrated 12-bit resistive touch panel sampling, interrupt reading, and inversion correction in [`main/drivers/touch/`](file:///e:/PetBot/PetBot/pet-bot/loona_petbot/main/drivers/touch/).
- **GFX UI Engine & Vector Icons**:
  - Shapes, anti-aliased geometry, fonts, rounded buttons, and smartphone-like widgets in [`main/drivers/gfx/`](file:///e:/PetBot/PetBot/pet-bot/loona_petbot/main/drivers/gfx/).
- **OV2640 DVP Camera Driver**:
  - DVP interface streaming QVGA (320×240) JPEG frames with auto-exposure, auto-gain, and double framebuffers in 8MB Octal PSRAM in [`main/drivers/camera/`](file:///e:/PetBot/PetBot/pet-bot/loona_petbot/main/drivers/camera/).
- **RoboEyes Animation System**:
  - Loona/Emo-style glowing procedural eye expressions (Happy, Blink, Laugh, Angry, Tired, Curious, Cyclops, Winking) running on Core 1 ([`components/RoboEyes`](file:///e:/PetBot/PetBot/pet-bot/loona_petbot/components/RoboEyes)).
- **MicroSD Card Storage Driver (Display SPI Header)**:
  - Mounted at `/sdcard` via FATFS over `SPI2_HOST` with `SD_CS = GPIO 14` in [`main/drivers/storage/`](file:///e:/PetBot/PetBot/pet-bot/loona_petbot/main/drivers/storage/).
- **MAX98357A I2S Audio Driver & Synth Generator**:
  - High-performance 16-bit DMA I2S audio streaming on BCLK: 0, WS: 48, DOUT: 21 with software volume scaling (0-100%) and clean chirp generator in [`main/drivers/audio/`](file:///e:/PetBot/PetBot/pet-bot/loona_petbot/main/drivers/audio/).
- **Streaming MP3 / WAV Audio Player Service**:
  - Background FreeRTOS task with `minimp3` chunked stream decoder for smooth playback from `/sdcard/music/` and `/sdcard/sounds/` in [`main/services/audio_player/`](file:///e:/PetBot/PetBot/pet-bot/loona_petbot/main/services/audio_player/).
- **Interactive Smartphone App Launcher**:
  - 8-tile interactive launcher on Core 0 with touch navigation, status bar, and automatic screensaver timer in [`main/core/app_launcher.c`](file:///e:/PetBot/PetBot/pet-bot/loona_petbot/main/core/app_launcher.c).
- **8 Built-in Touch Apps**:
  1. `Camera Preview`: Live on-screen viewfinder.
  2. `RoboEyes`: Interactive eye expressions test.
  3. `Music Player`: Full SD MP3/WAV playback, track list, play/pause, volume control.
  4. `Settings`: System configurations.
  5. `Moods`: Emotional state selector.
  6. `SysInfo`: Live hardware specs, free heap, PSRAM, CPU freq, uptime, and MAC address.
  7. `Torch`: Full-screen white illumination tool.
  8. `Touch Test`: 24-block touch calibration grid.
- **Wi-Fi & HTTP MJPEG Video Streamer**:
  - Non-blocking Wi-Fi station manager with auto-reconnect and real-time MJPEG camera server in [`main/services/`](file:///e:/PetBot/PetBot/pet-bot/loona_petbot/main/services/).

---

### ⏳ Remaining to Implement (Future Work)
- **Locomotion / Motor Control**:
  - L298N Dual H-Bridge motor driver (skid-steer 4-wheel drive: left pair + right pair).
  - Kinematics, speed ramping, turning routines.
- **Servo & Articulation System**:
  - PCA9685 16-channel I2C PWM driver for 2× MG996R (arms) and 2× SG90 (ears).
  - Choreographed pet movement routines.
- **Sensors & Telemetry**:
  - MPU6050 (GY-521) 6-axis IMU over I2C (fall detection, tilt reactions, lift-up sensing).
  - VL53L8CX / VL53L0X multi-zone ToF distance matrix for obstacle avoidance.
- **Voice AI & Microphone Input**:
  - INMP441 I2S digital microphone capture.
  - Wake-word detection & offline command parsing with Gemini Cloud AI fallback.

---

## 🔌 2. Complete GPIO Pin Audit

> [!IMPORTANT]
> **Active Firmware vs. Early Draft Differences:**
> The code currently running on the ESP32-S3 uses specific pins for Display and Touch that differ from early concept drafts. Always refer to the **Active Code GPIO** column when wiring hardware.

| Subsystem | Signal Name | Active Code GPIO | Early Draft GPIO | Status & Notes |
| :--- | :--- | :---: | :---: | :--- |
| **Camera (OV2640)** | D0 | **GPIO 11** | GPIO 11 | Fixed by FPC Ribbon |
| | D1 | **GPIO 9** | GPIO 9 | Fixed by FPC Ribbon |
| | D2 | **GPIO 8** | GPIO 8 | Fixed by FPC Ribbon |
| | D3 | **GPIO 10** | GPIO 10 | Fixed by FPC Ribbon |
| | D4 | **GPIO 12** | GPIO 12 | Fixed by FPC Ribbon |
| | D5 | **GPIO 18** | GPIO 18 | Fixed by FPC Ribbon |
| | D6 | **GPIO 17** | GPIO 17 | Fixed by FPC Ribbon |
| | D7 | **GPIO 16** | GPIO 16 | Fixed by FPC Ribbon |
| | XCLK | **GPIO 15** | GPIO 15 | Fixed by FPC Ribbon |
| | PCLK | **GPIO 13** | GPIO 13 | Fixed by FPC Ribbon |
| | VSYNC | **GPIO 6** | GPIO 6 | Fixed by FPC Ribbon |
| | HREF | **GPIO 7** | GPIO 7 | Fixed by FPC Ribbon |
| | SCCB SIOD (SDA) | **GPIO 4** | GPIO 4 | Fixed by FPC Ribbon |
| | SCCB SIOC (SCL) | **GPIO 5** | GPIO 5 | Fixed by FPC Ribbon |
| **Octal PSRAM** | Internal Bus | **GPIO 35, 36, 37** | GPIO 35, 36, 37 | **Internally Reserved** (Never use) |
| **Display (ILI9341)** | SCK (Clock) | **GPIO 40** | *GPIO 45* | Defined in `display.h` |
| | MOSI (Data In) | **GPIO 41** | *GPIO 46* | Defined in `display.h` |
| | MISO (Data Out) | **GPIO 39** | *—* | Shared with Touch `T_DO` |
| | CS (Chip Select) | **GPIO 42** | *GPIO 14* | Defined in `display.h` |
| | DC (Data/Command)| **GPIO 2** | *GPIO 47* | Defined in `display.h` |
| | RST (Reset) | **GPIO 1** | *GPIO 21* | Defined in `display.h` |
| **Touch (XPT2046)** | T_CS | **GPIO 38** | *GPIO 0* | Defined in `touch_xpt2046.h` |
| | T_IRQ (Pen IRQ) | **GPIO 3** | *—* | Defined in `touch_xpt2046.h` |
| | T_CLK, T_DIN, T_DO| **40, 41, 39** | Shared | Shared SPI bus with display |
| **SD Card (Display Slot)** | **SD_CS (Chip Select)** | **GPIO 14** | *—* | **Wired & Configured** |
| | SD_CLK, SD_MOSI, SD_MISO | **40, 41, 39** | Shared | Shared SPI bus with display & touch |
| **Audio DAC (MAX98357A)** | **BCLK (Bit Clock)** | **GPIO 47** | *GPIO 40* | **Active in `i2s_audio.h`** (Clean GPIO) |
| | **WS / LRC (Word Select)** | **GPIO 48** | *GPIO 39* | **Active in `i2s_audio.h`** |
| | **DOUT (Data In on Amp)** | **GPIO 21** | *GPIO 41* | **Active in `i2s_audio.h`** |
| **Serial Debug** | UART0 TX / RX | **GPIO 43, 44** | GPIO 43, 44 | USB-to-UART / Flashing console |

---

## 🧭 3. Free GPIOs & Future Wiring Plan

The ESP32-S3 has **5 unassigned GPIOs** remaining for motors and I2C sensors:
- **Available Pins:** `GPIO 19`, `GPIO 20`, `GPIO 45`, `GPIO 46`, `GPIO 47`.

### Recommended Pin Allocation Plan:

```
                  ┌─────────────────────────────────────────┐
                  │          ESP32-S3 (N16R8)               │
                  └────┬──────────────────────┬─────────────┘
                       │                      │              
              I2C Bus (Sensors)          Motors (L298N)     
          (PCA9685 / IMU / ToF)        (4-Wheel Drive)      
                       ▼                      ▼              
                 SDA: GPIO 47           L_IN1: GPIO 19       
                 SCL: (Internal/Ext)    L_IN2: GPIO 20       
                                        R_IN1: GPIO 45       
                                        R_IN2: GPIO 46       
```

1. **Drive Motors (L298N Skid-Steer)**:
   - **Left Pair (IN1, IN2)**: `GPIO 19`, `GPIO 20`
   - **Right Pair (IN1, IN2)**: `GPIO 45`, `GPIO 46`
   - **ENA / ENB**: Fixed 5V jumper on L298N
2. **Shared I2C Bus (PCA9685 Servos + MPU6050 IMU + VL53L8CX ToF)**:
   - **SDA**: `GPIO 47`
   - **SCL**: Shared or remapped pin

---

## 📦 4. Bill of Materials (BOM)

| Component | Status | Qty | Role | Interface / Power |
| :--- | :---: | :---: | :--- | :--- |
| **ESP32-S3-WROOM-1-N16R8** | In Hand | 1 | Main Controller (16MB Flash, 8MB PSRAM) | 5V / 3.3V Logic |
| **OV2640 Camera Module** | In Hand | 1 | Computer Vision & Video Stream | DVP FPC Header |
| **2.4" ILI9341 SPI TFT LCD** | In Hand | 1 | Display / RoboEyes Expressions | SPI (GPIO 40, 41, 39, 42, 2, 1) |
| **XPT2046 Resistive Touch** | In Hand | 1 | Touch Screen UI Input | SPI (GPIO 40, 41, 39, 38, 3) |
| **PCA9685 16-Ch PWM Driver** | In Hand | 1 | Offloads Servo Control | I2C (GPIO 47, 21) |
| **L298N Dual H-Bridge Driver**| In Hand | 1 | 4-Wheel Skid Steer Motor Control | Digital (GPIO 19, 20, 45, 46) |
| **MG996R High-Torque Servos**| In Hand | 2 | Robot Arm Articulation | PWM via PCA9685 |
| **SG90 Micro Servos** | In Hand | 2 | Robot Ear Articulation | PWM via PCA9685 |
| **GY-521 (MPU6050) IMU** | In Hand | 1 | 6-DOF Tilt, Balance & Motion Sense | I2C (GPIO 47, 21) |
| **INMP441 MEMS Microphone** | In Hand | 1 | Voice Input / Wake-Word Detection | I2S (GPIO 14, 0, 48) |
| **MAX98357A Class-D I2S DAC**| In Hand | 1 | Audio & Speech Output Amplifier | I2S |
| **N20 DC Gear Motors (6V)** | To Buy | 4 | Wheel Drive (~100–200 RPM) | 6V Rail via L298N |
| **Robot Wheels & Tires** | To Buy | 4 | Chassis Locomotion | Mechanical |
| **VL53L8CX 8x8 ToF Sensor** | To Buy | 1 | Distance Matrix / Obstacle Detection| I2C (GPIO 47, 21) |
| **LiPo / 18650 Battery + BMS**| To Buy | 1 | Regulated Power Rails (5V / 6V / 3.3V) | Power System |
| **4Ω / 8Ω Speaker** | To Buy | 1 | Sound Output paired with MAX98357A | Analog to MAX98357A |

---

## 🛠️ 5. Quick Build & Run Commands

```powershell
# Build the project
idf.py build

# Flash firmware (change COM port to your device port)
idf.py -p COM7 flash

# View real-time logs & IP address
idf.py -p COM7 monitor
```
