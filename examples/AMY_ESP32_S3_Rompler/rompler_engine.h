#pragma once
#include <Arduino.h>
#include "config.h"

// ============================================================================
//  rompler_engine.h — turns (track, note, velocity) into AMY PCM events.
//
//  This is the glue xtro never needed (it drove its own oscillators directly)
//  and AMY doesn't provide on its own (its only built-in note->sample mapping
//  is the fixed GM drum table in src/patches.c): look up which keyzone of the
//  track's loaded patch covers this note/velocity (patch.cpp), then build and
//  send the amy_event that plays it, remembering enough to send a matching
//  note-off later.
// ============================================================================

// (Re)loads a patch onto a track, releasing whatever the track was sounding
// first. Thin wrapper around patchLoad() (patch.h) that also clears this
// engine's active-note bookkeeping for the track.
bool romplerLoadPatch(uint8_t track, const char* patchDirName);

void romplerNoteOn(uint8_t track, uint8_t note, uint8_t velocity);
void romplerNoteOff(uint8_t track, uint8_t note);

// Releases every note currently sounding on every track (panic / stop-all).
void romplerAllOff();

// Releases every note sounding on one track (used when its patch changes).
void romplerReleaseTrack(uint8_t track);
