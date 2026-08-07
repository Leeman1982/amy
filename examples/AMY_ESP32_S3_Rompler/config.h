#pragma once
// ============================================================================
//  AMY ESP32-S3 Rompler — config.h
//  Board : any ESP32-S3-WROOM(-1) dev board (Arduino-ESP32 core)
//  Audio : AMY (PCM sample playback engine) -> I2S -> external DAC (PCM5102-
//          class, no MCLK required)
//  Disp  : 128x64 OLED, SH1106 controller, hardware I2C
//  Input : 1x rotary encoder w/ push + 5x momentary switches (same layout as
//          the xtro UltimateSynthNYR control surface this UI/sequencer was
//          ported from)
//  Store : microSD over SPI — patches (multisample banks) are streamed/loaded
//          from the card on demand, never all held in RAM at once.
//
//  Pin map below is a safe default for a generic ESP32-S3-WROOM-1 devkit: it
//  avoids the USB-CDC pins (19/20), the UART0 boot-strap pins (0/3/45/46) and
//  the GPIO26-37 range some WROOM-1 modules reserve for octal PSRAM/flash.
//  Every pin is a single #define here — remap freely for your own wiring.
// ============================================================================

#include <Arduino.h>

// ─── I2S audio out -> external DAC (PCM5102 / PCM5100A / similar) ──────────
// ESP32-S3's I2S peripheral can route BCLK/LRCLK/DOUT to any GPIO (no
// consecutive-pin requirement like the RP2350 PIO I2S this was ported from).
// PCM5102 wiring: BCK<-BCLK  LRCK<-LRC  DIN<-DOUT  SCK->GND (internal PLL)
//                 FLT/DEMP/FMT to GND, XSMT to 3V3.
#define PIN_I2S_BCLK    15
#define PIN_I2S_LRC     16
#define PIN_I2S_DOUT    17
#define PIN_I2S_MCLK    -1     // not needed by PCM5102-class DACs

// ─── microSD over SPI ────────────────────────────────────────────────────────
// A plain SPI breakout board (the common cheap microSD adapters are SPI-only,
// not 4-bit SDIO), so this uses Arduino SD.h / SPI.h rather than SD_MMC.
#define PIN_SD_CS       10
#define PIN_SD_SCK      12
#define PIN_SD_MISO     13
#define PIN_SD_MOSI     11
#define SD_SPI_HZ       20000000   // most breakout boards are happy at 20 MHz

// ─── 128x64 OLED, SH1106, hardware I2C ──────────────────────────────────────
#define PIN_OLED_SDA     8
#define PIN_OLED_SCL     9
#define OLED_ADDR       0x3C   // 0x3C typical; some panels are 0x3D
#define OLED_I2C_HZ     1000000   // see xtro's note: 400000 if the panel glitches

// ─── Rotary encoder (EC11 w/ push) ──────────────────────────────────────────
#define PIN_ENC_A        4
#define PIN_ENC_B        5
#define PIN_ENC_SW       6
#define ENC_TICKS_PER_DETENT 4

// ─── 5 momentary switches (active-low to GND, internal pull-ups) ────────────
#define PIN_BTN_PLAY     7     // play / stop / confirm  (long = toggle step)
#define PIN_BTN_SHIFT   18     // modifier (hold)
#define PIN_BTN_PAGE    21     // back / menu            (long = save)
#define PIN_BTN_TRACK   38     // switch active track    (long = copy pattern)
#define PIN_BTN_MUTE    39     // toggle step / mute     (long = paste pattern)

#define DEBOUNCE_MS     15
#define LONG_PRESS_MS   600

// ─── Optional click/gate out (metronome pulse, unused unless wired) ─────────
#define PIN_CLICK_OUT   40

// ─── MIDI (5-pin DIN or TRS, opto-isolated in) ──────────────────────────────
// ESP32-S3's GPIO matrix routes any UART to any pin directly — no PIO/software
// UART trick needed (unlike the RP2350 board this was ported from).
#define PIN_MIDI_TX     41
#define PIN_MIDI_RX     42     // -1 to disable MIDI IN
#define MIDI_BAUD       31250
#define MIDI_UART_NUM    1     // HardwareSerial port index (Serial1)

// ─── Optional status LED (plain GPIO LED, heartbeat blink) ──────────────────
// Some ESP32-S3 devkits carry an addressable NeoPixel instead of a plain LED
// on their "onboard LED" pin (e.g. GPIO48 on DevKitC-1) — driving that needs
// Adafruit_NeoPixel, deliberately not pulled in here to keep the dependency
// list at just U8g2. Wire a plain LED (+ resistor) to PIN_STATUS_LED, or set
// HAS_STATUS_LED to 0 if your board's LED pin is a NeoPixel or unused.
#define HAS_STATUS_LED   1
#define PIN_STATUS_LED  47

// ─── Audio ──────────────────────────────────────────────────────────────────
// AMY's own block size (AMY_BLOCK_SIZE, 256 samples) and sample rate
// (AMY_SAMPLE_RATE, 44100 by default) drive the actual render; these two are
// only used by the sequencer/UI for tempo math and are kept at AMY's default.
#define SAMPLE_RATE       44100
#define SAMPLE_RATE_F     44100.0f

// ─── Polyphony ──────────────────────────────────────────────────────────────
// Per-track voice ceiling handed to AMY when a patch is loaded (one AMY
// "synth" per sequencer track). Streamed (disk) zones are effectively
// monophonic per zone regardless of this number — see patch.h.
#define TRACK_POLYPHONY    8

// ─── Sequencer dimensions ───────────────────────────────────────────────────
// Every track drives its own AMY synth (its own loaded patch) and can also
// echo to MIDI OUT on its own channel — there is no "MIDI-only" track here,
// every track is a rompler part.
#define NUM_TRACKS        4
#define NUM_STEPS         64
#define STEPS_PER_PAGE    16      // the 2x8 grid the main screen draws
#define NUM_PAGES         (NUM_STEPS / STEPS_PER_PAGE)
#define DEFAULT_STEPS     16
#define NUM_PATTERNS      8
#define CHAIN_LEN         8
#define STEP_MAX_NOTES    4       // chord size per step
#define STEP_LOCKS        4       // parameter locks per step

// ─── Tempo ──────────────────────────────────────────────────────────────────
#define BPM_DEFAULT    120
#define BPM_MIN        40
#define BPM_MAX        300
#define SWING_MAX      66      // % maximum swing

// ─── Display layout ─────────────────────────────────────────────────────────
#define DISP_W            128
#define DISP_H            64
#define HEADER_H          10
#define STEP_CELL_W       16
#define STEP_CELL_H       12
#define STEP_ROW1_Y       12
#define STEP_ROW2_Y       25
#define UI_REFRESH_MS     33    // ~30 fps

// ─── Storage (SD card) ──────────────────────────────────────────────────────
#define SD_MOUNT_POINT    "/sd"
#define PATCH_DIR         "/patches"
#define KIT_DIR            "/kits"
#define STORAGE_MAGIC     0x5241   // 'RA' — Rompler AMY
#define STORAGE_VERSION   1
#define KIT_SLOTS         32
#define MAX_PATCHES       64       // patches listed in the on-screen browser
#define MAX_ZONES_PER_PATCH 32
