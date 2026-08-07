/*
 * AMY ESP32-S3 Rompler
 * ───────────────────────────────────────────────────────────────────────────
 *  Target    : ESP32-S3-WROOM(-1) dev board (Arduino-ESP32 core)
 *  Sound     : AMY (github.com/shorepine/amy) PCM sample playback — every
 *              track is its own multitimbral AMY "synth" driven by a
 *              multisample "patch" streamed/loaded from a microSD card.
 *  UI/Seq    : ported from xtro/UltimateSynthNYR (github repo "xtro") — the
 *              step sequencer (sequencer.h/.cpp) and menu-driven OLED UI
 *              (ui.h/.cpp) are the same engine, retargeted from xtro's own
 *              virtual-analog voices onto AMY sample playback.
 *  Display   : 128x64 OLED, SH1106 controller, hardware I2C.
 *  Storage   : microSD over SPI — patches and samples are read on demand
 *              (patch.cpp / sd_storage.cpp), never all held in RAM at once.
 *
 *  See config.h for the full pinout and README.md for wiring + the SD card
 *  patch-bank format.
 *
 *  Architecture: AMY owns its own render task(s) (platform.multithread/
 *  multicore, see setup() below) and writes straight to I2S — there is no
 *  hand-rolled second core/render loop here the way xtro needed on RP2350;
 *  loop() just has to keep calling amy_update() and servicing the UI/
 *  sequencer/MIDI at a reasonable rate.
 */

#include <AMY-Arduino.h>
#include <esp_heap_caps.h>   // MALLOC_CAP_SPIRAM
#include "config.h"
#include "sequencer.h"
#include "controls.h"
#include "midi.h"
#include "sd_storage.h"
#include "patch.h"
#include "rompler_params.h"
#include "rompler_engine.h"
#include "ui.h"
#include "scales.h"

// ═══════════════════════════════════════════════════════════════════════════
//  Globals
// ═══════════════════════════════════════════════════════════════════════════
Sequencer  seqs[NUM_TRACKS];
MidiIO     midi;
SdStorage  storage;
Controls   controls;
UI         ui;
GlobalSettings gs;

uint8_t activeTrack = 0;
bool    g_recArm = false;
float   g_masterVol = 1.0f;

PatchInfo g_patchList[MAX_PATCHES];
uint8_t   g_patchCount = 0;

// ═══════════════════════════════════════════════════════════════════════════
//  Parameter locks — momentary: the first step to lock a (track, param)
//  records what it was, and everything is put back when the transport stops,
//  so a locked pattern never silently rewrites a saved kit.
// ═══════════════════════════════════════════════════════════════════════════
static float plockOrig[NUM_TRACKS][NUM_ROMPLER_PARAMS];
static bool  plockHeld[NUM_TRACKS][NUM_ROMPLER_PARAMS];

static void plockApply(uint8_t track, uint8_t id, uint8_t value) {
    if (track >= NUM_TRACKS || id >= NUM_ROMPLER_PARAMS) return;
    if (!plockHeld[track][id]) { plockOrig[track][id] = getTrackParam(track, id); plockHeld[track][id] = true; }
    setTrackParam(track, id, (float)value * (1.0f / 255.0f));
}
static void plockRestoreAll() {
    for (uint8_t t = 0; t < NUM_TRACKS; t++)
        for (uint8_t i = 0; i < NUM_ROMPLER_PARAMS; i++)
            if (plockHeld[t][i]) { setTrackParam(t, i, plockOrig[t][i]); plockHeld[t][i] = false; }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Sequencer routing — every track drives its own AMY synth AND echoes to
//  MIDI OUT on its pattern's channel (unlike xtro, where only track 0 was
//  "internal" and 1..3 were MIDI-only).
// ═══════════════════════════════════════════════════════════════════════════
static void onSeqNoteOn(uint8_t track, uint8_t chan, uint8_t note, uint8_t vel, bool accent, bool slide) {
    (void)accent; (void)slide;
    midi.noteOn(chan, note, vel);
    romplerNoteOn(track, note, vel);
}
static void onSeqNoteOff(uint8_t track, uint8_t chan, uint8_t note) {
    midi.noteOff(chan, note);
    romplerNoteOff(track, note);
}
void seqEmitClock()    { midi.clock(); }
void seqEmitStart()    { midi.start(); }
void seqEmitStop()     { midi.stop(); }
void seqEmitContinue() { midi.continueMsg(); }

// ═══════════════════════════════════════════════════════════════════════════
//  MIDI in — an incoming note plays every track whose pattern is currently
//  set to that MIDI channel (multitimbral: several tracks can share a
//  channel, though normally each has its own).
// ═══════════════════════════════════════════════════════════════════════════
static void onMidiNoteOn(uint8_t ch, uint8_t note, uint8_t vel) {
    if (gs.midiThru) midi.noteOn(ch, note, vel);
    for (uint8_t t = 0; t < NUM_TRACKS; t++)
        if (seqs[t].midiChannel() == ch) romplerNoteOn(t, note, vel);
    if (g_recArm) seqs[activeTrack].recordNote(note, vel);
}
static void onMidiNoteOff(uint8_t ch, uint8_t note, uint8_t vel) {
    (void)vel;
    if (gs.midiThru) midi.noteOff(ch, note);
    for (uint8_t t = 0; t < NUM_TRACKS; t++)
        if (seqs[t].midiChannel() == ch) romplerNoteOff(t, note);
}
static void onMidiCC(uint8_t ch, uint8_t cc, uint8_t val) {
    if (cc == 123 || cc == 120) { romplerAllOff(); return; }
    float n = (float)val * (1.0f / 127.0f);
    for (uint8_t t = 0; t < NUM_TRACKS; t++) {
        if (seqs[t].midiChannel() != ch) continue;
        for (uint8_t i = 0; i < NUM_ROMPLER_PARAMS; i++)
            if (ROMPLER_PARAM_INFO[i].cc == cc && cc != 0) { setTrackParam(t, i, n); ui.markDirty(); }
    }
}
static void onMidiPC(uint8_t ch, uint8_t prog) {
    (void)ch;
    // Program change recalls a kit — the sample-playback equivalent of
    // switching a synth patch bank.
    extern void uiLoadKit(uint8_t slot);
    if (prog < KIT_SLOTS) uiLoadKit(prog);
}
static void onMidiBend(uint8_t ch, int16_t bend) { (void)ch; (void)bend; }  // not wired up in this pass
static void onMidiRealtime(uint8_t status) {
    switch (status) {
    case MIDI_RT_CLOCK:    for (uint8_t t = 0; t < NUM_TRACKS; t++) seqs[t].extTick();     break;
    case MIDI_RT_START:    for (uint8_t t = 0; t < NUM_TRACKS; t++) seqs[t].extStart();    break;
    case MIDI_RT_CONTINUE: for (uint8_t t = 0; t < NUM_TRACKS; t++) seqs[t].extContinue(); break;
    case MIDI_RT_STOP:     for (uint8_t t = 0; t < NUM_TRACKS; t++) seqs[t].extStop();     break;
    default: break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  UI hooks
// ═══════════════════════════════════════════════════════════════════════════
void uiApplySync() {
    for (uint8_t t = 0; t < NUM_TRACKS; t++) {
        seqs[t].setExternalSync(gs.extSync != 0);
        // Only track 0 emits clock: four tracks each sending 24 PPQN would be
        // 96 PPQN of nonsense, and clock out is suppressed entirely while
        // slaved so two boxes can't fight over the tempo.
        seqs[t].setMidiClock(gs.midiClockOut && !gs.extSync && t == 0);
    }
}

void uiTogglePlayAll() {
    bool anyPlaying = false;
    for (uint8_t t = 0; t < NUM_TRACKS; t++)
        if (seqs[t].playState() == PlayState::PLAYING) anyPlaying = true;
    if (anyPlaying) {
        for (uint8_t t = 0; t < NUM_TRACKS; t++) seqs[t].stop();
        plockRestoreAll();
        romplerAllOff();
    } else {
        unsigned long t0 = micros();       // one timestamp so the lanes stay locked
        for (uint8_t t = 0; t < NUM_TRACKS; t++) seqs[t].play(t0);
    }
}

void uiStopAll() {
    for (uint8_t t = 0; t < NUM_TRACKS; t++) seqs[t].stop();
    plockRestoreAll();
    romplerAllOff();
}

void uiPanic() {
    romplerAllOff();
    for (uint8_t ch = 1; ch <= 16; ch++) midi.allNotesOff(ch);
}

void uiRescanPatches() {
    g_patchCount = patchScan(g_patchList, MAX_PATCHES);
}

void uiLoadKit(uint8_t slot) {
    Kit k;
    if (!storage.loadKit(slot, k)) { ui.toast("EMPTY SLOT"); return; }
    for (uint8_t t = 0; t < NUM_TRACKS; t++) {
        if (k.patchDir[t][0]) romplerLoadPatch(t, k.patchDir[t]);
        restoreTrackParams(t, k.params[t]);
    }
    for (uint8_t t = 0; t < NUM_TRACKS; t++) seqs[t].setBPM(k.bpm);
    for (uint8_t t = 0; t < NUM_TRACKS; t++) plockHeld[t][0] = plockHeld[t][1] = plockHeld[t][2] = false;
    ui.toast(k.name);
}

void uiSaveKit(uint8_t slot, const char* name) {
    Kit k;
    strncpy(k.name, name, sizeof(k.name) - 1);
    k.bpm = seqs[0].bpm();
    for (uint8_t t = 0; t < NUM_TRACKS; t++) {
        TrackPatch& tp = patchForTrack(t);
        if (tp.loaded) strncpy(k.patchDir[t], tp.info.dirName, sizeof(k.patchDir[t]) - 1);
        else k.patchDir[t][0] = 0;
        snapshotTrackParams(t, k.params[t]);
    }
    if (!storage.saveKit(slot, k)) ui.toast("SAVE FAILED");
}

// Auditions the note being dialled in on the step editor / patch browser.
// Released by the housekeeping timer in loop().
static unsigned long g_previewOffAt = 0;
static int g_lastPreviewNote = -1;

void uiPreviewNote(uint8_t note, bool on) {
    if (!on) {
        if (g_lastPreviewNote >= 0) { romplerNoteOff(activeTrack, (uint8_t)g_lastPreviewNote); g_lastPreviewNote = -1; }
        return;
    }
    if (g_lastPreviewNote >= 0) romplerNoteOff(activeTrack, (uint8_t)g_lastPreviewNote);
    romplerNoteOn(activeTrack, note, 100);
    g_lastPreviewNote = note;
    g_previewOffAt = millis() + 220;
}

void uiSaveAll() {
    gs.bpm       = seqs[0].bpm();
    gs.lastTrack = activeTrack;
    gs.masterVol = (uint8_t)constrain(g_masterVol * 128.0f, 0.0f, 255.0f);
    for (uint8_t t = 0; t < NUM_TRACKS; t++) {
        TrackState& ts = gs.track[t];
        ts.lastPattern = seqs[t].currentPattern();
        ts.mute        = seqs[t].muted() ? 1 : 0;
        TrackPatch& tp = patchForTrack(t);
        if (tp.loaded) strncpy(ts.patchDir, tp.info.dirName, sizeof(ts.patchDir) - 1);
        else ts.patchDir[0] = 0;
        snapshotTrackParams(t, ts.params);
        for (uint8_t p = 0; p < NUM_PATTERNS; p++)
            storage.savePattern(t, p, seqs[t].getPattern(p));
    }
    storage.saveSettings(gs);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Demo pattern — a simple 16-step idea per track so a first boot with no
//  saved patterns isn't just a silent grid, whatever patch ends up loaded.
// ═══════════════════════════════════════════════════════════════════════════
static void initDemoPattern(Sequencer& s, uint8_t track) {
    Pattern& p = s.getPattern(0);
    p.rootNote = 0;                 // C
    p.scaleIdx = 9;                 // pentatonic minor
    p.midiChannel = track + 1;
    p.length = DEFAULT_STEPS;

    static const uint8_t BASS[] = {36,36,48,36,39,36,43,36,36,48,36,41,36,39,36,34};
    static const uint8_t LEAD[] = {60,63,67,63,70,67,63,60,72,70,67,63,60,63,67,70};
    static const uint8_t ARP [] = {72,75,79,84,79,75,72,67,72,75,79,84,79,75,72,67};
    const uint8_t* src = (track == 0) ? BASS : (track == 2) ? ARP : LEAD;

    for (uint8_t i = 0; i < DEFAULT_STEPS; i++) {
        Step& st = p.steps[i];
        st.notes[0]    = src[i];
        st.noteCount   = 1;
        st.velocity    = (i % 4 == 0) ? 112 : 82;
        st.gate        = (i % 4 == 0) ? 80 : 55;
        st.probability = 100;
        st.ratchet     = 1;
        st.micro       = 0;
        st.flags       = (i % 8 == 0) ? ST_ACCENT : 0;
        bool on = (track == 0) ? true
                : (track == 1) ? (i % 2 == 0)
                : (track == 2) ? (i % 4 == 0) : false;
        if (on) st.flags |= ST_ACTIVE;
    }
}

// Blinks the status LED at ~1 Hz so a glance confirms the sketch is alive,
// independent of anything the OLED is doing.
static void statusLedTick() {
#if HAS_STATUS_LED
    static unsigned long lastToggle = 0;
    static bool on = false;
    unsigned long now = millis();
    if (now - lastToggle > 500) { lastToggle = now; on = !on; digitalWrite(PIN_STATUS_LED, on ? HIGH : LOW); }
#endif
}

// A 6 ms gate pulse on PIN_CLICK_OUT at the top of each beat, referenced to
// track 0's transport. Cheap, silent in the audio path, drives an LED or a
// click-track input directly.
static void metronomeTick() {
    static unsigned long pulseUntil = 0;
    static uint8_t lastStep = 255;
    if (!gs.metronome) { digitalWrite(PIN_CLICK_OUT, LOW); return; }
    Sequencer& s = seqs[0];
    if (s.playState() == PlayState::PLAYING) {
        uint8_t st = s.currentStep();
        if (st != lastStep) {
            lastStep = st;
            if ((st & 3) == 0) { digitalWrite(PIN_CLICK_OUT, HIGH); pulseUntil = millis() + 6; }
        }
    }
    if (pulseUntil && millis() > pulseUntil) { digitalWrite(PIN_CLICK_OUT, LOW); pulseUntil = 0; }
}

// ═══════════════════════════════════════════════════════════════════════════
//  setup / loop
// ═══════════════════════════════════════════════════════════════════════════
void setup() {
    // ── 1. DISPLAY FIRST ───────────────────────────────────────────────────
    // Before Serial, before AMY, before storage — see xtro's UI::beginDisplay
    // for why: a stall anywhere later still leaves a readable screen behind.
    ui.beginDisplay();

    Serial.begin(115200);
    randomSeed((uint32_t)esp_random());

    pinMode(PIN_CLICK_OUT, OUTPUT);
    digitalWrite(PIN_CLICK_OUT, LOW);
#if HAS_STATUS_LED
    pinMode(PIN_STATUS_LED, OUTPUT);
#endif

    romplerParamsInit();

    // ── 2. Storage + patch bank ───────────────────────────────────────────
    ui.bootStatus("SD CARD");
    bool sdOk = storage.begin();
    if (!sdOk) {
        Serial.println("[WARN] no SD card - check wiring/format (FAT32)");
        ui.bootStatus("NO SD CARD");
        delay(1200);
    }
    ui.bootStatus("SCANNING PATCHES");
    uiRescanPatches();

    // ── 3. AMY ─────────────────────────────────────────────────────────────
    ui.bootStatus("AMY ENGINE");
    amy_config_t amy_config = amy_default_config();
    amy_config.features.default_synths = 0;   // we define our own per-track synths
    amy_config.features.startup_bleep  = 0;
    amy_config.platform.multithread = 1;      // render on its own FreeRTOS task
    amy_config.platform.multicore   = 1;      // ...pinned to the second core
    amy_config.audio = AMY_AUDIO_IS_I2S;
    amy_config.i2s_mclk = PIN_I2S_MCLK;
    amy_config.i2s_bclk = PIN_I2S_BCLK;
    amy_config.i2s_lrc  = PIN_I2S_LRC;
    amy_config.i2s_dout = PIN_I2S_DOUT;
    if (psramFound()) {
        // Sample buffers (streamed and RAM-resident PCM presets both) are the
        // biggest allocations in this whole sketch — push them into PSRAM so
        // the ~512 KB of internal SRAM stays free for AMY's own working set
        // and everything else in the sketch.
        amy_config.ram_caps_sample = MALLOC_CAP_SPIRAM;
        amy_config.ram_caps_synth  = MALLOC_CAP_SPIRAM;
    }
    sdInstallAmyFileHooks(amy_config);
    amy_start(amy_config);

    // ── 4. Sequencer ───────────────────────────────────────────────────────
    ui.bootStatus("SEQUENCER");
    seqNoteOn  = onSeqNoteOn;
    seqNoteOff = onSeqNoteOff;
    seqLock    = plockApply;
    for (uint8_t t = 0; t < NUM_TRACKS; t++) seqs[t].begin(t);

    bool haveSettings = sdOk && storage.loadSettings(gs);
    if (haveSettings) {
        for (uint8_t t = 0; t < NUM_TRACKS; t++) seqs[t].setBPM(gs.bpm);
        activeTrack  = gs.lastTrack % NUM_TRACKS;
        g_masterVol  = (float)gs.masterVol / 128.0f;
        for (uint8_t t = 0; t < NUM_TRACKS; t++) {
            if (gs.track[t].patchDir[0]) romplerLoadPatch(t, gs.track[t].patchDir);
            restoreTrackParams(t, gs.track[t].params);
        }
    } else {
        for (uint8_t t = 0; t < NUM_TRACKS; t++)
            for (uint8_t i = 0; i < CHAIN_LEN; i++) gs.track[t].chain[i].patternIdx = -1;
    }

    for (uint8_t t = 0; t < NUM_TRACKS; t++) {
        bool loadedAny = false;
        for (uint8_t p = 0; p < NUM_PATTERNS; p++) {
            Pattern tmp;
            if (storage.loadPattern(t, p, tmp)) { seqs[t].getPattern(p) = tmp; loadedAny = true; }
        }
        if (!loadedAny) initDemoPattern(seqs[t], t);
        if (haveSettings && gs.track[t].lastPattern < NUM_PATTERNS) seqs[t].setPattern(gs.track[t].lastPattern);
        seqs[t].setMuted(gs.track[t].mute != 0);
    }

    // ── 5. MIDI ────────────────────────────────────────────────────────────
    ui.bootStatus("MIDI");
    midi.begin();
    midi.setInChannel(gs.midiInChannel);
    midi.setOutEnabled(gs.midiOutEnable != 0);
    midi.setNoteOn(onMidiNoteOn);
    midi.setNoteOff(onMidiNoteOff);
    midi.setCC(onMidiCC);
    midi.setPC(onMidiPC);
    midi.setBend(onMidiBend);
    midi.setRealtime(onMidiRealtime);
    uiApplySync();

    controls.begin();
    ui.bind(seqs, &activeTrack, &storage, &gs);
    ui.applyContrast(gs.contrast);

    ui.bootStatus(sdOk ? "READY" : "READY (NO SD)");
    delay(700);
    ui.markDirty();
    Serial.printf("[AMY Rompler] ready - %u patches found, MIDI OUT on GPIO%d, SD %s\n",
                  (unsigned)g_patchCount, PIN_MIDI_TX, sdOk ? "ok" : "absent");
}

void loop() {
    midi.poll();
    for (uint8_t t = 0; t < NUM_TRACKS; t++) seqs[t].update();

    controls.update();
    ui.setShift(controls.shift.isHeld());

    int d = controls.encoder.getDelta();
    if (d != 0) ui.onEncoder(d);
    if (controls.encoder.wasPressed())   ui.onEncPress();
    if (controls.encoder.wasLongPress()) ui.onEncLong();

    if (controls.play.wasPressed())    ui.onPlay();
    if (controls.play.wasLongPress())  ui.onPlayLong();
    if (controls.page.wasPressed())    ui.onPage();
    if (controls.page.wasLongPress())  ui.onPageLong();
    if (controls.track.wasPressed())   ui.onTrack();
    if (controls.track.wasLongPress()) ui.onTrackLong();
    if (controls.rec.wasPressed())     ui.onMute();
    if (controls.rec.wasLongPress())   ui.onMuteLong();
    controls.shift.wasPressed();       // consumed; SHIFT is a hold, not a press

    if (g_previewOffAt && millis() > g_previewOffAt) {
        uiPreviewNote(0, false);
        g_previewOffAt = 0;
    }

    metronomeTick();
    statusLedTick();

    midi.poll();          // drain before the blocking OLED transfer
    ui.update();
    midi.poll();           // ...and catch up straight after it

    // Drives AMY's message-queue/event-timing housekeeping every pass; actual
    // rendering happens on AMY's own background task(s) (platform.multithread
    // / multicore, set in setup()), so this call is cheap and must not be
    // skipped even though this loop() never touches the returned buffer.
    amy_update();
}
