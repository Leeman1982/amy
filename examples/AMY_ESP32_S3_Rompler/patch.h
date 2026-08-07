#pragma once
#include <Arduino.h>
#include "config.h"

// ============================================================================
//  patch.h — SD-card multisample "patch" banks.
//
//  This is the piece AMY does not provide: AMY's PCM oscillator (src/pcm.c)
//  plays exactly one sample per "preset number", pitch-shifted around a
//  single root note. There is no built-in keyzone/velocity-layer map beyond
//  a hardcoded GM drum table (src/patches.c). A rompler patch is a small,
//  human-editable directory on the SD card describing a set of zones (note
//  range [+ velocity range] -> sample file), which this module turns into
//  AMY PCM presets on demand.
//
//  SD layout:
//    /patches/<dirname>/patch.cfg
//    /patches/<dirname>/<sample1>.wav
//    /patches/<dirname>/<sample2>.wav
//    ...
//
//  patch.cfg (plain text, one directive per line, '#' comments allowed):
//    name=Grand Piano
//    category=Keys
//    poly=8
//    zone <noteLo> <noteHi> <velLo> <velHi> <root> <chromatic 0|1> <mode 0=ram|1=stream> <loop 0|1> <gain%> <pan%> <file>
//
//  Example:
//    name=808 Kit
//    category=Drums
//    poly=8
//    zone 36 36 0 127 36 0 0 0 100 0 kick.wav
//    zone 38 38 0 127 38 0 0 0 100 0 snare.wav
//    zone 42 42 0 127 42 0 0 0 90  0 hat_closed.wav
//
//  A "ram" zone is fully loaded into RAM at patch-load time (polyphonic —
//  good for short, frequently-retriggered one-shots like drums). A "stream"
//  zone is read from the card a block at a time as it plays (good for long
//  sustained samples) but AMY's streamed PCM presets are effectively
//  monophonic per zone: a second note-on for the same zone while the first
//  is still sounding will fight it over the shared file handle/buffer.
//  16-bit PCM WAV only, mono or stereo (matches AMY's own WAV parser).
// ============================================================================

struct PatchZone {
    uint8_t noteLo = 0, noteHi = 127;
    uint8_t velLo  = 0, velHi  = 127;
    uint8_t rootNote  = 60;
    bool    chromatic = true;    // true: pitch-shifts with the note played;
                                  // false: always plays at rootNote (drum pad style)
    bool    streaming = true;    // true: AMY_PCM_TYPE_FILE; false: AMY_PCM_TYPE_MEMORY
    bool    loop      = false;   // whole-sample loop (RAM zones only)
    float   gain = 1.0f;         // linear, 0..2 (100% = 1.0)
    float   pan  = 0.0f;         // -1..1
    int16_t presetId = -1;       // AMY PCM preset number once loaded, -1 = unloaded
    char    file[40] = {0};      // filename inside the patch directory
};

struct PatchInfo {
    char    dirName[40] = {0};   // directory name under PATCH_DIR
    char    name[24]    = {0};   // display name, from "name="
    char    category[16] = {0};
    uint8_t polyphony = TRACK_POLYPHONY;
};

struct TrackPatch {
    PatchInfo info;
    PatchZone zones[MAX_ZONES_PER_PATCH];
    uint8_t   numZones = 0;
    bool      loaded = false;
};

// Scans PATCH_DIR for patch directories, filling `list` with up to `max`
// entries (name/category read from each patch.cfg). Returns the count found.
uint8_t patchScan(PatchInfo* list, uint8_t max);

// Loads the patch at `dirName` onto `track`: unloads whatever that track had
// loaded first, parses patch.cfg, and registers every zone's sample with AMY
// (streamed or fully in RAM per-zone). Returns true if at least one zone
// loaded successfully.
bool patchLoad(uint8_t track, const char* dirName);

// Frees every AMY PCM preset the track's current patch holds.
void patchUnload(uint8_t track);

// Finds the zone that should sound for (note, velocity) on a track, or
// nullptr if none of its zones cover that key/velocity.
const PatchZone* patchFindZone(uint8_t track, uint8_t note, uint8_t velocity);

TrackPatch& patchForTrack(uint8_t track);
