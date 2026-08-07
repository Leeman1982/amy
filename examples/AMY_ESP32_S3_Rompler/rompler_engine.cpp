#include "rompler_engine.h"
#include <AMY-Arduino.h>
#include "patch.h"
#include "rompler_params.h"

// What's actually sounding right now, keyed by (track, note), so a later
// note-off can reproduce the exact preset/pitch the note-on used — the patch
// (and therefore the zone->preset mapping) can change while a note still
// rings, and AMY's own voice-stealing keys a note-off to (synth, preset,
// midi_note) (src/patches.c: instrument_voice_for_note_event), not just the
// note number.
struct ActiveNote {
    bool    active = false;
    int16_t preset = -1;
    float   midiNote = 0.0f;
};
static ActiveNote g_active[NUM_TRACKS][128];

static void sendNoteOff(uint8_t track, ActiveNote& a) {
    amy_event e = amy_default_event();
    e.synth     = track;
    e.preset    = a.preset;
    e.midi_note = a.midiNote;
    e.velocity  = 0;
    amy_add_event(&e);
    a.active = false;
}

void romplerNoteOn(uint8_t track, uint8_t note, uint8_t velocity) {
    if (track >= NUM_TRACKS || note >= 128) return;
    const PatchZone* z = patchFindZone(track, note, velocity);
    if (!z) return;

    // Retrigger: if this exact key is already sounding, close it out first
    // rather than leaving an orphaned voice with no way back to note-off.
    if (g_active[track][note].active) sendNoteOff(track, g_active[track][note]);

    float tuneSemis = trackTuneCents(track) / 100.0f;
    float playedNote = (z->chromatic ? (float)note : (float)z->rootNote) + tuneSemis;

    amy_event e = amy_default_event();
    e.synth     = track;
    e.wave      = PCM;
    e.preset    = z->presetId;
    e.midi_note = playedNote;
    e.velocity  = ((float)velocity / 127.0f) * z->gain * trackGainMul(track);
    e.feedback  = z->loop ? 1.0f : 0.0f;   // pcm_note_on() reads this to decide whether to loop
    float pan = constrain(z->pan + trackPanBias(track), -1.0f, 1.0f);
    e.pan_coefs[COEF_CONST] = 0.5f + pan * 0.5f;
    amy_add_event(&e);

    g_active[track][note] = { true, z->presetId, playedNote };
}

void romplerNoteOff(uint8_t track, uint8_t note) {
    if (track >= NUM_TRACKS || note >= 128) return;
    ActiveNote& a = g_active[track][note];
    if (!a.active) return;
    sendNoteOff(track, a);
}

void romplerReleaseTrack(uint8_t track) {
    if (track >= NUM_TRACKS) return;
    for (uint8_t n = 0; n < 128; n++)
        if (g_active[track][n].active) sendNoteOff(track, g_active[track][n]);
}

void romplerAllOff() {
    for (uint8_t t = 0; t < NUM_TRACKS; t++) romplerReleaseTrack(t);
}

bool romplerLoadPatch(uint8_t track, const char* patchDirName) {
    if (track >= NUM_TRACKS) return false;
    romplerReleaseTrack(track);
    return patchLoad(track, patchDirName);
}
