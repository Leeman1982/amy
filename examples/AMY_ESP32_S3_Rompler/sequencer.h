#pragma once
#include <Arduino.h>
#include "config.h"
#include "scales.h"

// ============================================================================
//  sequencer.h — one 64-step lane. Ported unchanged (in logic) from
//  xtro/sequencer.h/.cpp: this class never touched the RP2350's synth or UART
//  directly, only the two callback typedefs below — which is exactly the seam
//  that lets it drive AMY sample-playback events here instead of xtro's
//  virtual-analog voices, with zero changes to the timing/pattern engine
//  itself.
//
//  Every track here drives its own AMY synth (its own loaded rompler patch)
//  and can also echo to MIDI OUT on its own channel — unlike xtro, where only
//  track 0 was "internal" and 1..3 were MIDI-only lanes.
//
//  Everything a step can carry is editable from the front panel; MIDI CC is
//  not required to reach any of it.
// ============================================================================

// ─── Step flags ─────────────────────────────────────────────────────────────
#define ST_ACTIVE  0x01
#define ST_ACCENT  0x02
#define ST_SLIDE   0x04
#define ST_TIE     0x08     // hold through the next step (no note-off)

#define LOCK_NONE  0xFF

struct Step {
    uint8_t notes[STEP_MAX_NOTES] = {60, 0, 0, 0};
    uint8_t noteCount   = 1;    // 1..STEP_MAX_NOTES — chords per step
    uint8_t velocity    = 100;  // 0-127
    uint8_t gate        = 75;   // % of the step interval
    uint8_t probability = 100;  // % chance of firing
    int8_t  micro       = 0;    // -50..+50 % of a step, timing nudge
    uint8_t ratchet     = 1;    // 1..8 retriggers inside the step
    uint8_t flags       = 0;
    uint8_t lockId[STEP_LOCKS]  = {LOCK_NONE, LOCK_NONE, LOCK_NONE, LOCK_NONE};
    uint8_t lockVal[STEP_LOCKS] = {0, 0, 0, 0};

    inline bool active() const { return flags & ST_ACTIVE; }
    inline bool accent() const { return flags & ST_ACCENT; }
    inline bool slide()  const { return flags & ST_SLIDE;  }
    inline bool tie()    const { return flags & ST_TIE;    }
};

// ─── Rhythm subdivisions ────────────────────────────────────────────────────
// Step duration relative to a 16th = num/den. Every entry divides 6 ticks of
// 24 PPQN exactly, so external sync lands on the grid with no rounding drift.
struct Subdiv { const char name[6]; uint8_t num, den; };
static const Subdiv SUBDIVS[] = {
    { "1/16",  1, 1 },
    { "1/8",   2, 1 },
    { "1/4",   4, 1 },
    { "1/2",   8, 1 },
    { "1/32",  1, 2 },
    { "1/8T",  4, 3 },
    { "1/16T", 2, 3 },
    { "1/4.",  6, 1 },
    { "1/8.",  3, 1 },
};
static const uint8_t NUM_SUBDIVS = sizeof(SUBDIVS) / sizeof(SUBDIVS[0]);

struct Pattern {
    Step    steps[NUM_STEPS];
    uint8_t length      = DEFAULT_STEPS;
    uint8_t rootNote    = 0;      // 0 = C
    uint8_t scaleIdx    = 0;      // index into SCALES[]
    uint8_t midiChannel = 1;
    uint8_t swing       = 0;      // 0..SWING_MAX %
    uint8_t subdiv      = 0;
    int8_t  transpose   = 0;      // semitones, applied on output
};

enum class PlayState : uint8_t { STOPPED, PLAYING, PAUSED };

struct ChainEntry {
    int8_t  patternIdx = -1;      // -1 = end of chain
    uint8_t repeats    = 1;
};

// ─── Output hooks ───────────────────────────────────────────────────────────
// The sequencer never touches AMY or the UART directly; the sketch owns that
// routing (rompler_engine.cpp), which is what lets every track be an
// independently-patched rompler part without the sequencer knowing anything
// about AMY.
typedef void (*SeqNoteOnFn)(uint8_t track, uint8_t chan, uint8_t note, uint8_t vel, bool accent, bool slide);
typedef void (*SeqNoteOffFn)(uint8_t track, uint8_t chan, uint8_t note);
typedef void (*SeqLockFn)(uint8_t track, uint8_t param, uint8_t value);
extern SeqNoteOnFn  seqNoteOn;
extern SeqNoteOffFn seqNoteOff;
extern SeqLockFn    seqLock;

// MIDI real-time emission, implemented in the sketch. Only the track with
// setMidiClock(true) calls these — four tracks each sending 24 PPQN would be
// 96 PPQN of nonsense on the wire.
void seqEmitClock();
void seqEmitStart();
void seqEmitStop();
void seqEmitContinue();

class Sequencer {
public:
    void begin(uint8_t trackIdx);

    // ── Transport ──────────────────────────────────────────────────────────
    // startUs lets several tracks be started from one timestamp so they stay
    // phase-locked (0 = "now").
    void play(unsigned long startUs = 0);
    void stop();
    void pause();
    void togglePlay();
    PlayState playState() const { return _playState; }

    void update();                             // call from loop() as often as possible

    // ── Tempo ──────────────────────────────────────────────────────────────
    void     setBPM(uint16_t bpm);
    uint16_t bpm() const { return _bpm; }
    void     nudgeBPM(int delta) { setBPM((int)_bpm + delta); }
    void     refreshTiming()     { computeInterval(); }
    unsigned long stepIntervalUs() const { return _stepIntervalUs; }

    // ── MIDI clock ─────────────────────────────────────────────────────────
    // Re-arm the tick schedule as well: leaving _nextClockUs stale from an
    // earlier run makes the bounded catch-up loop dump eight clock bytes per
    // pass until it works its way back to the present.
    void setMidiClock(bool en) { _midiClock = en; _nextClockUs = micros(); }
    bool midiClockOut() const  { return _midiClock; }
    void setExternalSync(bool en);
    bool externalSync() const  { return _extSync; }
    void extTick();
    void extStart();
    void extContinue();
    void extStop();

    // ── State ──────────────────────────────────────────────────────────────
    uint8_t  currentStep()    const { return _step; }
    uint8_t  currentPattern() const { return _patIdx; }
    uint16_t stepProgress()   const;           // 0..999 through the current step
    uint8_t  track()          const { return _track; }

    void setMuted(bool m);
    bool muted() const { return _muted; }

    // ── Pattern access ─────────────────────────────────────────────────────
    void     setPattern(uint8_t idx);
    Pattern& getPattern(uint8_t idx)   { return _patterns[idx % NUM_PATTERNS]; }
    Pattern& currentPatternRef()       { return _patterns[_patIdx]; }
    Step&    getStep(uint8_t s)        { return _patterns[_patIdx].steps[s % NUM_STEPS]; }
    uint8_t  midiChannel() const       { return _patterns[_patIdx].midiChannel; }

    void clearPattern(uint8_t idx);
    void copyPattern(uint8_t src, uint8_t dst);
    void quantizePattern(uint8_t idx);
    void randomisePattern(uint8_t idx, uint8_t density);
    void euclidFill(uint8_t idx, uint8_t hits, uint8_t rotate);
    void humanise(uint8_t idx, uint8_t amount);

    // ── Chain ──────────────────────────────────────────────────────────────
    void setChain(const ChainEntry* c, uint8_t len);
    void clearChain() { _chainLen = 0; }
    bool isChaining() const { return _chainLen > 0; }
    const ChainEntry* chain() const { return _chain; }
    uint8_t chainLen() const { return _chainLen; }

    // ── Live record ────────────────────────────────────────────────────────
    // Notes arriving while armed are written into the step nearest the
    // playhead, so you can play a part in rather than dial it in.
    void setRecord(bool on) { _recording = on; }
    bool recording() const  { return _recording; }
    void recordNote(uint8_t note, uint8_t vel);

    // Audition a step's notes without moving the transport (UI preview).
    void auditionStep(uint8_t s);

private:
    uint8_t   _track     = 0;
    PlayState _playState = PlayState::STOPPED;
    uint8_t   _patIdx    = 0;
    uint8_t   _step      = 0;
    uint16_t  _bpm       = BPM_DEFAULT;
    bool      _midiClock = false;
    bool      _muted     = false;
    bool      _recording = false;

    unsigned long _stepIntervalUs  = 125000;
    unsigned long _clockIntervalUs = 20833;
    unsigned long _stepBaseUs      = 0;   // nominal (un-nudged) start of _step
    unsigned long _nextClockUs     = 0;
    unsigned long _lastFireUs      = 0;   // when the current step actually sounded

    // Ratchet scheduling
    uint8_t       _ratchetLeft = 0;
    unsigned long _ratchetNextUs = 0, _ratchetIntervalUs = 0;
    uint8_t       _ratchetStep = 0;

    // Notes currently sounding, with their scheduled note-off.
    struct Held { uint8_t note; uint8_t chan; unsigned long offUs; bool tied; };
    Held    _held[STEP_MAX_NOTES * 2];
    uint8_t _heldCount = 0;

    Pattern    _patterns[NUM_PATTERNS];
    ChainEntry _chain[CHAIN_LEN];
    uint8_t    _chainLen = 0, _chainPos = 0, _chainRepeat = 0;

    // External sync
    bool     _extSync = false, _extAwaitFirst = false;
    uint16_t _tickInStep = 0;
    uint32_t _tickCount = 0;
    unsigned long _tickRefUs = 0;

    void     computeInterval();
    unsigned long intervalForStep(uint8_t s) const;   // includes swing
    long     microOffsetUs(uint8_t s) const;
    void     triggerStep(uint8_t s, bool isRatchet);
    void     advanceStep();
    void     releaseAll(bool sendOff = true);
    void     serviceHeld(unsigned long now);
    void     applyLocks(const Step& st);
    uint16_t stepTicks() const;
    uint16_t ticksForStep(uint8_t s) const;
};
