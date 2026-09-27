# PetBot Firmware & Modular Architecture

## 🚀 Commands
Run in ESP-IDF Terminal:
```bash
idf.py build flash -p COM7 monitor
```

### PSRAM Configuration (`idf.py menuconfig`):
1. **Component config** → **ESP PSRAM**
2. Enable **Support for external, SPI-connected RAM**
3. In **SPI RAM config**: ensure **Mode is set to Octal Mode PSRAM** (ESP32-S3 N16R8)
4. Save (`S`) and Quit (`Q`).


---

## 📚 Documentation
- 📌 [System Status, Active Pin Map & Roadmap](file:///e:/PetBot/PetBot/pet-bot/loona_petbot/docs/STATUS_AND_PINMAP.md)
- 📺 [ILI9341 Display & RoboEyes Engine Guide](file:///e:/PetBot/PetBot/pet-bot/loona_petbot/docs/DISPLAY_DOCUMENTATION.md)

---

## 📁 Architecture & File Structure

```
main/
├── CMakeLists.txt                  # Build configuration & includes
├── idf_component.yml               # Component dependencies
├── main.c                          # Boot orchestrator & subsystem startup
├── app_config.h                    # Master Pin Map & Board Configuration (Centralized GPIOs)
│
├── core/                           # System Core & App Launcher (Task / State Orchestration)
│   ├── app_common.h                # App states, common header & navigation helpers
│   ├── app_common.c
│   ├── app_launcher.h              # App Drawer manager & screensaver timer
│   └── app_launcher.c
│
├── drivers/                        # Hardware Peripheral Drivers (Raw I/O)
│   ├── camera/                     # OV2640 Camera driver
│   │   ├── camera_driver.h
│   │   └── camera_driver.c
│   ├── display/                    # ILI9341 2.4" TFT LCD driver
│   │   ├── display.h
│   │   └── display.c
│   ├── touch/                      # XPT2046 Touch controller driver
│   │   ├── touch_xpt2046.h
│   │   └── touch_xpt2046.c
│   ├── gfx/                        # GFX vector icons, fonts & UI primitives
│   │   ├── display_gfx.h
│   │   └── display_gfx.c
│   ├── motors/                     # Motor & Servo Drivers (Phase 3)
│   │   ├── motor_driver.h / .c     # N20 / L298N Skid-Steer drive
│   │   ├── servo_driver.h / .c     # PCA9685 PWM I2C driver (MG996R Arms, SG90 Ears)
│   │   └── motion_control.h / .c   # Kinematics, speed ramp & turning
│   ├── sensors/                    # Environmental & Inertial Sensors (Phase 4)
│   │   ├── mpu6050_driver.h / .c   # 6-DOF IMU (tilt/balance)
│   │   ├── vl53l8cx_driver.h / .c  # Multi-zone 8x8 ToF Distance Matrix
│   │   └── sensor_fusion.h / .c    # Combined telemetry & fall/cliff safety
│   └── audio/                      # I2S Microphone & Amplifier (Phase 5)
│       ├── i2s_input.h / .c        # INMP441 MEMS microphone stream
│       ├── i2s_output.h / .c       # MAX98357A I2S Class-D DAC / Speaker
│       └── audio_buffer.h / .c     # Ring buffer & audio processing pipeline
│
├── perception/                     # Sensor Data Interpretation & Computer Vision (Phase 6+)
│   ├── vision/                     # Face detection, person tracker, YOLO/ESP-DL inference
│   └── spatial/                    # Obstacle avoidance map & cliff detector
│
├── ai/                             # Voice AI & Personality Engine (Phase 5 & 7)
│   ├── wake_word/                  # Wake-word detection (ESP-SR / custom model)
│   ├── speech_commands/            # Local offline speech command engine
│   ├── nlp/                        # Gemini Cloud AI Client & TTS Engine
│   └── personality/                # Behavior FSM & Emotion State Engine
│
├── behavior/                       # Autonomous Behaviors & Modes (Phase 8)
│   ├── behavior_engine.h / .c      # Follow person, explore room, play mode, sleep mode
│   └── navigation.h / .c           # Obstacle-aware reactive pathing
│
├── apps/                           # Modular Touchscreen Apps (UI Layer)
│   ├── camera/                     # Camera Live Preview App
│   ├── roboeyes/                   # Loona-style Glowing Eye Expressions & Screensaver
│   ├── settings/                   # System Settings & Preferences App
│   ├── sysinfo/                    # System Metrics & Hardware Specs App
│   ├── moods/                      # Interactive Pet Emotion Trigger App
│   ├── webstream/                  # Web Browser MJPEG Stream Info App
│   ├── torch/                      # Flashlight / Torch App
│   └── touch_test/                 # 24-Block Touch Calibration App
│
├── services/                       # Background Connectivity & Network Services
│   ├── wifi/                       # WiFi station manager & auto-reconnect
│   └── web_stream/                 # HTTP MJPEG streaming server
│
└── storage/                        # Persistent Configuration & Assets (Phase 1 & 9)
    ├── nvs_storage.h / .c          # Wi-Fi credentials, user preferences, calibration data
    └── spiffs_assets.h / .c        # Audio clips, sound effects & pre-rendered assets
```

---

## 📋 Hardware Roadmap & Integration Checklist

### 🔌 Power & Actuation
- [ ] Finalize battery pack voltage (drives final N20 motor RPM selection)
- [ ] Buy N20 drive motors + wheels once voltage is locked in
- [ ] Motor + chassis integration (skid-steer wiring per pin map)
- [ ] Confirm L298N ENA/ENB wiring (jumper vs. PWM pin) and finalize speed-control GPIOs
- [ ] Wire PCA9685 + servos (arms via MG996R, ears via SG90) on the shared I2C bus
- [ ] Power management/regulation for full robot (separate power rails for logic vs. motors/servos vs. audio)
- [ ] Mechanical decision: MG996R servos for arms vs. original head/body-tilt plan

### 🧭 Sensors & Telemetry
- [ ] Integrate GY-521 / MPU6050 6-axis IMU on shared I2C bus
- [ ] Add VL53L8CX ToF sensor for multi-zone distance sensing
- [ ] Implement obstacle avoidance & cliff detection logic based on ToF + IMU

### 🎙️ Audio & Voice AI Pipeline
- [ ] Audio input: INMP441 microphone I2S stream
- [ ] Local wake-word detection & offline speech commands
- [ ] Gemini Cloud AI integration for natural conversation fallback
- [ ] Audio output: MAX98357A I2S DAC + speaker (speech, SFX, and pet vocalizations)

---

## 📟 Target Board Hardware Specs

```
Chip:           ESP32-S3 (QFN56) (revision v0.2)
Flash:          16MB Quad SPI Flash
PSRAM:          8MB Octal SPI Embedded PSRAM (AP_3v3)
Cores:          Dual-Core Xtensa LX7 @ 240MHz + Ultra Low Power (ULP) Co-processor
Wireless:       Wi-Fi 802.11 b/g/n + Bluetooth 5.0 (LE)
USB:            Native USB-Serial/JTAG
Display:        2.4" ILI9341 SPI TFT LCD (320x240)
Touch:          XPT2046 Resistive Touch Controller
Camera:         OV2640 DVP Camera Module
```

---

## 🛠 Adding a New App in the Future
To add any new application to the App Launcher:
1. Create a folder in `main/apps/<your_app>/` (e.g. `main/apps/music/`).
2. Add your `app_<your_app>.h` and `app_<your_app>.c` implementing `draw` and `handle_touch`.
3. Add an entry to `AppState` enum in [`main/core/app_common.h`](file:///e:/PetBot/PetBot/pet-bot/loona_petbot/main/core/app_common.h).
4. Register the new tile in `MENU_ITEMS` inside [`main/core/app_launcher.c`](file:///e:/PetBot/PetBot/pet-bot/loona_petbot/main/core/app_launcher.c).
5. Add the source files & include path to [`main/CMakeLists.txt`](file:///e:/PetBot/PetBot/pet-bot/loona_petbot/main/CMakeLists.txt).
