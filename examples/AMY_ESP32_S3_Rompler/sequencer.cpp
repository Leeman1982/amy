#include "sequencer.h"
#include <string.h>

SeqNoteOnFn  seqNoteOn  = nullptr;
SeqNoteOffFn seqNoteOff = nullptr;
SeqLockFn    seqLock    = nullptr;

void Sequencer::begin(uint8_t trackIdx) {
    _track = trackIdx;
    _bpm   = BPM_DEFAULT;
    for (uint8_t i = 0; i < NUM_PATTERNS; i++) clearPattern(i);
    for (uint8_t i = 0; i < NUM_PATTERNS; i++) _patterns[i].midiChannel = trackIdx + 1;
    _step = 0; _patIdx = 0; _chainLen = 0; _heldCount = 0;
    _playState = PlayState::STOPPED;
    computeInterval();
}

// ─── Timing ─────────────────────────────────────────────────────────────────
void Sequencer::computeInterval() {
    // 24-PPQN MIDI clock is tempo-only: a quarter / 24 = 2,500,000 / BPM. It
    // must NOT follow the per-pattern subdivision or slaves would drift.
    _clockIntervalUs = 2500000UL / _bpm;
    unsigned long base = 15000000UL / _bpm;                 // one 16th note
    const Subdiv& sd = SUBDIVS[_patterns[_patIdx].subdiv % NUM_SUBDIVS];
    _stepIntervalUs = (unsigned long)((uint64_t)base * sd.num / sd.den);
}

void Sequencer::setBPM(uint16_t b) {
    _bpm = constrain((int)b, BPM_MIN, BPM_MAX);
    computeInterval();
}

// Swing pushes odd steps late and pulls even steps early by the same amount, so
// a pair still spans exactly two nominal steps and the bar never drifts.
unsigned long Sequencer::intervalForStep(uint8_t s) const {
    const Pattern& p = _patterns[_patIdx];
    if (p.swing == 0) return _stepIntervalUs;
    long sw = (long)(_stepIntervalUs * p.swing / 200UL);
    long iv = (long)_stepIntervalUs + ((s & 1) ? sw : -sw);
    return (unsigned long)(iv > 100 ? iv : 100);
}

long Sequencer::microOffsetUs(uint8_t s) const {
    int8_t m = _patterns[_patIdx].steps[s].micro;
    if (m == 0) return 0;
    return (long)_stepIntervalUs * (long)m / 100L;
}

uint16_t Sequencer::stepProgress() const {
    if (_playState != PlayState::PLAYING || _stepIntervalUs == 0) return 0;
    unsigned long e = micros() - _stepBaseUs;
    long prog = (long)((uint64_t)e * 1000 / _stepIntervalUs);
    return (uint16_t)constrain(prog, 0L, 999L);
}

// ─── Transport ──────────────────────────────────────────────────────────────
void Sequencer::play(unsigned long startUs) {
    if (_playState == PlayState::PLAYING) return;
    _playState   = PlayState::PLAYING;
    _step        = 0;
    _chainPos    = 0;
    _chainRepeat = 0;
    _ratchetLeft = 0;
    computeInterval();
    if (_midiClock) seqEmitStart();
    _stepBaseUs  = startUs ? startUs : micros();
    _nextClockUs = _stepBaseUs;

    if (_extSync) {
        // Slaved: the master's clock owns the grid. Arm and let the next
        // incoming tick fire step 0, rather than sounding one note and then
        // freezing until the clock happens to arrive.
        _tickInStep = 0;
        _extAwaitFirst = true;
        return;
    }
    // Fire step 0 immediately — don't wait one full interval. The base MUST
    // advance with it: leaving it on the play timestamp makes the scheduler
    // consider step 1 already due on the very next update(), so the first two
    // steps land on top of each other and every lap is one interval short.
    triggerStep(_step, false);
    _stepBaseUs += intervalForStep(_step);
    advanceStep();
}

void Sequencer::stop() {
    _playState = PlayState::STOPPED;
    releaseAll(true);
    if (_midiClock) seqEmitStop();
    _ratchetLeft = 0;
    _step = 0;
}

void Sequencer::pause() {
    if (_playState == PlayState::PLAYING) {
        _playState = PlayState::PAUSED;
        releaseAll(true);
        if (_midiClock) seqEmitStop();
    } else if (_playState == PlayState::PAUSED) {
        _playState  = PlayState::PLAYING;
        _stepBaseUs = micros();
        _nextClockUs = _stepBaseUs;
        if (_midiClock) seqEmitContinue();
    }
}

void Sequencer::togglePlay() {
    if (_playState == PlayState::PLAYING) stop(); else play();
}

void Sequencer::setMuted(bool m) {
    _muted = m;
    if (m) releaseAll(true);
}

void Sequencer::setPattern(uint8_t idx) {
    if (idx >= NUM_PATTERNS) return;
    releaseAll(true);
    _patIdx = idx;
    computeInterval();
}

// ─── Note plumbing ──────────────────────────────────────────────────────────
void Sequencer::releaseAll(bool sendOff) {
    if (sendOff && seqNoteOff)
        for (uint8_t i = 0; i < _heldCount; i++) seqNoteOff(_track, _held[i].chan, _held[i].note);
    _heldCount = 0;
}

// Note-offs are absolute-time entries rather than "the one note that is
// playing": a step can be a four-note chord, and ratchets can overlap the tail
// of the previous hit, so a single _activeNote would strand notes on.
void Sequencer::serviceHeld(unsigned long now) {
    uint8_t w = 0;
    for (uint8_t i = 0; i < _heldCount; i++) {
        if (!_held[i].tied && (long)(now - _held[i].offUs) >= 0) {
            if (seqNoteOff) seqNoteOff(_track, _held[i].chan, _held[i].note);
        } else {
            _held[w++] = _held[i];
        }
    }
    _heldCount = w;
}

// Every track can now lock its own mixer/engine parameters to a step (gain,
// pan, tune — see rompler_params.h) — unlike xtro, where only track 0 (the
// one internal synth) could carry parameter locks.
void Sequencer::applyLocks(const Step& st) {
    if (!seqLock) return;
    for (uint8_t i = 0; i < STEP_LOCKS; i++)
        if (st.lockId[i] != LOCK_NONE) seqLock(_track, st.lockId[i], st.lockVal[i]);
}

void Sequencer::triggerStep(uint8_t s, bool isRatchet) {
    Pattern& pat = _patterns[_patIdx];
    const Step& st = pat.steps[s];
    unsigned long now = micros();
    _lastFireUs = now;

    if (_muted) return;
    if (!st.active()) {
        // An inactive step ends anything the previous step tied over.
        for (uint8_t i = 0; i < _heldCount; i++) _held[i].tied = false;
        return;
    }
    if (!isRatchet && st.probability < 100 && (uint8_t)random(100) >= st.probability) {
        for (uint8_t i = 0; i < _heldCount; i++) _held[i].tied = false;
        return;
    }

    if (!isRatchet) applyLocks(st);

    bool slide = st.slide() && _heldCount > 0;

    // Legato overlap: with slide, the old notes are released *after* the new
    // ones start so mono/legato receivers glide instead of retriggering.
    Held prev[STEP_MAX_NOTES * 2];
    uint8_t prevCount = 0;
    if (slide) {
        memcpy(prev, _held, sizeof(Held) * _heldCount);
        prevCount = _heldCount;
        _heldCount = 0;
    } else {
        releaseAll(true);
    }

    uint8_t vel = st.velocity;
    if (st.accent()) vel = (uint8_t)min(127, (int)vel + 30);

    uint8_t n = st.noteCount ? st.noteCount : 1;
    if (n > STEP_MAX_NOTES) n = STEP_MAX_NOTES;

    // Gate: a tie holds past the boundary; ratchets share the step between them.
    unsigned long span = isRatchet ? _ratchetIntervalUs : intervalForStep(s);
    if (!isRatchet && st.ratchet > 1) span = _stepIntervalUs / st.ratchet;
    unsigned long gateUs = span * st.gate / 100UL;
    if (gateUs < 1000UL) gateUs = 1000UL;

    for (uint8_t i = 0; i < n && _heldCount < STEP_MAX_NOTES * 2; i++) {
        int note = (int)st.notes[i] + (int)pat.transpose;
        note = constrain(note, 0, 127);
        uint8_t q = quantizeNote((uint8_t)note, pat.rootNote, pat.scaleIdx);
        if (seqNoteOn) seqNoteOn(_track, pat.midiChannel, q, vel, st.accent(), slide);
        _held[_heldCount].note  = q;
        _held[_heldCount].chan  = pat.midiChannel;
        _held[_heldCount].offUs = now + gateUs;
        _held[_heldCount].tied  = st.tie() && !isRatchet;
        _heldCount++;
    }

    if (slide && seqNoteOff) {
        for (uint8_t i = 0; i < prevCount; i++) {
            bool stillHeld = false;
            for (uint8_t j = 0; j < _heldCount; j++)
                if (_held[j].note == prev[i].note) { stillHeld = true; break; }
            if (!stillHeld) seqNoteOff(_track, prev[i].chan, prev[i].note);
        }
    }

    // Arm the remaining ratchet hits.
    if (!isRatchet && st.ratchet > 1) {
        _ratchetLeft       = st.ratchet - 1;
        _ratchetIntervalUs = _stepIntervalUs / st.ratchet;
        _ratchetNextUs     = now + _ratchetIntervalUs;
        _ratchetStep       = s;
    }
}

void Sequencer::advanceStep() {
    Pattern& pat = _patterns[_patIdx];
    uint8_t len = pat.length ? pat.length : 1;
    _step = (uint8_t)((_step + 1) % len);

    if (_step == 0 && _chainLen > 0) {
        _chainRepeat++;
        if (_chainRepeat >= _chain[_chainPos].repeats) {
            _chainRepeat = 0;
            _chainPos = (uint8_t)((_chainPos + 1) % _chainLen);
            if (_chain[_chainPos].patternIdx < 0) _chainPos = 0;
            int8_t next = _chain[_chainPos].patternIdx;
            if (next >= 0 && next < NUM_PATTERNS && next != (int8_t)_patIdx) {
                _patIdx = (uint8_t)next;
                computeInterval();
            }
        }
    }
}

// ─── Main update ────────────────────────────────────────────────────────────
// All time comparisons use signed-difference form so they survive the ~71
// minute micros() rollover.
void Sequencer::update() {
    unsigned long now = micros();

    // Gate-offs run even when paused/stopped so nothing is ever stranded on.
    serviceHeld(now);

    if (_playState != PlayState::PLAYING) return;

    Pattern& pat = _patterns[_patIdx];
    if (_step >= pat.length) _step = 0;

    // Ratchet sub-hits: timed in microseconds from the step that armed them, so
    // they land identically whether the clock is internal or external.
    while (_ratchetLeft > 0 && (long)(now - _ratchetNextUs) >= 0) {
        triggerStep(_ratchetStep, true);
        _ratchetLeft--;
        _ratchetNextUs += _ratchetIntervalUs;
    }

    if (_extSync) return;        // the master owns the grid; see extTick()

    // MIDI clock — 24 PPQN, tempo-based (never subdivided), bounded catch-up.
    if (_midiClock) {
        for (int i = 0; i < 8 && (long)(now - _nextClockUs) >= 0; i++) {
            seqEmitClock();
            _nextClockUs += _clockIntervalUs;
        }
    }

    // Step advance. Processing every elapsed interval (not just one per call)
    // is what stops a slow OLED frame from turning the sequencer into a
    // loop()-rate sequencer: without it, steps would silently be dropped.
    int guard = pat.length + 1;
    while (guard-- > 0) {
        long trigAt = (long)(_stepBaseUs + microOffsetUs(_step));
        if ((long)(now - (unsigned long)trigAt) < 0) break;
        triggerStep(_step, false);
        _stepBaseUs += intervalForStep(_step);
        advanceStep();
    }
}

// ─── External sync (MIDI clock in, 24 PPQN) ─────────────────────────────────
uint16_t Sequencer::stepTicks() const {
    const Subdiv& sd = SUBDIVS[_patterns[_patIdx].subdiv % NUM_SUBDIVS];
    uint16_t t = (uint16_t)((6UL * sd.num) / sd.den);
    return t ? t : 1;
}

uint16_t Sequencer::ticksForStep(uint8_t s) const {
    uint16_t base = stepTicks();
    uint16_t d = (uint16_t)(((uint32_t)base * _patterns[_patIdx].swing) / 200UL);
    if (s & 1) return (uint16_t)(base + d);
    return (base > d) ? (uint16_t)(base - d) : 1;
}

void Sequencer::setExternalSync(bool en) {
    if (_extSync == en) return;
    _extSync = en;
    _tickInStep = 0; _tickCount = 0; _tickRefUs = 0; _extAwaitFirst = false;
    if (en && _playState == PlayState::PLAYING) { _extAwaitFirst = true; _step = 0; }
}

void Sequencer::extStart() {
    if (!_extSync) return;
    releaseAll(true);
    _step = 0; _chainPos = 0; _chainRepeat = 0;
    _tickInStep = 0; _tickCount = 0; _tickRefUs = 0;
    _extAwaitFirst = true;
    _playState = PlayState::PLAYING;
    computeInterval();
}

void Sequencer::extContinue() {
    if (!_extSync) return;
    _playState = PlayState::PLAYING;
    _extAwaitFirst = true;
}

void Sequencer::extStop() {
    if (!_extSync) return;
    _playState = PlayState::STOPPED;
    releaseAll(true);
}

void Sequencer::extTick() {
    if (!_extSync || _playState != PlayState::PLAYING) return;
    unsigned long now = micros();

    // Derive tempo once per quarter note. Gate lengths, ratchets and the BPM
    // readout are all microsecond-based, so they have to track the master even
    // though the step grid itself is driven by counting ticks.
    if ((_tickCount % 24) == 0) {
        if (_tickRefUs) {
            unsigned long dt = now - _tickRefUs;
            if (dt > 20000UL && dt < 2000000UL) {
                uint32_t b = 60000000UL / dt;
                if (b >= BPM_MIN && b <= BPM_MAX && b != _bpm) setBPM((uint16_t)b);
            }
        }
        _tickRefUs = now;
    }
    _tickCount++;

    if (_extAwaitFirst) {
        _extAwaitFirst = false;
        _tickInStep = 0;
        _stepBaseUs = now;
        triggerStep(_step, false);
        advanceStep();
        return;
    }

    if (++_tickInStep >= ticksForStep(_step)) {
        _tickInStep = 0;
        _stepBaseUs = now;
        if (_step >= _patterns[_patIdx].length) _step = 0;
        triggerStep(_step, false);
        advanceStep();
    }
}

// ─── Chain ──────────────────────────────────────────────────────────────────
void Sequencer::setChain(const ChainEntry* c, uint8_t len) {
    _chainLen = min(len, (uint8_t)CHAIN_LEN);
    _chainPos = 0; _chainRepeat = 0;
    memcpy(_chain, c, _chainLen * sizeof(ChainEntry));
    if (_chainLen > 0 && _chain[0].patternIdx >= 0) setPattern((uint8_t)_chain[0].patternIdx);
}

// ─── Pattern utilities ──────────────────────────────────────────────────────
void Sequencer::clearPattern(uint8_t idx) {
    if (idx >= NUM_PATTERNS) return;
    Pattern& p = _patterns[idx];
    uint8_t ch = p.midiChannel ? p.midiChannel : (uint8_t)(_track + 1);
    p = Pattern();
    p.midiChannel = ch;
}

void Sequencer::copyPattern(uint8_t src, uint8_t dst) {
    if (src >= NUM_PATTERNS || dst >= NUM_PATTERNS) return;
    _patterns[dst] = _patterns[src];
}

void Sequencer::quantizePattern(uint8_t idx) {
    if (idx >= NUM_PATTERNS) return;
    Pattern& p = _patterns[idx];
    for (uint8_t s = 0; s < NUM_STEPS; s++) {
        if (!p.steps[s].active()) continue;
        for (uint8_t n = 0; n < p.steps[s].noteCount && n < STEP_MAX_NOTES; n++)
            p.steps[s].notes[n] = quantizeNote(p.steps[s].notes[n], p.rootNote, p.scaleIdx);
    }
}

// Fills within the pattern's own scale, so "randomise" produces something
// playable rather than a chromatic accident.
void Sequencer::randomisePattern(uint8_t idx, uint8_t density) {
    if (idx >= NUM_PATTERNS) return;
    Pattern& p = _patterns[idx];
    for (uint8_t s = 0; s < p.length; s++) {
        Step& st = p.steps[s];
        bool on = (uint8_t)random(100) < density;
        st.flags = on ? ST_ACTIVE : 0;
        if (!on) continue;
        int base = 48 + (int)random(25);
        st.notes[0]   = quantizeNote((uint8_t)base, p.rootNote, p.scaleIdx);
        st.noteCount  = 1;
        st.velocity   = (uint8_t)(70 + random(50));
        st.gate       = (uint8_t)(40 + random(50));
        st.probability= (random(6) == 0) ? (uint8_t)(50 + random(40)) : 100;
        st.ratchet    = (random(10) == 0) ? (uint8_t)(2 + random(3)) : 1;
        st.micro      = 0;
        if (random(8) == 0) st.flags |= ST_ACCENT;
    }
}

// Bjorklund/Euclid: spread `hits` as evenly as possible over the pattern length.
void Sequencer::euclidFill(uint8_t idx, uint8_t hits, uint8_t rotate) {
    if (idx >= NUM_PATTERNS) return;
    Pattern& p = _patterns[idx];
    uint8_t n = p.length ? p.length : 1;
    if (hits > n) hits = n;
    for (uint8_t s = 0; s < n; s++) {
        // The classic closed form: a hit lands wherever the running numerator
        // crosses a multiple of n.
        bool on = hits ? (((uint32_t)((s + rotate) % n) * hits) % n) < hits : false;
        if (on) p.steps[s].flags |= ST_ACTIVE;
        else    p.steps[s].flags &= (uint8_t)~ST_ACTIVE;
    }
}

void Sequencer::humanise(uint8_t idx, uint8_t amount) {
    if (idx >= NUM_PATTERNS) return;
    Pattern& p = _patterns[idx];
    for (uint8_t s = 0; s < p.length; s++) {
        Step& st = p.steps[s];
        if (!st.active()) continue;
        int v = (int)st.velocity + (int)random(-(int)amount, (int)amount + 1);
        st.velocity = (uint8_t)constrain(v, 1, 127);
        int m = (int)st.micro + (int)random(-(int)(amount / 6) - 1, (int)(amount / 6) + 2);
        st.micro = (int8_t)constrain(m, -50, 50);
    }
}

// ─── Live record / audition ─────────────────────────────────────────────────
void Sequencer::recordNote(uint8_t note, uint8_t vel) {
    if (!_recording) return;
    Pattern& pat = _patterns[_patIdx];
    // Quantise to the nearest step: past the halfway point the note belongs to
    // the step you are heading into, which is how anyone actually plays.
    uint8_t target = _step;
    if (_playState == PlayState::PLAYING) {
        target = (stepProgress() > 500) ? _step : (uint8_t)((_step + pat.length - 1) % pat.length);
    }
    Step& st = pat.steps[target];
    if (!st.active()) {
        st.flags |= ST_ACTIVE;
        st.noteCount = 1;
        st.notes[0]  = note;
    } else if (st.noteCount < STEP_MAX_NOTES) {
        bool dup = false;
        for (uint8_t i = 0; i < st.noteCount; i++) if (st.notes[i] == note) dup = true;
        if (!dup) st.notes[st.noteCount++] = note;
    }
    st.velocity = vel ? vel : st.velocity;
}

void Sequencer::auditionStep(uint8_t s) {
    Pattern& pat = _patterns[_patIdx];
    const Step& st = pat.steps[s % NUM_STEPS];
    if (!seqNoteOn || !seqNoteOff) return;
    unsigned long now = micros();
    releaseAll(true);
    uint8_t n = st.noteCount ? st.noteCount : 1;
    for (uint8_t i = 0; i < n && _heldCount < STEP_MAX_NOTES * 2; i++) {
        int note = constrain((int)st.notes[i] + (int)pat.transpose, 0, 127);
        uint8_t q = quantizeNote((uint8_t)note, pat.rootNote, pat.scaleIdx);
        seqNoteOn(_track, pat.midiChannel, q, st.velocity, st.accent(), false);
        _held[_heldCount].note  = q;
        _held[_heldCount].chan  = pat.midiChannel;
        _held[_heldCount].offUs = now + 180000UL;   // 180 ms audition
        _held[_heldCount].tied  = false;
        _heldCount++;
    }
}
