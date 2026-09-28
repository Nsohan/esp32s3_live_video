# 🎵 PetBot Audio Subsystem Documentation

Comprehensive reference documentation for the **MAX98357A I2S DAC**, **MicroSD Audio Streaming**, **Audio Player Service**, and **Music App** on the **ESP32-S3** in native **ESP-IDF v6.1**.

---

## 📌 1. Hardware Architecture & Pinout

The PetBot audio pipeline uses a dedicated digital **MAX98357A I2S Class-D mono amplifier** connected to a 3W / 4Ω or 8Ω speaker, reading audio files from a **MicroSD Card** sharing the display's high-speed SPI bus.

### MAX98357A Pinout Mapping

| MAX98357A Pin | Function | ESP32-S3 GPIO | Description & Electrical Notes |
|---|---|:---:|---|
| **VIN / VCC** | Power Supply | `5V` (or `3.3V`) | 5V recommended from USB rail for maximum audio headroom and volume |
| **GND** | Ground | `GND` | Common system ground |
| **BCLK** | Continuous Serial Clock | **`GPIO 47`** | Bit Clock driven by ESP32-S3 I2S0 peripheral |
| **LRC / WS** | Word Select (Left/Right) | **`GPIO 48`** | Frame Clock (44.1 kHz, 22.05 kHz, etc.) |
| **DIN** | Serial Data In | **`GPIO 21`** | High-speed I2S DOUT from ESP32-S3 |
| **SD_MODE** | Channel Select & Shutdown | `Pull-up` / `NC` | Hardware channel selector (see section below) |
| **GAIN** | Hardware Gain Select | `GND` / `Float` | Float = 9 dB, GND = 12 dB, 100k to GND = 6 dB |
| **SPK+ / SPK-** | Speaker Outputs | Speaker | Bridge-Tied Load (BTL) speaker outputs |

> [!TIP]
> **Mono Downmix in Software:**
> The MAX98357A is a mono amplifier. Depending on the resistor on its `SD_MODE` pin, it decodes either the Left channel, Right channel, or the average `(L+R)/2`. To guarantee full fidelity on any hardware wiring, our driver **automatically downmixes stereo signals `(L + R) / 2` in software and duplicates the result into both Left and Right slots**.

---

### MicroSD Card Pinout (Shared Display SPI Bus)

The MicroSD card slot is located on the back of the 2.4" ILI9341 display PCB and shares the primary SPI bus:

| SD Card Pin | Function | ESP32-S3 GPIO | Bus Sharing Notes |
|---|---|:---:|---|
| **SD_CS** | Chip Select | **`GPIO 14`** | Dedicated active-LOW chip select |
| **SD_MOSI** | Data In | **`GPIO 41`** | Shared with Display MOSI |
| **SD_MISO** | Data Out | **`GPIO 39`** | Shared with Touch `T_DO` / Display MISO |
| **SD_SCK** | Serial Clock | **`GPIO 40`** | Shared with Display SCK (clocked at 20 MHz) |

---

## 🏗️ 2. Software Architecture Layers

The audio subsystem is divided into three distinct modular layers:

```
┌────────────────────────────────────────────────────────┐
│               Music Application UI                     │
│         (main/apps/music/app_music.c)                  │
│   • Scans /sdcard/sounds and /sdcard/music             │
│   • Touch scroll list, Play/Pause/Stop, Vol Controls   │
└───────────────────────────┬────────────────────────────┘
                            │ FreeRTOS Audio Queue
┌───────────────────────────▼────────────────────────────┐
│              Audio Player Service                      │
│   (main/services/audio_player/audio_player.c)          │
│   • Dedicated background task on Core 0 (Priority 5)   │
│   • minimp3 frame decoder & stream refill              │
│   • Fast ID3v2 tag parser & sync word finder           │
│   • RIFF/WAVE header chunk scanner                     │
└───────────────────────────┬────────────────────────────┘
                            │ PCM Buffer
┌───────────────────────────▼────────────────────────────┐
│                I2S Audio Driver                        │
│         (main/drivers/audio/i2s_audio.c)               │
│   • Native ESP-IDF v6.1 esp_driver_i2s (STD Mode)      │
│   • 8 x 256-frame GDMA descriptors (1024 bytes/desc)   │
│   • Dynamic rate/slot reconfiguration                  │
│   • Stereo-to-mono downmixing & digital volume scale   │
│   • Sine wave synthesizer for UI clicks and beeps      │
└────────────────────────────────────────────────────────┘
```

---

## ⚙️ 3. I2S Peripheral & DMA Configuration

### Critical Driver Specifications (`i2s_audio.c`)

- **I2S Peripheral**: `I2S_NUM_0` configured as `I2S_ROLE_MASTER`.
- **Mode**: Standard Philips Mode (`I2S_COMM_MODE_STD`).
- **Data Bit Width**: 16-bit signed integer (`I2S_DATA_BIT_WIDTH_16BIT`).
- **Slot Bit Width**: **Strict 16-bit** (`I2S_SLOT_BIT_WIDTH_16BIT`).
- **Slot Mode**: Stereo (`I2S_SLOT_MODE_STEREO`) with `I2S_STD_SLOT_BOTH`.
- **DMA Buffer Count**: `8` descriptors.
- **DMA Buffer Length**: `256` frames.
  - Calculation: $256 \text{ frames} \times 2 \text{ channels} \times 2 \text{ bytes} = 1024 \text{ bytes per descriptor}$.
  - Total DMA ring buffer: $8 \times 1024 = 8192 \text{ bytes}$ (~46.4 ms latency at 44.1 kHz).
  - This avoids the ESP32-S3 GDMA 511-frame clamp warning.

### Dynamic Parameter Reconfiguration Sequence

When transitioning between audio files with differing sample rates (e.g., from 44.1 kHz MP3 to 16 kHz WAV), ESP-IDF requires the slot configuration to be updated **before** the clock configuration:

```c
i2s_channel_disable(s_tx_chan);
// 1. Reconfigure slot FIRST so BCLK calculations know the slot width
i2s_channel_reconfig_std_slot(s_tx_chan, &slot_cfg);
// 2. Reconfigure clock SECOND using the updated slot parameters
i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg);
// 3. Re-enable channel
i2s_channel_enable(s_tx_chan);
```

---

## 📁 4. Supported Audio Formats & Decoding

### 1. MP3 Audio (`minimp3`)
- **Decoder Engine**: `minimp3` (public domain, header-only).
- **Memory Allocation**: Decoder scratchpad and struct are heap-allocated via `calloc()` (saves 16.5 KB of task stack).
- **Stream Staging Buffer**: 4,096 bytes input buffer. Refilled from MicroSD card in 2,048-byte blocks when the buffer drops below half.
- **ID3v2 Metadata Fast-Seek**: Reads the 10-byte ID3 header, calculates the synchsafe 28-bit tag size, and calls `fseek()` to instantly skip high-resolution album art images and ID3 tags.
- **Sync Word Recovery**: If garbage or padding bytes are encountered, the decoder scans for the MPEG sync word (`0xFF 0xEx`) and shifts directly to the next frame.

### 2. WAV Audio (PCM)
- **Decoder Engine**: Native RIFF chunk parser.
- **Chunk Scanner**: Automatically seeks past `LIST`, `JUNK`, and metadata chunks to isolate `fmt ` and `data` blocks.
- **Supported Encodings**: 16-bit PCM, 8-bit unsigned PCM; Mono and Stereo; sample rates from 8,000 Hz to 48,000 Hz.

### 3. Procedural Synthetic Tones
- **Sine Wave Generator**: Real-time sine computation with a 150-sample smooth cosine attack/decay envelope to eliminate speaker clicks/pops.
- **Presets**:
  - `CMD_PLAY_CLICK`: 1,800 Hz / 25 ms UI touch feedback click.
  - `CMD_PLAY_HAPPY`: 3-tone Loona chirp (880 Hz $\rightarrow$ 1174 Hz $\rightarrow$ 1760 Hz).
  - `CMD_PLAY_TEST_TONE`: 1,000 Hz / 1.0 second calibrated audio test tone.

---

## 🩺 5. History of Issues & Solutions (Lessons Learned)

| Issue Encountered | Root Cause | Permanent Solution |
|---|---|---|
| **Crash when tapping a track** | `minimp3` allocates a 16.5 KB `mp3dec_scratch_t` struct on the task stack inside `mp3dec_decode_frame`, blowing through the 4 KB FreeRTOS stack. | Embedded the scratchpad directly into `mp3dec_t` and allocated it dynamically on the heap with `calloc()`. |
| **Continuous 1-second boot loop** | Six 25.6 KB internal SRAM display chunk buffers consumed all DMA-capable memory, starving the WiFi driver. | Consolidated the display driver into 1 single shared DMA chunk buffer (freed 130 KB internal SRAM). |
| **Fast Forward (~2x–3x speed) & Harsh Noise** | Slot width was set to `I2S_SLOT_BIT_WIDTH_32BIT`. The I2S DMA consumed 4 bytes per sample for 16-bit PCM, doubling consumption speed and corrupting audio data. | Set `.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT` matching 16-bit PCM, and implemented stereo-to-mono software downmixing. |
| **`i2s_common: dma frame num limited to 511` warning** | `dma_frame_num` was set to `512`, which exceeds the S3 GDMA limit. The driver clamped it to 511 (odd frame count), causing sample misalignment. | Reduced `dma_frame_num` to `256` frames (1024 bytes per descriptor), ensuring power-of-two alignment. |
| **Freezing/lag on MP3 start** | Embedded album art (JPEG/PNG) in MP3 files was being parsed byte-by-byte with thousands of `memmove()` calls. | Implemented `skip_id3v2_tag()` to fast-seek past metadata directly to audio headers in a single operation. |
| **GCC 14 `-Werror=stringop-truncation`** | GCC 14 flags `strncpy(dest, src, n - 1)` with `dest[n - 1] = 0` as a build-breaking error. | Replaced with safe, bounded loops and `snprintf`. |

---

## 💻 6. Public C API Reference

### I2S Low-Level Driver (`i2s_audio.h`)

```c
// Initialize I2S peripheral, pins, and DMA buffers
esp_err_t i2s_audio_init(uint32_t sample_rate, uint16_t bits_per_sample, uint8_t channels);

// Dynamically adjust sample rate and channel count
esp_err_t i2s_audio_set_params(uint32_t sample_rate, uint16_t bits_per_sample, uint8_t channels);

// Write raw PCM samples to DMA (blocks cleanly with flow control)
esp_err_t i2s_audio_write(const void *src, size_t size, size_t *bytes_written, uint32_t timeout_ms);

// Master volume controls (0 to 100%)
void i2s_audio_set_volume(uint8_t volume_percent);
uint8_t i2s_audio_get_volume(void);

// Play a synthesized sine-wave tone
void i2s_audio_play_beep(uint32_t freq_hz, uint32_t duration_ms);

// Deinitialize and release I2S peripheral
void i2s_audio_deinit(void);
```

### High-Level Audio Player Service (`audio_player.h`)

```c
// Start the background audio playback FreeRTOS task
esp_err_t audio_player_init(void);

// Queue and play an MP3 or WAV file from the filesystem
esp_err_t audio_player_play_file(const char *filepath);

// Playback state controls
void audio_player_pause(void);
void audio_player_resume(void);
void audio_player_toggle_play_pause(void);
void audio_player_stop(void);

// State inspection
audio_player_state_t audio_player_get_state(void);
const char* audio_player_get_current_track_name(void);

// Procedural audio triggers
void audio_player_play_ui_click(void);
void audio_player_play_happy_sound(void);
void audio_player_play_test_tone(void);
```

---

## 📂 7. SD Card Audio File Structure

The Music app automatically scans the following directories on boot:
- `/sdcard/sounds/` — Short sound effects and Loona voice prompts.
- `/sdcard/music/` — Full-length songs and music tracks.
- `/sdcard/` — Root fallback directory.

Supported filenames: any file ending in `.mp3`, `.MP3`, `.wav`, or `.WAV` (up to 32 tracks indexed in playlist).
