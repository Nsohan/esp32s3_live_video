# 🎵 PetBot Audio Subsystem Documentation

Comprehensive reference documentation for the PetBot digital audio pipeline running on the **ESP32-S3** in native **ESP-IDF v6.1**:
- **Audio Output**: MAX98357A Class-D I2S DAC (`I2S_NUM_0`)
- **Audio Input**: INMP441 Omnidirectional MEMS Microphone (`I2S_NUM_1`)
- **Storage & Playback**: MicroSD Card FATFS streaming (`/sdcard`)
- **Applications**: Interactive **Music Player App** and **Audio Recorder App**

---

## 📌 1. Hardware Architecture & Pinouts

The PetBot architecture features independent dual-channel digital audio peripherals:
1. **`I2S_NUM_0` (Transmitter / Output)**: Drives the MAX98357A amplifier.
2. **`I2S_NUM_1` (Receiver / Input)**: Reads digital 24/16-bit PDM/PCM samples from the INMP441 microphone.

```
                   ┌────────────────────────────────────────┐
                   │          ESP32-S3-WROOM-1              │
                   └──────┬──────────────────┬──────────────┘
                          │                  │               
                     I2S_NUM_0 (TX)     I2S_NUM_1 (RX)       
                   (BCLK:47, WS:48,   (SCK:45, WS:46,        
                       DOUT:21)            SD:0)             
                          │                  │               
                          ▼                  ▼               
                   ┌─────────────┐    ┌─────────────┐        
                   │  MAX98357A  │    │   INMP441   │        
                   │  Class-D    │    │  MEMS Mic   │        
                   │  Amplifier  │    │  (Digital)  │        
                   └──────┬──────┘    └─────────────┘        
                          │                                  
                          ▼                                  
                   ┌─────────────┐                           
                   │ 3W Speaker  │                           
                   └─────────────┘                           
```

---

### A. MAX98357A I2S DAC Pinout (Output)

| MAX98357A Pin | Function | ESP32-S3 GPIO | Description & Electrical Notes |
|---|---|:---:|---|
| **VIN / VCC** | Power Supply | `5V` (or `3.3V`) | 5V recommended from USB rail for maximum volume and headroom |
| **GND** | Ground | `GND` | Common system ground |
| **BCLK** | Continuous Serial Clock | **`GPIO 47`** | Bit Clock driven by ESP32-S3 `I2S_NUM_0` peripheral |
| **LRC / WS** | Word Select (Left/Right) | **`GPIO 48`** | Frame Clock (44.1 kHz, 22.05 kHz, 16 kHz) |
| **DIN** | Serial Data In | **`GPIO 21`** | High-speed I2S DOUT from ESP32-S3 |
| **SD_MODE** | Channel Select & Shutdown | `Pull-up` / `NC` | Hardware channel selector |
| **GAIN** | Hardware Gain Select | `GND` / `Float` | Float = 9 dB, GND = 12 dB, 100k to GND = 6 dB |
| **SPK+ / SPK-** | Speaker Outputs | Speaker | Bridge-Tied Load (BTL) outputs to 3W 4Ω/8Ω speaker |

> [!TIP]
> **Mono Downmix in Software:**
> The MAX98357A is a mono amplifier. Our driver automatically downmixes stereo signals `(L + R) / 2` in software and duplicates the result into both Left and Right slots to ensure compatibility with any stereo audio source.

---

### B. INMP441 MEMS Microphone Pinout (Input)

| INMP441 Pin | Function | ESP32-S3 GPIO | Description & Electrical Notes |
|---|---|:---:|---|
| **VDD** | Power Supply | `3.3V` | Digital 3.3V power supply |
| **GND** | Ground | `GND` | Common system ground |
| **SCK** | Serial Clock | **`GPIO 45`** | Bit Clock driven by ESP32-S3 `I2S_NUM_1` peripheral |
| **WS** | Word Select | **`GPIO 46`** | 16 kHz frame sync pulse |
| **SD** | Serial Data Out | **`GPIO 0`** | Digital 24-bit audio data into ESP32-S3 (internal pullup) |
| **L/R** | Channel Select | `GND` | Tied to GND to transmit on Left slot |

---

### C. MicroSD Card Pinout (Shared Display SPI Bus)

The MicroSD card slot shares the primary high-speed SPI bus with the ILI9341 display and XPT2046 touch controller:

| SD Card Pin | Function | ESP32-S3 GPIO | Bus Sharing Notes |
|---|---|:---:|---|
| **SD_CS** | Chip Select | **`GPIO 14`** | Dedicated active-LOW chip select |
| **SD_MOSI** | Data In | **`GPIO 41`** | Shared with Display MOSI (`LCD_PIN_MOSI`) |
| **SD_MISO** | Data Out | **`GPIO 39`** | Shared with Touch `T_DO` and Display MISO |
| **SD_SCK** | Serial Clock | **`GPIO 40`** | Shared with Display SCK (`LCD_PIN_SCK`) |

---

## 🏗️ 2. Software Architecture Layers

```
┌──────────────────────────────────────┐  ┌──────────────────────────────────────┐
│        Music Player App              │  │        Audio Recorder App            │
│   (main/apps/music/app_music.c)      │  │ (main/apps/recorder/app_recorder.c)  │
│  • SD track browser & playback UI    │  │  • 20-segment LED-style peak VU meter│
│  • Volume slider & track control     │  │  • 16kHz WAV recording & playback    │
└──────────────────┬───────────────────┘  └──────────────────┬───────────────────┘
                   │ FreeRTOS Queue                          │
┌──────────────────▼───────────────────┐                     │
│         Audio Player Service         │                     │
│ (main/services/audio_player/)        │                     │
│  • Background task on Core 0         │                     │
│  • minimp3 frame decoder & stream    │                     │
│  • RIFF/WAVE header chunk scanner    │                     │
└──────────────────┬───────────────────┘                     │
                   │ PCM Buffer                              │
┌──────────────────▼───────────────────┐  ┌──────────────────▼───────────────────┐
│           I2S Audio Output           │  │           I2S Audio Input            │
│   (main/drivers/audio/i2s_audio.c)   │  │    (main/drivers/audio/i2s_mic.c)    │
│  • I2S_NUM_0 (TX, Master)            │  │  • I2S_NUM_1 (RX, Master)            │
│  • 8 x 256-frame GDMA descriptors    │  │  • 16 kHz 16-bit mono capture        │
│  • Dynamic sample rate switching     │  │  • Real-time RMS loudness monitor    │
│  • Procedural UI beep synthesizer    │  │  • Non-blocking DMA ring buffer      │
└──────────────────────────────────────┘  └──────────────────────────────────────┘
```

---

## ⚙️ 3. I2S Transmitter Details (`i2s_audio.c`)

- **Peripheral**: `I2S_NUM_0` configured as `I2S_ROLE_MASTER`.
- **Mode**: Standard Philips Mode (`I2S_COMM_MODE_STD`).
- **Data Bit Width**: 16-bit signed integer (`I2S_DATA_BIT_WIDTH_16BIT`).
- **Slot Bit Width**: Strict 16-bit (`I2S_SLOT_BIT_WIDTH_16BIT`).
- **Slot Mode**: Stereo (`I2S_SLOT_MODE_STEREO`) with `I2S_STD_SLOT_BOTH`.
- **DMA Buffer Count**: `8` descriptors.
- **DMA Buffer Length**: `256` frames.
  - Calculation: $256 \times 2 \times 2 = 1024 \text{ bytes per descriptor}$.
  - Ring buffer: $8 \times 1024 = 8192 \text{ bytes}$ (~46.4 ms latency at 44.1 kHz).
  - Compliant with ESP32-S3 GDMA descriptor limits.

### Dynamic Clock & Slot Reconfiguration
When tracks change sample rate (e.g., 44.1 kHz MP3 to 16 kHz WAV), the driver safely updates the slot configuration **before** the clock configuration:
```c
i2s_channel_disable(s_tx_chan);
i2s_channel_reconfig_std_slot(s_tx_chan, &slot_cfg);
i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg);
i2s_channel_enable(s_tx_chan);
```

---

## 🎙️ 4. I2S Microphone Details (`i2s_mic.c`)

- **Peripheral**: `I2S_NUM_1` configured as `I2S_ROLE_MASTER` (RX only).
- **Standard Format**: 16 kHz sample rate, 16-bit resolution, Left slot.
- **RMS Level Monitor**: Computes the Root Mean Square (RMS) of audio chunks for real-time visualization and speech detection:
$$\text{RMS} = \sqrt{\frac{1}{N} \sum_{i=0}^{N-1} x[i]^2}$$
- **API Functions**:
  - `i2s_mic_init()`: Initializes the RX channel on GPIO 45, 46, 0.
  - `i2s_mic_read()`: Reads raw 16-bit samples from the DMA ring buffer.
  - `i2s_mic_get_latest_rms()`: Returns current sound pressure level for UI meters.
  - `i2s_mic_start_level_monitor()` / `i2s_mic_stop_level_monitor()`: Background serial logger.

---

## ⏺️ 5. Sound Recorder Application (`app_recorder.c`)

The built-in **Recorder App** provides an interactive voice recording workstation:
- **Live 20-Segment VU Meter**:
  - Smooth peak-decay algorithm visually displays ambient volume and voice dynamics.
  - Segments transition from Cyan (normal) to Green (optimal) to Red (peak overload).
- **WAV File Generation**:
  - Generates standard 44-byte RIFF/WAVE headers at 16,000 Hz, 16-bit mono.
  - Streams directly to `/sdcard/recordings/rec_latest.wav`.
  - Includes a **320 KB RAM fallback buffer** (~10 seconds) if an SD card is not inserted.
- **Touch Controls**:
  - **Record Button**: Starts recording from the INMP441 with active timer.
  - **Stop Button**: Finalizes WAV header, flushes FATFS buffers, updates file length.
  - **Play Button**: Immediately plays back recorded audio through the MAX98357A speaker.

---

## 📂 6. SD Card Directory Structure

```
/sdcard/
├── music/            # Full-length songs (.mp3, .wav)
├── sounds/           # UI sounds, chirps, and robotic voice clips (.mp3, .wav)
└── recordings/       # Recorded voice notes saved by the Recorder app
    └── rec_latest.wav
```
