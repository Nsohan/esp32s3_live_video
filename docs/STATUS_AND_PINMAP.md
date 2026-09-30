# 🤖 PetBot (Jarvis) — System Status, Pin Map & Architecture Reference

This document serves as the single source of truth for hardware bring-up, active GPIO pin configurations in firmware, implemented subsystems, electrical pinouts, and the roadmap for remaining features.

---

## 📊 1. Implementation Status Overview

### ✅ Implemented & Working in Active Firmware

1. **Display Driver (ILI9341 2.4" SPI TFT, 320×240)**:
   - Hardware SPI with DMA chunk buffering (6 dedicated bands, 25.6 KB each) in [`main/drivers/display/`](file:///e:/PetBot/PetBot/loona_petbot/main/drivers/display/).
   - Thread-safe rendering protected by FreeRTOS display mutex (`s_display_mutex`).
   - Clean hardware reset and zero-flicker DMA block streaming (`display_draw_bitmap_block`).

2. **Touch Controller Driver (XPT2046 Resistive Touch)**:
   - Calibrated 12-bit resistive touch panel sampling, pen interrupt detection (`T_IRQ`), and landscape orientation mapping in [`main/drivers/touch/`](file:///e:/PetBot/PetBot/loona_petbot/main/drivers/touch/).
   - Shared high-speed SPI bus (`SPI2_HOST`) with independent chip select (`T_CS = GPIO 38`).

3. **GFX UI Engine & Vector Icon Suite**:
   - Shapes, anti-aliased geometry, fonts, rounded buttons, and smartphone-like widgets in [`main/drivers/gfx/`](file:///e:/PetBot/PetBot/loona_petbot/main/drivers/gfx/).
   - High-performance bitmap blitting directly to DMA buffers.

4. **OV2640 DVP Camera Driver**:
   - DVP interface streaming QVGA (320×240) JPEG frames with auto-exposure, auto-gain, and double framebuffers in 8MB Octal PSRAM in [`main/drivers/camera/`](file:///e:/PetBot/PetBot/loona_petbot/main/drivers/camera/).
   - On-screen live viewfinder and zero-copy HTTP MJPEG streaming pipeline.

5. **RoboEyes Procedural Animation Engine**:
   - Loona/Emo-style glowing procedural eye expressions (Happy, Blink, Laugh, Angry, Tired, Curious, Cyclops, Winking) running on Core 1 ([`components/RoboEyes/`](file:///e:/PetBot/PetBot/loona_petbot/components/RoboEyes/)).
   - Standalone app mode plus automatic idle screensaver with touch wakeup.

6. **MicroSD Card Storage Driver (Display SPI Header)**:
   - Mounted at `/sdcard` via FATFS over shared `SPI2_HOST` with `SD_CS = GPIO 14` in [`main/drivers/storage/`](file:///e:/PetBot/PetBot/loona_petbot/main/drivers/storage/).
   - Scans and indexes directories (`/sdcard/sounds`, `/sdcard/music`, `/sdcard/recordings`).

7. **MAX98357A I2S Class-D DAC Audio Output**:
   - High-performance 16-bit DMA I2S transmitter on `I2S_NUM_0` (BCLK: 47, WS: 48, DOUT: 21) in [`main/drivers/audio/i2s_audio.c`](file:///e:/PetBot/PetBot/loona_petbot/main/drivers/audio/i2s_audio.c).
   - Dynamic sample rate reconfiguration (16 kHz – 48 kHz), stereo-to-mono downmixing, digital volume scaling (0–100%), and procedural UI click/alert tone synthesizer.

8. **INMP441 I2S Digital MEMS Microphone**:
   - Dedicated 16 kHz 16-bit mono recording channel on `I2S_NUM_1` (SCK: 45, WS: 46, SD: 0) in [`main/drivers/audio/i2s_mic.c`](file:///e:/PetBot/PetBot/loona_petbot/main/drivers/audio/i2s_mic.c).
   - Real-time RMS loudness computation and live VU level metering.

9. **Streaming Audio Player Service**:
   - Background FreeRTOS playback task with `minimp3` chunked stream decoder and RIFF/WAVE header parser in [`main/services/audio_player/`](file:///e:/PetBot/PetBot/loona_petbot/main/services/audio_player/).
   - Seamless background playback while navigating touch apps or running RoboEyes.

10. **Interactive Smartphone App Launcher**:
    - 2-page paginated app drawer on Core 0 with touch navigation, smartphone-style status bar (battery %, WiFi, time, audio playing indicator), and automatic screensaver timer in [`main/core/app_launcher.c`](file:///e:/PetBot/PetBot/loona_petbot/main/core/app_launcher.c).

11. **9 Built-in Touchscreen Apps**:
    1. `Camera Preview`: Live on-screen viewfinder streaming directly from the OV2640.
    2. `RoboEyes`: Interactive procedural eye expressions and emotion triggers.
    3. `Music Player`: Full SD MP3/WAV playback, track browser, play/pause, and volume slider.
    4. `Settings`: Screensaver timeout and display brightness configuration.
    5. `Moods`: Emotional state selector and reaction triggers.
    6. `SysInfo`: Hardware metrics: CPU frequency, free internal heap, free 8MB PSRAM, uptime, WiFi SSID, and IP address.
    7. `Recorder`: Digital sound recorder with live 20-segment LED VU meter, saving 16 kHz WAV files to `/sdcard/recordings/` (or RAM fallback) with immediate playback.
    8. `Touch Test`: 24-block interactive touch calibration and accuracy testing grid.
    9. `WebStream`: Live IP status and Web browser MJPEG camera streamer controls.

12. **Wi-Fi & HTTP MJPEG Video Streamer**:
    - Non-blocking Wi-Fi station manager with auto-reconnect, clean disconnect handling (zero display reload/flicker during retries), and HTTP MJPEG camera server in [`main/services/`](file:///e:/PetBot/PetBot/loona_petbot/main/services/).

13. **Battery Monitoring Service**:
    - Battery percentage monitoring and charging state detection in [`main/services/battery/`](file:///e:/PetBot/PetBot/loona_petbot/main/services/battery/).

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
- **Voice AI & Wake-Word Engine**:
  - Wake-word detection (ESP-SR) using existing INMP441 audio feed.
  - Offline command parsing with cloud AI / LLM fallback.

---

## 🔌 2. Complete GPIO Pin Audit

> [!IMPORTANT]
> **Active Firmware Pin Assignments:**
> The table below matches the active, working pin configuration in the firmware code. Always use this table when wiring or testing hardware.

| Subsystem | Signal Name | ESP32-S3 GPIO | Header / Source File | Electrical & Wiring Notes |
| :--- | :--- | :---: | :--- | :--- |
| **Microphone (INMP441)** | **SCK (Bit Clock)** | **GPIO 45** | `i2s_mic.h` | ESP32 Clock Output (`I2S_NUM_1`) |
| | **WS (Word Select)** | **GPIO 46** | `i2s_mic.h` | ESP32 Word Select Output |
| | **SD (Serial Data)** | **GPIO 0** | `i2s_mic.h` | Data from Mic into ESP32 (Pullup internally configured) |
| | **L/R (Channel)** | `GND` | Hardware Pin | Set to GND for Left Channel |
| **Audio DAC (MAX98357A)** | **BCLK (Bit Clock)** | **GPIO 47** | `i2s_audio.h` | ESP32 Clock Output (`I2S_NUM_0`) |
| | **WS / LRC (Word Select)**| **GPIO 48** | `i2s_audio.h` | ESP32 Left/Right Clock Output |
| | **DOUT / DIN (Data In)** | **GPIO 21** | `i2s_audio.h` | ESP32 Audio Data Output to Amp |
| | **GAIN** | `GND` / `Float`| Hardware Pin | Float = 9 dB, GND = 12 dB |
| **Display (ILI9341)** | **SCK (Clock)** | **GPIO 40** | `display.h` | SPI2_HOST Clock (Shared SPI Bus) |
| | **MOSI (Data In)** | **GPIO 41** | `display.h` | SPI2_HOST MOSI (Shared SPI Bus) |
| | **MISO (Data Out)** | **GPIO 39** | `display.h` | SPI2_HOST MISO (Shared SPI Bus) |
| | **CS (Chip Select)** | **GPIO 42** | `display.h` | Dedicated Display Chip Select (Active LOW) |
| | **DC (Data / Command)**| **GPIO 2** | `display.h` | High = Data, Low = Command |
| | **RST (Hardware Reset)**| **GPIO 1** | `display.h` | Active LOW Hardware Reset |
| | **LED / BL (Backlight)** | `3.3V` | `display.h` | Connected to 3.3V (`LCD_PIN_BCKL = -1`) |
| **Touch (XPT2046)** | **T_CS (Chip Select)** | **GPIO 38** | `touch_xpt2046.h` | Dedicated Touch Chip Select (Active LOW) |
| | **T_IRQ (Pen Interrupt)**| **GPIO 3** | `touch_xpt2046.h` | Falling edge on pen press |
| | **T_CLK, T_DIN, T_DO** | **40, 41, 39** | `touch_xpt2046.h` | Shared with Display SPI bus |
| **SD Card (Display Slot)** | **SD_CS (Chip Select)** | **GPIO 14** | `sdcard.h` | Dedicated SD Card Chip Select (Active LOW) |
| | **SD_SCK, MOSI, MISO** | **40, 41, 39** | `sdcard.h` | Shared with Display & Touch SPI bus |
| **Camera (OV2640 DVP)** | **D0** | **GPIO 11** | `camera_driver.h` | Fixed by FPC Ribbon Pinout |
| | **D1** | **GPIO 9** | `camera_driver.h` | Fixed by FPC Ribbon Pinout |
| | **D2** | **GPIO 8** | `camera_driver.h` | Fixed by FPC Ribbon Pinout |
| | **D3** | **GPIO 10** | `camera_driver.h` | Fixed by FPC Ribbon Pinout |
| | **D4** | **GPIO 12** | `camera_driver.h` | Fixed by FPC Ribbon Pinout |
| | **D5** | **GPIO 18** | `camera_driver.h` | Fixed by FPC Ribbon Pinout |
| | **D6** | **GPIO 17** | `camera_driver.h` | Fixed by FPC Ribbon Pinout |
| | **D7** | **GPIO 16** | `camera_driver.h` | Fixed by FPC Ribbon Pinout |
| | **XCLK** | **GPIO 15** | `camera_driver.h` | Fixed by FPC Ribbon Pinout |
| | **PCLK** | **GPIO 13** | `camera_driver.h` | Fixed by FPC Ribbon Pinout |
| | **VSYNC** | **GPIO 6** | `camera_driver.h` | Fixed by FPC Ribbon Pinout |
| | **HREF** | **GPIO 7** | `camera_driver.h` | Fixed by FPC Ribbon Pinout |
| | **SCCB SIOD (SDA)** | **GPIO 4** | `camera_driver.h` | Camera I2C Data (SCCB) |
| | **SCCB SIOC (SCL)** | **GPIO 5** | `camera_driver.h` | Camera I2C Clock (SCCB) |
| | **PWDN / RESET** | `-1` | `camera_driver.h` | Tied to GND / 3.3V |
| **Octal PSRAM / Flash** | **Internal Bus** | **GPIO 35, 36, 37** | Hardware | **Internally Reserved** for 8MB PSRAM |
| **Serial Debug** | **UART0 TX / RX** | **GPIO 43, 44** | Hardware | USB-to-UART / Flashing console |

---

## 🧭 3. Free GPIOs & Future Expansion Plan

With Display, Touch, SD Card, OV2640 Camera, MAX98357A Audio DAC, and INMP441 Microphone all active, the remaining external GPIOs are:

- **Available Free GPIOs**: `GPIO 19`, `GPIO 20`
- **Shared Bus Available**: `GPIO 4` (SDA) and `GPIO 5` (SCL) are already used by the camera SCCB I2C bus and can host secondary I2C slave devices (e.g. MPU6050, PCA9685) provided distinct 7-bit addresses are used.

### Recommended Allocation for Phase 3 & 4:

```
                  ┌─────────────────────────────────────────┐
                  │      ESP32-S3-WROOM-1-N16R8             │
                  └────┬──────────────────────┬─────────────┘
                       │                      │              
               I2C Bus (Sensors)         Motor / PWM Control 
             (PCA9685 / MPU6050)          (L298N / Servos)   
                       ▼                      ▼              
                 SDA: GPIO 19           M1/M2: GPIO 20 (or PCA9685)
                 SCL: GPIO 20           Servos: via PCA9685 channels
```

1. **Option A (PCA9685 Offloaded — Recommended)**:
   - Connect PCA9685 16-channel I2C PWM controller to `GPIO 19` (SDA) and `GPIO 20` (SCL).
   - Drive all 4 motors (via L298N inputs) AND all 4 servos (2× MG996R, 2× SG90) entirely from the PCA9685 channels!
   - This frees up CPU overhead and eliminates pin starvation completely.

2. **Option B (Shared Camera I2C)**:
   - Utilize existing `GPIO 4` (SDA) and `GPIO 5` (SCL) for IMU / sensors, reserving `GPIO 19` and `GPIO 20` for motor direction logic.

---

## 📦 4. Bill of Materials (BOM)

| Component | Status | Qty | Role | Interface / Power |
| :--- | :---: | :---: | :--- | :--- |
| **ESP32-S3-WROOM-1-N16R8** | In Hand & Active | 1 | Main Controller (16MB Flash, 8MB Octal PSRAM) | 5V / 3.3V Logic |
| **OV2640 Camera Module** | In Hand & Active | 1 | Computer Vision & Video Stream | DVP FPC Header |
| **2.4" ILI9341 SPI TFT LCD** | In Hand & Active | 1 | Display / RoboEyes Expressions | SPI (GPIO 40, 41, 39, 42, 2, 1) |
| **XPT2046 Resistive Touch** | In Hand & Active | 1 | Touch Screen UI Input | SPI (GPIO 40, 41, 39, 38, 3) |
| **MicroSD Card (FATFS)** | In Hand & Active | 1 | Audio playback & voice recordings storage | SPI (GPIO 40, 41, 39, 14) |
| **MAX98357A Class-D I2S DAC**| In Hand & Active | 1 | Audio & Speech Output Amplifier | I2S0 (GPIO 47, 48, 21) |
| **INMP441 MEMS Microphone** | In Hand & Active | 1 | Voice Input & Sound Recorder | I2S1 (GPIO 45, 46, 0) |
| **PCA9685 16-Ch PWM Driver** | In Hand | 1 | Offloads Servo and Motor PWM | I2C (GPIO 19, 20 or GPIO 4, 5) |
| **L298N Dual H-Bridge Driver**| In Hand | 1 | 4-Wheel Skid Steer Motor Control | Digital / PWM from PCA9685 |
| **MG996R High-Torque Servos**| In Hand | 2 | Robot Arm Articulation | PWM via PCA9685 |
| **SG90 Micro Servos** | In Hand | 2 | Robot Ear Articulation | PWM via PCA9685 |
| **GY-521 (MPU6050) IMU** | In Hand | 1 | 6-DOF Tilt, Balance & Motion Sense | I2C |
| **N20 DC Gear Motors (6V)** | Planned | 4 | Wheel Drive (~100–200 RPM) | 6V Rail via L298N |
| **Robot Wheels & Tires** | Planned | 4 | Chassis Locomotion | Mechanical |
| **VL53L8CX 8x8 ToF Sensor** | Planned | 1 | Distance Matrix / Obstacle Detection | I2C |
| **LiPo / 18650 Battery + BMS**| Planned | 1 | Regulated Power Rails (5V / 6V / 3.3V) | Power System |
| **4Ω / 8Ω 3W Speaker** | In Hand & Active | 1 | Sound Output paired with MAX98357A | Analog to MAX98357A BTL |

---

## 🛠️ 5. Quick Build & Run Commands

```powershell
# In ESP-IDF PowerShell Environment:
idf.py build

# Flash firmware (adjust COM port to your device)
idf.py -p COM3 flash

# View real-time logs & IP address
idf.py -p COM3 monitor
```
