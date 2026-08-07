# AMY ESP32-S3 Rompler

A sample-based rompler (ROM player / multisample sampler) built on the
[AMY](https://github.com/shorepine/amy) sound engine, targeting a plain
ESP32-S3-WROOM dev board with a microSD card for sample storage and a
128x64 SH1106 OLED for the UI.

The UI and step sequencer are ported from
[xtro](https://github.com/shorepine/xtro)'s `UltimateSynthNYR` firmware —
that project's menu system and sequencer engine were already decoupled from
its (RP2350-specific) synth voices, so they're reused here largely as-is,
retargeted to trigger AMY's PCM sample playback instead. AMY itself supplies
the actual sound engine: PCM sample playback with pitch-shifting, looping,
per-oscillator envelopes, and — the piece that makes on-demand SD loading
possible — a pluggable file-I/O hook so samples can be streamed a block at a
time straight off the card instead of living in RAM. See `patch.h` and
`rompler_engine.h` for where the two meet: AMY has no built-in concept of a
multisample "patch" (only a fixed GM drum table), so that mapping — which
note/velocity plays which sample file — is what this example adds on top.

## Hardware

| Function            | Signal      | ESP32-S3 GPIO | Notes                                   |
|----------------------|-------------|:---:|------------------------------------------|
| I2S DAC (e.g. PCM5102) | BCK       | 15  | |
|                      | LRCK        | 16  | |
|                      | DIN         | 17  | DAC's SCK pin -> GND (internal PLL); FLT/DEMP/FMT -> GND, XSMT -> 3V3 |
| microSD (SPI)        | CS          | 10  | |
|                      | SCK         | 12  | |
|                      | MISO        | 13  | |
|                      | MOSI        | 11  | |
| OLED (SH1106, I2C)   | SDA         | 8   | |
|                      | SCL         | 9   | address 0x3C by default |
| Rotary encoder        | A / B / SW  | 4 / 5 / 6 | EC11-style, with push button |
| Buttons (active-low, internal pull-up) | PLAY | 7  | |
|                      | SHIFT       | 18  | |
|                      | PAGE        | 21  | |
|                      | TRACK       | 38  | |
|                      | MUTE        | 39  | |
| Click/gate out (metronome) | -     | 40  | optional |
| MIDI OUT / IN         | TX / RX     | 41 / 42 | 5-pin DIN or TRS; opto-isolate the input |
| Status LED (optional) | -          | 47  | plain LED, not a NeoPixel — see config.h |

All of the above are `#define`s in `config.h` — remap freely to match your
wiring. The pin choices avoid the USB-CDC pins (19/20), the boot-strapping
pins (0/3/45/46) and the GPIO26-37 range some ESP32-S3-WROOM-1 modules
reserve for octal PSRAM/flash.

A board with PSRAM is strongly recommended: sample buffers (both streamed
and fully-RAM-resident) are allocated in PSRAM automatically when
`psramFound()` is true (see `setup()` in the .ino). Without PSRAM, samples
compete with the ~512 KB of internal SRAM AMY and the rest of the sketch
need, which will limit you to very few/short samples.

## Libraries

- **AMY** — this repo (installed as an Arduino library, or built from a
  sibling checkout).
- **U8g2** (Oliver Kraus) — the OLED driver.
- Everything else (SD, SPI, Wire, HardwareSerial) ships with the
  Arduino-ESP32 core.

Board: any "ESP32S3 Dev Module" board definition works; enable PSRAM in
Tools if your module has it.

## SD card layout

```
/patches/
  001_grand_piano/
    patch.cfg
    c2.wav
    c5.wav
  002_808_kit/
    patch.cfg
    kick.wav
    snare.wav
    hat_closed.wav
/patterns/        (created automatically — saved sequencer patterns)
/kits/            (created automatically — saved kits, see below)
/settings.bin     (created automatically — global settings)
```

Format the card FAT32. Each patch is one directory under `/patches/`
containing a `patch.cfg` text file plus its WAV samples (16-bit PCM, mono or
stereo — the same format AMY's own WAV parser expects). Directory names
become the patch's identity on the card; the display name comes from
`patch.cfg`.

### `patch.cfg` format

```
name=Grand Piano
category=Keys
poly=8
zone <noteLo> <noteHi> <velLo> <velHi> <root> <chromatic> <mode> <loop> <gain%> <pan%> <file>
```

- `noteLo`/`noteHi`, `velLo`/`velHi` — MIDI note (0-127) and velocity (0-127)
  ranges this zone covers.
- `root` — the sample's own pitch, as a MIDI note number.
- `chromatic` — `1` to pitch-shift the sample across the note range (a
  melodic instrument), `0` to always play it at `root` regardless of which
  key in the zone was hit (a drum pad).
- `mode` — `0` loads the sample fully into RAM at patch-load time
  (polyphonic, good for short/frequently-retriggered one-shots like drums);
  `1` streams it from the card a block at a time (good for long sustained
  samples, but effectively monophonic per zone — see below).
- `loop` — `1` to loop the whole sample while the note is held (RAM-mode
  zones only; AMY's streamed PCM presets don't support looping).
- `gain%` / `pan%` — per-zone level (100 = unity) and pan (-100..100).
- `file` — filename inside the patch's own directory. No spaces or commas.

Example — a simple two-zone piano:

```
name=Grand Piano
category=Keys
poly=8
zone 21 59 0 127 36 1 1 0 100 0 c2.wav
zone 60 108 0 127 72 1 1 0 100 0 c5.wav
```

Example — a one-shot drum kit (each pad is its own zone, `chromatic=0`):

```
name=808 Kit
category=Drums
poly=8
zone 36 36 0 127 36 0 0 0 100 0 kick.wav
zone 38 38 0 127 38 0 0 0 100 0 snare.wav
zone 42 42 0 127 42 0 0 0 90  0 hat_closed.wav
```

Load a patch onto the currently-selected track from **MENU > PATCH**.
Loading a patch unloads whatever that track had loaded before it (freeing
its AMY PCM presets), so a track never holds more sample data than its
current patch needs — exactly the "only load what's relevant to the current
patch" behavior this was built for.

**Streaming vs. RAM zones**: AMY's streamed (`mode=1`) PCM presets each own
exactly one file handle and read-ahead buffer, so a second overlapping
note-on for the *same zone* will steal it rather than playing polyphonically.
Use `mode=0` (RAM) for anything short and often-retriggered (drums,
plucks); reserve streaming for long pads/sustained notes you're unlikely to
re-trigger while the previous one is still ringing.

## Kits

A **kit** is a saved scene: which patch is loaded on each of the 4 tracks,
each track's gain/pan/tune, and the tempo — save one from **MENU > KITS**
(hold SHIFT and press to save into the highlighted slot), recall one with a
plain press. Incoming MIDI Program Change also recalls a kit (program number
= slot number).

## Tracks, mixing and per-step locks

Every one of the 4 sequencer tracks is its own AMY "synth" with its own
patch, polyphony, and MIDI channel (all four are independent rompler parts —
unlike the xtro firmware this was ported from, where only one of four tracks
drove the internal synth and the rest were MIDI-only lanes). **MENU > MIXER**
lists gain/pan/tune for every track; each can also be locked to a specific
step's value from **STEP EDIT > P-LOCKS**, the same mechanism xtro used to
automate its synth parameters per step.

## Building

Arduino IDE or `arduino-cli`, board "ESP32S3 Dev Module":

```
arduino-cli compile --fqbn esp32:esp32:esp32s3 \
  --build-property build.psram_type=opi \
  AMY_ESP32_S3_Rompler
arduino-cli upload --fqbn esp32:esp32:esp32s3 -p /dev/ttyACM0 AMY_ESP32_S3_Rompler
```

Install the AMY library (this repo) and U8g2 first, either via the Library
Manager or by symlinking this repo into your Arduino `libraries/` folder.

## Known limitations / follow-up ideas

- Streamed PCM zones are effectively monophonic per zone (see above) — an
  AMY core constraint, not something this example works around.
- No stereo sample playback yet (`wave=PCM` downmixes to mono); AMY supports
  true stereo via paired `PCM_LEFT`/`PCM_RIGHT` oscillators, which would be a
  reasonable follow-up.
- MIDI pitch bend is received but not yet wired to anything.
- No velocity-crossfade between overlapping velocity-layer zones — the first
  zone whose range covers the incoming (note, velocity) wins.
- The onboard help screen and MIXER/PATCH/KITS screens are new, hand-written
  UI for this project; everything else (step editor, pattern tools, chain,
  MIDI settings) is xtro's screens carried over close to verbatim.
