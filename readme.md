# PetBot Firmware & Modular Architecture

## 🚀 Quick Start
Run in ESP-IDF PowerShell Terminal:
```powershell
idf.py build flash -p COM3 monitor
```

### PSRAM Configuration (`idf.py menuconfig`):
1. **Component config** → **ESP PSRAM**
2. Enable **Support for external, SPI-connected RAM**
3. In **SPI RAM config**: ensure **Mode is set to Octal Mode PSRAM** (ESP32-S3 N16R8)
4. Save (`S`) and Quit (`Q`).

---

## 📚 Technical Documentation
- 📌 [System Status, Active Pin Map & Roadmap](docs/STATUS_AND_PINMAP.md)
- 📺 [ILI9341 Display & RoboEyes Engine Guide](docs/DISPLAY_DOCUMENTATION.md)
- 🎵 [MAX98357A DAC & INMP441 Mic Audio Subsystem Guide](docs/AUDIO_DOCUMENTATION.md)

---

## 🔌 Hardware Pin Summary

| Subsystem | Signal Name | ESP32-S3 GPIO | Header File | Notes |
|---|---|:---:|---|---|
| **Microphone (INMP441)** | SCK, WS, SD | **45, 46, 0** | `i2s_mic.h` | 16 kHz 16-bit mono input (`I2S_NUM_1`) |
| **Audio DAC (MAX98357A)**| BCLK, WS, DOUT | **47, 48, 21** | `i2s_audio.h` | 16-bit DMA stereo downmix (`I2S_NUM_0`) |
| **Display (ILI9341)** | SCK, MOSI, MISO | **40, 41, 39** | `display.h` | Shared `SPI2_HOST` |
| | CS, DC, RST, LED | **42, 2, 1, 3.3V** | `display.h` | Dedicated display control lines |
| **Touch (XPT2046)** | CS, IRQ | **38, 3** | `touch_xpt2046.h` | Shared SPI (40, 41, 39) + Pen IRQ |
| **SD Card (Display Slot)**| CS | **14** | `sdcard.h` | Shared SPI (40, 41, 39), FATFS mount |
| **Camera (OV2640 DVP)** | D0-D7, Clocks, SCCB | **11, 9, 8, 10, 12, 18, 17, 16, 15, 13, 6, 7, 4, 5** | `camera_driver.h` | Direct FPC Ribbon mapping |
| **Serial Debug** | TX, RX | **43, 44** | Hardware | UART0 USB flashing & console |
| **Octal PSRAM / Flash** | Internal | **35, 36, 37** | Hardware | Internally reserved for 8MB PSRAM |
| **Available Free Pins** | GPIO | **19, 20** | Future Expansion | For I2C (PCA9685/IMU) or Motors |

---

## 📁 Architecture & File Structure

```
main/
├── CMakeLists.txt                  # Build configuration & includes
├── idf_component.yml               # Component dependencies
├── main.c                          # Boot orchestrator & subsystem startup
│
├── core/                           # System Core & App Launcher (Task / State Orchestration)
│   ├── app_common.h                # App states, DMA status bar & navigation helpers
│   ├── app_common.c
│   ├── app_launcher.h              # App Drawer manager & screensaver timer
│   └── app_launcher.c
│
├── drivers/                        # Hardware Peripheral Drivers (Raw I/O)
│   ├── camera/                     # OV2640 DVP Camera driver
│   │   ├── camera_driver.h / .c
│   ├── display/                    # ILI9341 2.4" TFT LCD SPI driver (DMA bands)
│   │   ├── display.h / .c
│   ├── touch/                      # XPT2046 Resistive Touch driver (pen IRQ)
│   │   ├── touch_xpt2046.h / .c
│   ├── gfx/                        # GFX vector icons, fonts & UI primitives
│   │   ├── display_gfx.h / .c
│   ├── storage/                    # MicroSD card driver (FATFS on shared SPI)
│   │   ├── sdcard.h / .c
│   └── audio/                      # I2S Digital Audio Drivers
│       ├── i2s_audio.h / .c        # MAX98357A I2S Class-D DAC output (I2S_NUM_0)
│       └── i2s_mic.h / .c          # INMP441 I2S MEMS mic input & RMS monitor (I2S_NUM_1)
│
├── apps/                           # Modular Touchscreen Apps (UI Layer)
│   ├── camera/                     # Live Viewfinder Camera App
│   ├── roboeyes/                   # Loona-style Glowing Eye Expressions & Screensaver
│   ├── music/                      # SD Music & Audio Player App
│   ├── settings/                   # System Settings & Preferences App
│   ├── moods/                      # Interactive Pet Emotion Trigger App
│   ├── sysinfo/                    # System Metrics & Hardware Specs App
│   ├── recorder/                   # Digital Voice Recorder & Peak VU Meter App
│   ├── touch_test/                 # 24-Block Touch Calibration App
│   └── webstream/                  # Web Browser MJPEG Stream Info App
│
└── services/                       # Background Connectivity & Subsystems
    ├── audio_player/               # Background MP3/WAV decoder & audio stream engine
    ├── battery/                    # Battery monitor service & status bar integration
    ├── wifi/                       # WiFi station manager (flicker-free status bar updates)
    └── web_stream/                 # HTTP MJPEG video streamer server
```

---

## 📋 Hardware Roadmap & Integration Checklist

### 🔌 Power & Actuation
- [ ] Wire PCA9685 16-ch I2C PWM driver to `GPIO 19` (SDA) and `GPIO 20` (SCL)
- [ ] Connect L298N Dual H-Bridge motor driver to PCA9685 PWM outputs
- [ ] Buy 4× N20 DC gear motors (6V) + robot wheels
- [ ] Wire 2× MG996R servos (arms) and 2× SG90 servos (ears) to PCA9685
- [ ] Build isolated power distribution rails (5V logic, 6V motors/servos, 5V audio)

### 🧭 Sensors & Telemetry
- [ ] Connect GY-521 / MPU6050 6-axis IMU to the I2C bus
- [ ] Add VL53L8CX 8x8 ToF sensor for multi-zone distance sensing & cliff avoidance
- [ ] Implement reactive fall/lift-up detection logic

### 🎙️ Audio & Voice AI Pipeline
- [x] Audio output: MAX98357A I2S DAC + speaker (I2S_NUM_0)
- [x] Audio input: INMP441 digital microphone I2S stream (I2S_NUM_1)
- [x] Sound Recorder app with live VU meter & WAV playback
- [ ] Local wake-word detection (ESP-SR) using INMP441 audio feed
- [ ] Gemini Cloud AI integration for voice conversation and pet personality

---

## 📟 Target Board Hardware Specs

```
Chip:           ESP32-S3-WROOM-1-N16R8 (QFN56)
Flash:          16MB Quad SPI Flash
PSRAM:          8MB Octal SPI Embedded PSRAM (AP_3v3)
Cores:          Dual-Core Xtensa LX7 @ 240MHz
Wireless:       Wi-Fi 802.11 b/g/n + Bluetooth 5.0 (LE)
Display:        2.4" ILI9341 SPI TFT LCD (320x240)
Touch:          XPT2046 12-bit Resistive Touch Controller
Camera:         OV2640 DVP Camera Module
DAC:            MAX98357A I2S Class-D mono amplifier
Microphone:     INMP441 I2S digital MEMS microphone
Storage:        MicroSD card slot (SPI mode, FATFS)
```

---

## 🛠 Adding a New App
To add any new application to the App Launcher:
1. Create a folder in `main/apps/<your_app>/` (e.g. `main/apps/example/`).
2. Add your `app_<your_app>.h` and `app_<your_app>.c` implementing `draw` and `handle_touch`.
3. Add an entry to the `AppState` enum in [`main/core/app_common.h`](main/core/app_common.h).
4. Register the new tile in `MENU_ITEMS` inside [`main/core/app_launcher.c`](main/core/app_launcher.c).
5. Add the source files & include path to [`main/CMakeLists.txt`](main/CMakeLists.txt).
