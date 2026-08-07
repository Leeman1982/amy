#pragma once
#include <Arduino.h>
#include "config.h"

// ============================================================================
//  rompler_params.h — per-track mixer parameters.
//
//  xtro's synth_params.h was "the single source of truth" for ~70 oscillator/
//  filter/envelope/LFO parameters shared by the UI editor, MIDI CC, presets
//  and step parameter-locks. A rompler has no oscillator to shape — AMY's PCM
//  engine plays the sample as recorded — so there is no equivalent synth-edit
//  page here. What each track DOES need shaping is its place in the mix:
//  level, pan and fine tune. This is the same "one table serves every
//  consumer" pattern, just scoped to that.
// ============================================================================

enum RomplerParamId : uint8_t {
    RP_GAIN = 0,    // 0..1 -> 0..200% (0.5 = unity)
    RP_PAN,         // 0..1 -> -100..+100 (0.5 = center)
    RP_TUNE,        // 0..1 -> -1200..+1200 cents (0.5 = no tune)
    NUM_ROMPLER_PARAMS
};

struct RomplerParamInfo {
    const char* name;   // <= 9 chars, fits the OLED value column
    uint8_t     cc;     // MIDI CC number, 0 = none
};
extern const RomplerParamInfo ROMPLER_PARAM_INFO[NUM_ROMPLER_PARAMS];

void  romplerParamsInit();
void  setTrackParam(uint8_t track, uint8_t id, float value01);
float getTrackParam(uint8_t track, uint8_t id);
void  formatTrackParam(uint8_t track, uint8_t id, char* buf, size_t n);
bool  paramIsLockable(uint8_t id);   // all of them, but keeps the xtro naming/shape

// Decoded, engine-ready helpers used by rompler_engine.cpp.
float trackGainMul(uint8_t track);     // 0..2 linear amplitude multiplier
float trackPanBias(uint8_t track);     // -1..+1
float trackTuneCents(uint8_t track);   // -1200..+1200

// Snapshot/restore for kit save/load (sd_storage.cpp), same shape as xtro's
// UserPreset::params — one byte (0..255) per parameter per track.
void snapshotTrackParams(uint8_t track, uint8_t* out /* NUM_ROMPLER_PARAMS bytes */);
void restoreTrackParams(uint8_t track, const uint8_t* in /* NUM_ROMPLER_PARAMS bytes */);
