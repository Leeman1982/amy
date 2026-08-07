#pragma once
#include <Arduino.h>
#include <AMY-Arduino.h>
#include "config.h"
#include "sequencer.h"
#include "rompler_params.h"

// Points amy_config_t's file hooks at the SD card. Call BEFORE amy_start() —
// see sd_storage.cpp for why.
void sdInstallAmyFileHooks(amy_config_t& cfg);

// ============================================================================
//  sd_storage.h — everything that lives on the microSD card.
//
//  Two very different jobs share one card:
//   1. Bulk sample data, streamed/loaded on demand by patch.cpp through AMY's
//      own file hooks (amy_external_f{open,read,seek,close}_hook — see
//      src/transfer.c's posix_external_*_hook for the reference shape this
//      mirrors).
//   2. Small persisted state — patterns, global settings, kits — the same
//      "read/write one struct in one fopen() call" shape as xtro's FatFS
//      Storage class, just against SD.h/the SD VFS mount instead of FatFS on
//      internal flash.
// ============================================================================

struct TrackState {
    char    patchDir[40] = {0};              // "" = no patch loaded
    uint8_t mute = 0;
    uint8_t lastPattern = 0;
    uint8_t params[NUM_ROMPLER_PARAMS] = {128, 128, 128};   // gain/pan/tune, see rompler_params.h
    uint8_t chainLen = 0;
    ChainEntry chain[CHAIN_LEN];
};

struct GlobalSettings {
    uint16_t magic         = STORAGE_MAGIC;
    uint16_t version       = STORAGE_VERSION;
    uint16_t bpm           = BPM_DEFAULT;
    uint8_t  lastTrack     = 0;
    uint8_t  midiClockOut  = 0;
    uint8_t  extSync       = 0;
    uint8_t  midiInChannel = 0;      // 0 = omni
    uint8_t  midiOutEnable = 1;
    uint8_t  midiThru      = 0;
    uint8_t  contrast      = 200;
    uint8_t  metronome     = 0;
    uint8_t  masterVol     = 128;    // 0..255 -> 0..2.0x linear, 128 = unity
    TrackState track[NUM_TRACKS];
};

// A kit is a "scene": which patch is loaded on every track, each track's
// mixer settings, and the tempo — the sample-playback equivalent of xtro's
// UserPreset (which snapshotted its one synth's parameters).
struct Kit {
    char     name[14] = {0};
    uint16_t bpm = BPM_DEFAULT;
    char     patchDir[NUM_TRACKS][40] = {{0}};
    uint8_t  params[NUM_TRACKS][NUM_ROMPLER_PARAMS] = {{0}};
};

class SdStorage {
public:
    bool begin();                    // mount SD, register AMY file hooks, mkdir layout
    bool mounted() const { return _mounted; }

    bool savePattern(uint8_t track, uint8_t idx, const Pattern& p);
    bool loadPattern(uint8_t track, uint8_t idx, Pattern& p);
    bool patternExists(uint8_t track, uint8_t idx);

    bool saveSettings(const GlobalSettings& s);
    bool loadSettings(GlobalSettings& s);

    bool saveKit(uint8_t slot, const Kit& k);
    bool loadKit(uint8_t slot, Kit& k);
    bool kitExists(uint8_t slot);

private:
    bool _mounted = false;
    static void patternPath(uint8_t track, uint8_t idx, char* buf, size_t n);
    static void kitPath(uint8_t slot, char* buf, size_t n);
};
