# PetBot Firmware & App Launcher

## 🚀 Commands
Run in ESP-IDF Terminal:
```bash
idf.py build flash -p COM6 monitor
```

### PSRAM Configuration (`idf.py menuconfig`):
1. **Component config** → **ESP PSRAM**
2. Enable **Support for external, SPI-connected RAM**
3. In **SPI RAM config**: ensure **Mode is set to Octal Mode PSRAM** (ESP32-S3 N16R8)
4. Save (`S`) and Quit (`Q`).

---

## 📁 Architecture & File Structure

```
main/
├── CMakeLists.txt                  # Build configuration & includes
├── idf_component.yml               # Component dependencies
├── main.c                          # Boot orchestrator & subsystem startup
│
├── core/                           # System core & App Launcher
│   ├── app_common.h                # App states, common header & navigation helpers
│   ├── app_common.c
│   ├── app_launcher.h              # App Drawer manager & screensaver timer
│   └── app_launcher.c
│
├── drivers/                        # Hardware peripheral drivers
│   ├── camera/                     # OV2640 Camera driver
│   │   ├── camera_driver.h
│   │   └── camera_driver.c
│   ├── display/                    # ILI9341 2.4" TFT LCD driver
│   │   ├── display.h
│   │   └── display.c
│   ├── touch/                      # XPT2046 Touch controller driver
│   │   ├── touch_xpt2046.h
│   │   └── touch_xpt2046.c
│   └── gfx/                        # GFX vector icons, fonts & UI primitives
│       ├── display_gfx.h
│       └── display_gfx.c
│
├── services/                       # Background services & connectivity
│   ├── wifi/                       # WiFi station manager
│   │   ├── wifi_service.h
│   │   └── wifi_service.c
│   └── web_stream/                 # HTTP MJPEG streaming server
│       ├── http_stream.h
│       └── http_stream.c
│
└── apps/                           # Modular Apps (Each self-contained!)
    ├── camera/                     # Camera Live Preview App
    │   ├── app_camera.h
    │   └── app_camera.c
    ├── roboeyes/                   # RoboEyes Screensaver & Expressions
    │   ├── roboeyes_display.h
    │   └── roboeyes_display.cpp
    ├── settings/                   # System Settings & Preferences App
    │   ├── app_settings.h
    │   └── app_settings.c
    ├── sysinfo/                    # System Metrics & Specs App
    │   ├── app_sysinfo.h
    │   └── app_sysinfo.c
    ├── moods/                      # Pet Emotions & Moods App
    │   ├── app_moods.h
    │   └── app_moods.c
    ├── webstream/                  # Web Browser Stream Info App
    │   ├── app_webstream.h
    │   └── app_webstream.c
    ├── torch/                      # Flashlight / Torch App
    │   ├── app_torch.h
    │   └── app_torch.c
    └── touch_test/                 # 24-Block Touch Calibration App
        ├── app_touch_test.h
        └── app_touch_test.c
```

---

## 🛠 Adding a New App in the Future
To add any new application:
1. Create a folder in `main/apps/<your_app>/` (e.g. `apps/music/`).
2. Add your `app_<your_app>.h` and `app_<your_app>.c` implementing `draw` and `handle_touch`.
3. Add an entry to `AppState` in [`app_common.h`](file:///e:/PetBot/PetBot/pet-bot/camera_live/main/core/app_common.h).
4. Register the new tile in `MENU_ITEMS` inside [`app_launcher.c`](file:///e:/PetBot/PetBot/pet-bot/camera_live/main/core/app_launcher.c).
5. Add the source file & include directory to [`main/CMakeLists.txt`](file:///e:/PetBot/PetBot/pet-bot/camera_live/main/CMakeLists.txt).
