#include "ui.h"
#include <Wire.h>
#include "midi.h"
#include "rompler_engine.h"
#include "rompler_params.h"
#include <stdio.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include <esp_system.h>
#endif

// ─── Hooks into the sketch ──────────────────────────────────────────────────
extern MidiIO  midi;
extern bool    g_recArm;
extern float   g_masterVol;
extern PatchInfo g_patchList[MAX_PATCHES];
extern uint8_t   g_patchCount;
void uiTogglePlayAll();
void uiStopAll();
void uiApplySync();
void uiSaveAll();
void uiPanic();
void uiPreviewNote(uint8_t note, bool on);
void uiRescanPatches();
void uiLoadKit(uint8_t slot);
void uiSaveKit(uint8_t slot, const char* name);

// ─── Layout ─────────────────────────────────────────────────────────────────
#define ROW_H       10
#define VIS_ROWS     4
#define CONTENT_Y   11
#define FOOTER_Y    53

static const char* const MENU_LABELS[] = {
    "STEP EDIT", "PATTERN", "PAT OPTS", "CHAIN", "TOOLS",
    "PATCH", "KITS", "MIXER", "MIDI", "SETTINGS", "PERF", "HELP"
};

static const char* const STEP_FIELD_LABELS[] = {
    "Active", "Note 1", "Note 2", "Note 3", "Note 4", "Notes",
    "Velocity", "Gate", "Probab.", "Micro", "Ratchet",
    "Accent", "Slide", "Tie", "P-LOCKS >"
};

static const char* const TOOL_LABELS[] = {
    "Euclid hits", "Euclid rot", "APPLY EUCLID",
    "Rand dens", "RANDOMISE", "Human amt", "HUMANISE",
    "CLEAR PATTERN", "COPY->CLIP", "PASTE CLIP"
};
#define NUM_TOOL_ROWS 10

static const char* const MIDI_LABELS[] = {
    "In Chan", "Out Enable", "Clock Out", "Ext Sync", "Soft Thru", "PANIC"
};
#define NUM_MIDI_ROWS 6

static const char* const SETTING_LABELS[] = {
    "Master Vol", "Contrast", "Metronome", "SAVE ALL", "RESCAN SD"
};
#define NUM_SETTING_ROWS 5

static const char* const PATOPT_LABELS[] = {
    "Length", "Swing", "Subdiv", "Scale", "Root", "Transpose", "MIDI Chan"
};
#define NUM_PATOPT_ROWS 7

#define NUM_PERF_ROWS 8

static const char* const HELP_LINES[] = {
    "ENC: scroll/edit",
    "ENC PUSH: open/edit",
    "PLAY: run  SH+PLAY: rec",
    "PAGE: back  LONG=save",
    "TRACK: next  SH=pattern",
    "MUTE: mute  SH+MUTE",
    "MENU>PATCH: load sample",
    "MENU>KITS: save/recall",
    "MENU>MIXER: gain/pan/tune",
};
#define NUM_HELP_ROWS (int)(sizeof(HELP_LINES) / sizeof(HELP_LINES[0]))

// ─── Init ───────────────────────────────────────────────────────────────────
void UI::beginDisplay() {
    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
    _oled.setI2CAddress(OLED_ADDR << 1);
    _oled.setBusClock(OLED_I2C_HZ);
    _oled.begin();
    _oled.clearBuffer();
    _oled.setFont(u8g2_font_6x10_tr);
    _oled.drawStr(20, 30, "AMY ROMPLER");
    _oled.sendBuffer();
}

void UI::bootStatus(const char* msg) {
    _oled.clearBuffer();
    _oled.setFont(u8g2_font_6x10_tr);
    _oled.drawStr(2, 30, "AMY ROMPLER");
    _oled.setFont(u8g2_font_5x7_tr);
    _oled.drawStr(2, 45, msg);
    _oled.sendBuffer();
}

void UI::bind(Sequencer* seqs, uint8_t* activeTrack, SdStorage* storage, GlobalSettings* gs) {
    _seqs = seqs; _track = activeTrack; _store = storage; _gs = gs;
}

void UI::applyContrast(uint8_t c) { _oled.setContrast(c); }

void UI::toast(const char* msg) {
    strncpy(_toastMsg, msg, sizeof(_toastMsg) - 1);
    _toastMsg[sizeof(_toastMsg) - 1] = 0;
    _toastUntil = millis() + 900;
    _dirty = true;
}

void UI::gotoScreen(Screen s) {
    _screen = s;
    _editing = false;
    _row = 0; _scroll = 0;
    _dirty = true;
}

// ─── Row counts ─────────────────────────────────────────────────────────────
int UI::rowCount() const {
    switch (_screen) {
    case Screen::MENU:         return (int)MenuItem::COUNT;
    case Screen::STEP_EDIT:    return (int)StepField::COUNT;
    case Screen::PLOCK_EDIT:   return STEP_LOCKS * 2 + 1;   // id/value pairs + clear
    case Screen::PATTERN_SEL:  return NUM_PATTERNS;
    case Screen::PATTERN_OPTS: return NUM_PATOPT_ROWS;
    case Screen::CHAIN_EDIT:   return CHAIN_LEN + 1;        // slots + APPLY
    case Screen::TOOLS:        return NUM_TOOL_ROWS;
    case Screen::PATCH_BROWSE: return g_patchCount;
    case Screen::KITS:         return KIT_SLOTS;
    case Screen::KIT_SAVE:     return 3;                    // slot, name, SAVE
    case Screen::MIXER:        return NUM_TRACKS * NUM_ROMPLER_PARAMS;
    case Screen::MIDI_SET:     return NUM_MIDI_ROWS;
    case Screen::SETTINGS:     return NUM_SETTING_ROWS;
    case Screen::PERF:         return NUM_PERF_ROWS;
    case Screen::HELP:         return NUM_HELP_ROWS;
    default:                   return 0;
    }
}

void UI::ensureVisible(int visibleRows) {
    if (_row < _scroll) _scroll = _row;
    if (_row >= _scroll + visibleRows) _scroll = _row - visibleRows + 1;
    int n = rowCount();
    if (_scroll > n - visibleRows) _scroll = n - visibleRows;
    if (_scroll < 0) _scroll = 0;
}

void UI::moveRow(int delta) {
    int n = rowCount();
    if (n <= 0) return;
    _row += delta;
    if (_row < 0) _row = n - 1;
    if (_row >= n) _row = 0;
    ensureVisible(VIS_ROWS);
}

// ─── Encoder ────────────────────────────────────────────────────────────────
void UI::onEncoder(int delta) {
    if (delta == 0) return;
    _dirty = true;
    // SHIFT multiplies the step so long ranges (BPM, note, kit slot) are
    // reachable without spinning the encoder for a minute.
    int fast = _shift ? 8 : 1;

    if (_screen == Screen::MAIN) {
        if (_shift) {
            for (uint8_t t = 0; t < NUM_TRACKS; t++) _seqs[t].nudgeBPM(delta);
            _gs->bpm = seq().bpm();
        } else {
            int len = pat().length ? pat().length : 1;
            int s = (int)_selStep + delta;
            while (s < 0) s += len;
            _selStep = (uint8_t)(s % len);
        }
        return;
    }

    if (_editing) editValue(delta * fast);
    else          moveRow(delta * fast);
}

void UI::onEncPress() {
    _dirty = true;
    switch (_screen) {
    case Screen::MAIN:
        _row = 0; _scroll = 0;
        gotoScreen(Screen::STEP_EDIT);
        break;
    default:
        activateRow();
        break;
    }
}

void UI::onEncLong() {
    _dirty = true;
    if (_screen == Screen::MAIN) {
        curStep().flags ^= ST_ACTIVE;
        toast((curStep().flags & ST_ACTIVE) ? "STEP ON" : "STEP OFF");
    } else {
        _editing = false;
        onPage();
    }
}

// ─── Buttons ────────────────────────────────────────────────────────────────
void UI::onPlay() {
    _dirty = true;
    if (_shift) {
        g_recArm = !g_recArm;
        for (uint8_t t = 0; t < NUM_TRACKS; t++) _seqs[t].setRecord(g_recArm && t == *_track);
        toast(g_recArm ? "REC ARMED" : "REC OFF");
        return;
    }
    if (_screen == Screen::STEP_EDIT || _screen == Screen::PLOCK_EDIT) {
        seq().auditionStep(_selStep);
        return;
    }
    uiTogglePlayAll();
}

void UI::onPlayLong() {
    _dirty = true;
    if (_screen == Screen::MAIN || _screen == Screen::STEP_EDIT) {
        curStep().flags ^= ST_ACTIVE;
        toast((curStep().flags & ST_ACTIVE) ? "STEP ON" : "STEP OFF");
    } else {
        uiStopAll();
        toast("STOP ALL");
    }
}

void UI::onPage() {
    _dirty = true;
    if (_editing) { _editing = false; return; }
    switch (_screen) {
    case Screen::MAIN:       gotoScreen(Screen::MENU); break;
    case Screen::MENU:       gotoScreen(Screen::MAIN); break;
    case Screen::PLOCK_EDIT: gotoScreen(Screen::STEP_EDIT); break;
    case Screen::KIT_SAVE:   gotoScreen(Screen::KITS); break;
    default:                 gotoScreen(Screen::MENU); break;
    }
}

void UI::onPageLong() {
    if (_store && !_store->mounted()) {
        toast("NO SD CARD");
    } else {
        uiSaveAll();
        toast("SAVED");
    }
    _dirty = true;
}

void UI::onTrack() {
    _dirty = true;
    if (_screen == Screen::STEP_EDIT || _screen == Screen::PLOCK_EDIT) {
        // In the step editor the TRACK button steps through the pattern, which
        // is far quicker than backing out to the grid for every note.
        uint8_t len = pat().length ? pat().length : 1;
        _selStep = _shift ? (uint8_t)((_selStep + len - 1) % len)
                          : (uint8_t)((_selStep + 1) % len);
        return;
    }
    if (_shift) {   // SHIFT+TRACK on any other screen = next pattern
        uint8_t next = (uint8_t)((seq().currentPattern() + 1) % NUM_PATTERNS);
        seq().setPattern(next);
        _gs->track[*_track].lastPattern = next;
        char b[20]; snprintf(b, sizeof(b), "PATTERN %u", (unsigned)(next + 1));
        toast(b);
        return;
    }
    *_track = (uint8_t)((*_track + 1) % NUM_TRACKS);
    _selStep = 0;
    TrackPatch& tp = patchForTrack(*_track);
    char b[24];
    snprintf(b, sizeof(b), "TRACK %u %s", (unsigned)(*_track + 1),
             tp.loaded ? tp.info.name : "EMPTY");
    toast(b);
}

void UI::onTrackLong() {
    _clip = pat();
    _clipFull = true;
    toast("PATTERN COPIED");
    _dirty = true;
}

void UI::onMute() {
    _dirty = true;
    if (_shift) {
        seq().setMuted(!seq().muted());
        _gs->track[*_track].mute = seq().muted() ? 1 : 0;
        toast(seq().muted() ? "TRACK MUTED" : "TRACK LIVE");
        return;
    }
    if (_screen == Screen::MAIN || _screen == Screen::STEP_EDIT) {
        curStep().flags ^= ST_ACTIVE;
        return;
    }
    if (_screen == Screen::MIXER) {
        uint8_t t = (uint8_t)constrain((int)_row / NUM_ROMPLER_PARAMS, 0, NUM_TRACKS - 1);
        _seqs[t].setMuted(!_seqs[t].muted());
        _gs->track[t].mute = _seqs[t].muted() ? 1 : 0;
        return;
    }
    // Elsewhere MUTE is a quick jump back to the grid.
    gotoScreen(Screen::MAIN);
}

void UI::onMuteLong() {
    _dirty = true;
    if (!_clipFull) { toast("CLIPBOARD EMPTY"); return; }
    uint8_t ch = pat().midiChannel;
    pat() = _clip;
    pat().midiChannel = ch;          // paste the music, keep this track's routing
    seq().refreshTiming();
    toast("PATTERN PASTED");
}

// ─── Row activation / editing ───────────────────────────────────────────────
void UI::activateRow() {
    switch (_screen) {

    case Screen::MENU: {
        static const Screen DEST[] = {
            Screen::STEP_EDIT, Screen::PATTERN_SEL, Screen::PATTERN_OPTS,
            Screen::CHAIN_EDIT, Screen::TOOLS, Screen::PATCH_BROWSE,
            Screen::KITS, Screen::MIXER, Screen::MIDI_SET,
            Screen::SETTINGS, Screen::PERF, Screen::HELP
        };
        gotoScreen(DEST[_row]);
        break;
    }

    case Screen::STEP_EDIT:
        if ((StepField)_row == StepField::LOCKS) { gotoScreen(Screen::PLOCK_EDIT); break; }
        _editing = !_editing;
        break;

    case Screen::PLOCK_EDIT:
        if (_row == STEP_LOCKS * 2) {            // CLEAR ALL
            for (uint8_t i = 0; i < STEP_LOCKS; i++) curStep().lockId[i] = LOCK_NONE;
            toast("LOCKS CLEARED");
        } else {
            _editing = !_editing;
        }
        break;

    case Screen::PATTERN_SEL:
        seq().setPattern((uint8_t)_row);
        _gs->track[*_track].lastPattern = (uint8_t)_row;
        _selStep = 0;
        toast("PATTERN LOADED");
        break;

    case Screen::PATTERN_OPTS:
        _editing = !_editing;
        break;

    case Screen::CHAIN_EDIT:
        if (_row == CHAIN_LEN) {                 // APPLY
            ChainEntry c[CHAIN_LEN];
            uint8_t n = 0;
            TrackState& ts = _gs->track[*_track];
            for (uint8_t i = 0; i < CHAIN_LEN; i++) {
                if (ts.chain[i].patternIdx < 0) break;
                c[n++] = ts.chain[i];
            }
            if (n) { seq().setChain(c, n); toast("CHAIN ON"); }
            else   { seq().clearChain();   toast("CHAIN OFF"); }
            ts.chainLen = n;
        } else {
            _editing = !_editing;
        }
        break;

    case Screen::TOOLS: {
        Pattern& p = pat();
        uint8_t pi = seq().currentPattern();
        switch (_row) {
        case 0: case 1: case 3: case 5: _editing = !_editing; break;
        case 2: seq().euclidFill(pi, _eucHits, _eucRot); toast("EUCLID"); break;
        case 4: seq().randomisePattern(pi, _randDensity); toast("RANDOMISED"); break;
        case 6: seq().humanise(pi, _humanAmt); toast("HUMANISED"); break;
        case 7: seq().clearPattern(pi); toast("CLEARED"); break;
        case 8: _clip = p; _clipFull = true; toast("COPIED"); break;
        case 9: if (_clipFull) { uint8_t ch = p.midiChannel; p = _clip; p.midiChannel = ch;
                                  seq().refreshTiming(); toast("PASTED"); }
                else toast("CLIP EMPTY");
                break;
        }
        break;
    }

    case Screen::PATCH_BROWSE:
        if (_row < g_patchCount) {
            bool ok = romplerLoadPatch(*_track, g_patchList[_row].dirName);
            strncpy(_gs->track[*_track].patchDir, g_patchList[_row].dirName,
                    sizeof(_gs->track[*_track].patchDir) - 1);
            toast(ok ? g_patchList[_row].name : "LOAD FAILED");
        }
        break;

    case Screen::KITS:
        if (_shift) {
            _saveSlot = (uint8_t)_row;
            memset(_nameBuf, ' ', 13);
            _nameBuf[13] = 0;
            char tmp[14];
            snprintf(tmp, sizeof(tmp), "KIT %02u", (unsigned)(_saveSlot + 1));
            memcpy(_nameBuf, tmp, strlen(tmp));
            _nameCursor = 0;
            gotoScreen(Screen::KIT_SAVE);
        } else {
            uiLoadKit((uint8_t)_row);
        }
        break;

    case Screen::KIT_SAVE:
        if (_row == 2) { uiSaveKit(_saveSlot, _nameBuf); toast("KIT SAVED");
                         gotoScreen(Screen::KITS); }
        else _editing = !_editing;
        break;

    case Screen::MIXER:
        _editing = !_editing;
        break;

    case Screen::MIDI_SET:
        if (_row == 5) { uiPanic(); toast("ALL NOTES OFF"); }
        else _editing = !_editing;
        break;

    case Screen::SETTINGS:
        if (_row == 3) {
            if (!_store->mounted()) { toast("NO SD CARD"); break; }
            uiSaveAll();
            toast("SAVED");
        } else if (_row == 4) {
            uiRescanPatches();
            toast("SD RESCANNED");
        } else {
            _editing = !_editing;
        }
        break;

    default:
        break;
    }
    _dirty = true;
}

void UI::editValue(int d) {
    switch (_screen) {

    case Screen::STEP_EDIT: {
        Step& st = curStep();
        switch ((StepField)_row) {
        case StepField::ACTIVE:  st.flags ^= ST_ACTIVE; _editing = false; break;
        case StepField::NOTE1: case StepField::NOTE2:
        case StepField::NOTE3: case StepField::NOTE4: {
            uint8_t i = (uint8_t)_row - (uint8_t)StepField::NOTE1;
            st.notes[i] = (uint8_t)constrain((int)st.notes[i] + d, 0, 127);
            uiPreviewNote(st.notes[i], true);
            break;
        }
        case StepField::NOTECOUNT:
            st.noteCount = (uint8_t)constrain((int)st.noteCount + (d > 0 ? 1 : -1), 1, STEP_MAX_NOTES);
            break;
        case StepField::VELOCITY: st.velocity = (uint8_t)constrain((int)st.velocity + d, 1, 127); break;
        case StepField::GATE:     st.gate = (uint8_t)constrain((int)st.gate + d, 1, 200); break;
        case StepField::PROB:     st.probability = (uint8_t)constrain((int)st.probability + d, 0, 100); break;
        case StepField::MICRO:    st.micro = (int8_t)constrain((int)st.micro + d, -50, 50); break;
        case StepField::RATCHET:  st.ratchet = (uint8_t)constrain((int)st.ratchet + (d > 0 ? 1 : -1), 1, 8); break;
        case StepField::ACCENT:   st.flags ^= ST_ACCENT; _editing = false; break;
        case StepField::SLIDE:    st.flags ^= ST_SLIDE;  _editing = false; break;
        case StepField::TIE:      st.flags ^= ST_TIE;    _editing = false; break;
        default: break;
        }
        break;
    }

    case Screen::PLOCK_EDIT: {
        Step& st = curStep();
        uint8_t slot = (uint8_t)(_row / 2);
        if (slot >= STEP_LOCKS) break;
        if ((_row & 1) == 0) {
            // Parameter selector: -1 means "no lock", then every lockable id.
            int cur = (st.lockId[slot] == LOCK_NONE) ? -1 : (int)st.lockId[slot];
            int next = cur;
            for (int guard = 0; guard < NUM_ROMPLER_PARAMS + 2; guard++) {
                next += (d > 0 ? 1 : -1);
                if (next < -1) next = NUM_ROMPLER_PARAMS - 1;
                if (next >= NUM_ROMPLER_PARAMS) next = -1;
                if (next == -1 || paramIsLockable((uint8_t)next)) break;
            }
            if (next < 0) st.lockId[slot] = LOCK_NONE;
            else {
                st.lockId[slot] = (uint8_t)next;
                if (cur < 0) st.lockVal[slot] = (uint8_t)(getTrackParam(*_track, (uint8_t)next) * 255.0f);
            }
        } else {
            st.lockVal[slot] = (uint8_t)constrain((int)st.lockVal[slot] + d * 4, 0, 255);
        }
        break;
    }

    case Screen::PATTERN_OPTS: {
        Pattern& p = pat();
        switch (_row) {
        case 0: p.length = (uint8_t)constrain((int)p.length + d, 1, NUM_STEPS); break;
        case 1: p.swing  = (uint8_t)constrain((int)p.swing + d, 0, SWING_MAX); break;
        case 2: p.subdiv = (uint8_t)constrain((int)p.subdiv + (d > 0 ? 1 : -1), 0, NUM_SUBDIVS - 1);
                seq().refreshTiming(); break;
        case 3: p.scaleIdx = (uint8_t)constrain((int)p.scaleIdx + (d > 0 ? 1 : -1), 0, NUM_SCALES - 1); break;
        case 4: p.rootNote = (uint8_t)(((int)p.rootNote + (d > 0 ? 1 : 11)) % 12); break;
        case 5: p.transpose = (int8_t)constrain((int)p.transpose + (d > 0 ? 1 : -1), -24, 24); break;
        case 6: p.midiChannel = (uint8_t)constrain((int)p.midiChannel + (d > 0 ? 1 : -1), 1, 16); break;
        }
        break;
    }

    case Screen::CHAIN_EDIT: {
        if (_row >= CHAIN_LEN) break;
        ChainEntry& c = _gs->track[*_track].chain[_row];
        if (_shift) c.repeats = (uint8_t)constrain((int)c.repeats + (d > 0 ? 1 : -1), 1, 16);
        else {
            int v = c.patternIdx + (d > 0 ? 1 : -1);
            if (v < -1) v = NUM_PATTERNS - 1;
            if (v >= NUM_PATTERNS) v = -1;
            c.patternIdx = (int8_t)v;
            if (c.repeats == 0) c.repeats = 1;
        }
        break;
    }

    case Screen::TOOLS:
        switch (_row) {
        case 0: _eucHits = (uint8_t)constrain((int)_eucHits + (d > 0 ? 1 : -1), 0, NUM_STEPS); break;
        case 1: _eucRot  = (uint8_t)constrain((int)_eucRot + (d > 0 ? 1 : -1), 0, NUM_STEPS - 1); break;
        case 3: _randDensity = (uint8_t)constrain((int)_randDensity + d, 0, 100); break;
        case 5: _humanAmt = (uint8_t)constrain((int)_humanAmt + d, 0, 60); break;
        }
        break;

    case Screen::KIT_SAVE:
        if (_row == 0) {
            _saveSlot = (uint8_t)constrain((int)_saveSlot + (d > 0 ? 1 : -1), 0, KIT_SLOTS - 1);
        } else if (_row == 1) {
            // Name editor: turn picks the character, SHIFT+turn moves the caret.
            if (_shift) {
                _nameCursor = (uint8_t)constrain((int)_nameCursor + (d > 0 ? 1 : -1), 0, 12);
            } else {
                static const char CHARSET[] =
                    " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.";
                const int n = (int)sizeof(CHARSET) - 1;
                const char* p = strchr(CHARSET, _nameBuf[_nameCursor]);
                int idx = (p && *p) ? (int)(p - CHARSET) : 0;
                idx = (idx + (d > 0 ? 1 : n - 1)) % n;
                _nameBuf[_nameCursor] = CHARSET[idx];
                _nameBuf[13] = 0;
            }
        }
        break;

    case Screen::MIXER: {
        uint8_t track = (uint8_t)(_row / NUM_ROMPLER_PARAMS);
        uint8_t param = (uint8_t)(_row % NUM_ROMPLER_PARAMS);
        float v = getTrackParam(track, param);
        setTrackParam(track, param, constrain(v + (float)d * 0.01f, 0.0f, 1.0f));
        break;
    }

    case Screen::MIDI_SET:
        switch (_row) {
        case 0: _gs->midiInChannel = (uint8_t)constrain((int)_gs->midiInChannel + (d > 0 ? 1 : -1), 0, 16);
                midi.setInChannel(_gs->midiInChannel); break;
        case 1: _gs->midiOutEnable = !_gs->midiOutEnable; midi.setOutEnabled(_gs->midiOutEnable);
                _editing = false; break;
        case 2: _gs->midiClockOut = !_gs->midiClockOut; uiApplySync(); _editing = false; break;
        case 3: _gs->extSync = !_gs->extSync; uiApplySync(); _editing = false; break;
        case 4: _gs->midiThru = !_gs->midiThru; _editing = false; break;
        }
        break;

    case Screen::SETTINGS:
        switch (_row) {
        case 0: g_masterVol = constrain(g_masterVol + (float)d * 0.02f, 0.0f, 2.0f);
                _gs->masterVol = (uint8_t)constrain(g_masterVol * 128.0f, 0.0f, 255.0f); break;
        case 1: _gs->contrast = (uint8_t)constrain((int)_gs->contrast + d * 4, 0, 255);
                _oled.setContrast(_gs->contrast); break;
        case 2: _gs->metronome = !_gs->metronome; _editing = false; break;
        }
        break;

    default: break;
    }
    _dirty = true;
}

// ─── Drawing ────────────────────────────────────────────────────────────────
void UI::update() {
    unsigned long now = millis();
    // Redraw on a fixed cadence while playing (the playhead animates) and only
    // on demand when stopped, so a stopped machine leaves the I2C bus alone.
    bool animating = (seq().playState() == PlayState::PLAYING) || (_toastUntil > now);
    if (!_dirty && !animating) return;
    if (now - _lastDraw < UI_REFRESH_MS) return;
    _lastDraw = now;
    _dirty = false;
    drawAll();
}

void UI::drawBar(int x, int y, int w, float v) {
    _oled.drawFrame(x, y, w, 6);
    int fw = (int)(v * (float)(w - 2) + 0.5f);
    fw = constrain(fw, 0, w - 2);
    if (fw > 0) _oled.drawBox(x + 1, y + 1, fw, 4);
}

void UI::drawListRow(int y, bool sel, const char* label, const char* value) {
    if (sel) {
        _oled.drawBox(0, y, DISP_W, ROW_H - 1);
        _oled.setDrawColor(0);
    }
    _oled.drawStr(2, y + 8, label);
    if (value) {
        int w = _oled.getStrWidth(value);
        _oled.drawStr(DISP_W - 3 - w, y + 8, value);
    }
    if (sel) _oled.setDrawColor(1);
}

void UI::drawHeader() {
    _oled.setFont(u8g2_font_5x7_tr);
    char buf[26];
    Sequencer& s = seq();
    // T1.P1  120  > R
    snprintf(buf, sizeof(buf), "T%u%c P%u %u",
             (unsigned)(*_track + 1), s.muted() ? 'x' : '.',
             (unsigned)(s.currentPattern() + 1), (unsigned)s.bpm());
    _oled.drawStr(0, 7, buf);

    int x = 66;
    if (s.playState() == PlayState::PLAYING) {
        _oled.drawTriangle(x, 1, x, 7, x + 6, 4);   // play
        x += 9;
    } else if (s.playState() == PlayState::PAUSED) {
        _oled.drawBox(x, 1, 2, 7); _oled.drawBox(x + 3, 1, 2, 7);
        x += 8;
    }
    if (g_recArm) { _oled.drawDisc(x + 3, 4, 3); x += 9; }
    if (s.externalSync()) { _oled.drawStr(x, 7, "EXT"); x += 17; }
    else if (s.midiClockOut()) { _oled.drawStr(x, 7, "CLK"); x += 17; }

    _oled.drawHLine(0, 9, DISP_W);
}

void UI::drawFooter(const char* hint) {
    _oled.drawHLine(0, FOOTER_Y, DISP_W);
    _oled.setFont(u8g2_font_5x7_tr);
    if (_toastUntil > millis()) {
        int w = _oled.getStrWidth(_toastMsg);
        _oled.drawStr((DISP_W - w) / 2, FOOTER_Y + 9, _toastMsg);
    } else if (hint) {
        _oled.drawStr(1, FOOTER_Y + 9, hint);
    }
    if (_editing) {
        _oled.drawStr(DISP_W - 22, FOOTER_Y + 9, "EDIT");
    }
}

void UI::drawStepCell(uint8_t step, int x, int y, bool cursor, bool playing) {
    const Step& st = pat().steps[step];
    bool inLen = step < pat().length;

    if (playing) _oled.drawBox(x, y, STEP_CELL_W - 1, STEP_CELL_H - 1);
    else         _oled.drawFrame(x, y, STEP_CELL_W - 1, STEP_CELL_H - 1);

    if (playing) _oled.setDrawColor(0);

    if (st.active()) {
        // Filled block = note; the height encodes velocity so the whole page
        // reads as a dynamics contour at a glance.
        int h = 2 + (int)st.velocity * (STEP_CELL_H - 6) / 127;
        _oled.drawBox(x + 3, y + STEP_CELL_H - 3 - h, STEP_CELL_W - 7, h);
        if (st.accent()) _oled.drawPixel(x + 2, y + 2);
        if (st.slide())  _oled.drawPixel(x + STEP_CELL_W - 4, y + 2);
        if (st.ratchet > 1) _oled.drawPixel(x + STEP_CELL_W - 4, y + STEP_CELL_H - 4);
        if (st.lockId[0] != LOCK_NONE) _oled.drawPixel(x + 2, y + STEP_CELL_H - 4);
    } else if (!inLen) {
        _oled.drawPixel(x + STEP_CELL_W / 2 - 1, y + STEP_CELL_H / 2 - 1);
    }

    if (playing) _oled.setDrawColor(1);
    if (cursor) _oled.drawFrame(x - 1, y - 1, STEP_CELL_W + 1, STEP_CELL_H + 1);
}

void UI::drawMain() {
    Sequencer& s = seq();
    uint8_t page = _selStep / STEPS_PER_PAGE;
    uint8_t base = page * STEPS_PER_PAGE;

    for (uint8_t i = 0; i < STEPS_PER_PAGE; i++) {
        uint8_t st = base + i;
        if (st >= NUM_STEPS) break;
        int x = (i % 8) * STEP_CELL_W;
        int y = (i < 8) ? STEP_ROW1_Y : STEP_ROW2_Y;
        bool playing = (s.playState() == PlayState::PLAYING) && (st == s.currentStep());
        drawStepCell(st, x, y, st == _selStep, playing);
    }

    _oled.setFont(u8g2_font_5x7_tr);
    char buf[30];
    const Pattern& p = pat();
    snprintf(buf, sizeof(buf), "PG%u/%u L%u %s%s",
             (unsigned)(page + 1), (unsigned)((p.length + STEPS_PER_PAGE - 1) / STEPS_PER_PAGE),
             (unsigned)p.length, NOTE_NAMES[p.rootNote % 12], SCALES[p.scaleIdx].name);
    _oled.drawStr(0, 45, buf);

    const Step& st = pat().steps[_selStep];
    char nb[8]; noteName(st.notes[0], nb);
    snprintf(buf, sizeof(buf), "S%02u %s v%u", (unsigned)(_selStep + 1), nb, (unsigned)st.velocity);
    int w = _oled.getStrWidth(buf);
    _oled.drawStr(DISP_W - w, 45, buf);

    drawFooter(_shift ? "SH:BPM/PAT/REC" : "PAGE=MENU  PUSH=EDIT");
}

void UI::drawMenu() {
    _oled.setFont(u8g2_font_6x10_tr);
    for (int i = 0; i < VIS_ROWS; i++) {
        int idx = _scroll + i;
        if (idx >= (int)MenuItem::COUNT) break;
        drawListRow(CONTENT_Y + i * ROW_H, idx == _row, MENU_LABELS[idx], nullptr);
    }
    drawFooter("PUSH=OPEN  PAGE=BACK");
}

void UI::drawStepEdit() {
    _oled.setFont(u8g2_font_6x10_tr);
    Step& st = curStep();
    char val[16];

    for (int i = 0; i < VIS_ROWS; i++) {
        int idx = _scroll + i;
        if (idx >= (int)StepField::COUNT) break;
        val[0] = 0;
        switch ((StepField)idx) {
        case StepField::ACTIVE:  snprintf(val, sizeof(val), "%s", st.active() ? "ON" : "OFF"); break;
        case StepField::NOTE1: case StepField::NOTE2:
        case StepField::NOTE3: case StepField::NOTE4: {
            uint8_t n = (uint8_t)idx - (uint8_t)StepField::NOTE1;
            if (n < st.noteCount) noteName(st.notes[n], val);
            else                  snprintf(val, sizeof(val), "--");
            break;
        }
        case StepField::NOTECOUNT: snprintf(val, sizeof(val), "%u", (unsigned)st.noteCount); break;
        case StepField::VELOCITY:  snprintf(val, sizeof(val), "%u", (unsigned)st.velocity); break;
        case StepField::GATE:      snprintf(val, sizeof(val), "%u%%", (unsigned)st.gate); break;
        case StepField::PROB:      snprintf(val, sizeof(val), "%u%%", (unsigned)st.probability); break;
        case StepField::MICRO:     snprintf(val, sizeof(val), "%+d", (int)st.micro); break;
        case StepField::RATCHET:   snprintf(val, sizeof(val), "x%u", (unsigned)st.ratchet); break;
        case StepField::ACCENT:    snprintf(val, sizeof(val), "%s", st.accent() ? "ON" : "OFF"); break;
        case StepField::SLIDE:     snprintf(val, sizeof(val), "%s", st.slide() ? "ON" : "OFF"); break;
        case StepField::TIE:       snprintf(val, sizeof(val), "%s", st.tie() ? "ON" : "OFF"); break;
        case StepField::LOCKS: {
            uint8_t used = 0;
            for (uint8_t k = 0; k < STEP_LOCKS; k++) if (st.lockId[k] != LOCK_NONE) used++;
            snprintf(val, sizeof(val), "%u/%u", (unsigned)used, (unsigned)STEP_LOCKS);
            break;
        }
        default: break;
        }
        drawListRow(CONTENT_Y + i * ROW_H, idx == _row, STEP_FIELD_LABELS[idx], val);
    }

    char hint[26];
    snprintf(hint, sizeof(hint), "STEP %02u  TRK=NEXT", (unsigned)(_selStep + 1));
    drawFooter(hint);
}

void UI::drawStepLocks() {
    _oled.setFont(u8g2_font_6x10_tr);
    Step& st = curStep();
    char lbl[16], val[16];

    for (int i = 0; i < VIS_ROWS; i++) {
        int idx = _scroll + i;
        if (idx >= rowCount()) break;
        if (idx == STEP_LOCKS * 2) {
            drawListRow(CONTENT_Y + i * ROW_H, idx == _row, "CLEAR ALL", nullptr);
            continue;
        }
        uint8_t slot = (uint8_t)(idx / 2);
        if ((idx & 1) == 0) {
            snprintf(lbl, sizeof(lbl), "L%u Param", (unsigned)(slot + 1));
            if (st.lockId[slot] == LOCK_NONE) snprintf(val, sizeof(val), "--");
            else snprintf(val, sizeof(val), "%s", ROMPLER_PARAM_INFO[st.lockId[slot]].name);
        } else {
            snprintf(lbl, sizeof(lbl), "L%u Value", (unsigned)(slot + 1));
            if (st.lockId[slot] == LOCK_NONE) snprintf(val, sizeof(val), "--");
            else snprintf(val, sizeof(val), "%u", (unsigned)(st.lockVal[slot] * 100 / 255));
        }
        drawListRow(CONTENT_Y + i * ROW_H, idx == _row, lbl, val);
    }
    drawFooter("PARAM LOCKS");
}

void UI::drawPatternSel() {
    _oled.setFont(u8g2_font_6x10_tr);
    char lbl[16], val[16];
    for (int i = 0; i < VIS_ROWS; i++) {
        int idx = _scroll + i;
        if (idx >= NUM_PATTERNS) break;
        Pattern& p = seq().getPattern((uint8_t)idx);
        uint8_t used = 0;
        for (uint8_t s = 0; s < p.length; s++) if (p.steps[s].active()) used++;
        snprintf(lbl, sizeof(lbl), "PATTERN %d%s", idx + 1,
                 (idx == seq().currentPattern()) ? " *" : "");
        snprintf(val, sizeof(val), "%u/%u", (unsigned)used, (unsigned)p.length);
        drawListRow(CONTENT_Y + i * ROW_H, idx == _row, lbl, val);
    }
    drawFooter("PUSH=SELECT");
}

void UI::drawPatternOpts() {
    _oled.setFont(u8g2_font_6x10_tr);
    Pattern& p = pat();
    char val[16];
    for (int i = 0; i < VIS_ROWS; i++) {
        int idx = _scroll + i;
        if (idx >= NUM_PATOPT_ROWS) break;
        switch (idx) {
        case 0: snprintf(val, sizeof(val), "%u", (unsigned)p.length); break;
        case 1: snprintf(val, sizeof(val), "%u%%", (unsigned)p.swing); break;
        case 2: snprintf(val, sizeof(val), "%s", SUBDIVS[p.subdiv % NUM_SUBDIVS].name); break;
        case 3: snprintf(val, sizeof(val), "%s", SCALES[p.scaleIdx].name); break;
        case 4: snprintf(val, sizeof(val), "%s", NOTE_NAMES[p.rootNote % 12]); break;
        case 5: snprintf(val, sizeof(val), "%+d", (int)p.transpose); break;
        case 6: snprintf(val, sizeof(val), "%u", (unsigned)p.midiChannel); break;
        }
        drawListRow(CONTENT_Y + i * ROW_H, idx == _row, PATOPT_LABELS[idx], val);
    }
    drawFooter("PATTERN OPTIONS");
}

void UI::drawChain() {
    _oled.setFont(u8g2_font_6x10_tr);
    char lbl[16], val[16];
    for (int i = 0; i < VIS_ROWS; i++) {
        int idx = _scroll + i;
        if (idx > CHAIN_LEN) break;
        if (idx == CHAIN_LEN) {
            drawListRow(CONTENT_Y + i * ROW_H, idx == _row,
                        seq().isChaining() ? "APPLY / OFF" : "APPLY", nullptr);
            continue;
        }
        ChainEntry& c = _gs->track[*_track].chain[idx];
        snprintf(lbl, sizeof(lbl), "SLOT %d", idx + 1);
        if (c.patternIdx < 0) snprintf(val, sizeof(val), "--");
        else snprintf(val, sizeof(val), "P%d x%u", c.patternIdx + 1, (unsigned)c.repeats);
        drawListRow(CONTENT_Y + i * ROW_H, idx == _row, lbl, val);
    }
    drawFooter("SH+TURN = REPEATS");
}

void UI::drawTools() {
    _oled.setFont(u8g2_font_6x10_tr);
    char val[16];
    for (int i = 0; i < VIS_ROWS; i++) {
        int idx = _scroll + i;
        if (idx >= NUM_TOOL_ROWS) break;
        val[0] = 0;
        switch (idx) {
        case 0: snprintf(val, sizeof(val), "%u", (unsigned)_eucHits); break;
        case 1: snprintf(val, sizeof(val), "%u", (unsigned)_eucRot); break;
        case 3: snprintf(val, sizeof(val), "%u%%", (unsigned)_randDensity); break;
        case 5: snprintf(val, sizeof(val), "%u", (unsigned)_humanAmt); break;
        default: break;
        }
        drawListRow(CONTENT_Y + i * ROW_H, idx == _row, TOOL_LABELS[idx],
                    val[0] ? val : nullptr);
    }
    drawFooter("PATTERN TOOLS");
}

void UI::drawPatchBrowse() {
    _oled.setFont(u8g2_font_6x10_tr);
    if (g_patchCount == 0) {
        _oled.drawStr(4, 30, "NO PATCHES ON SD");
        _oled.setFont(u8g2_font_5x7_tr);
        _oled.drawStr(4, 42, "see /patches/*/patch.cfg");
        drawFooter("SETTINGS>RESCAN SD");
        return;
    }
    const char* curDir = patchForTrack(*_track).loaded ? patchForTrack(*_track).info.dirName : "";
    char val[6];
    for (int i = 0; i < VIS_ROWS; i++) {
        int idx = _scroll + i;
        if (idx >= (int)g_patchCount) break;
        bool isCur = strcmp(g_patchList[idx].dirName, curDir) == 0 && curDir[0] != 0;
        snprintf(val, sizeof(val), "%s", isCur ? "*" : "");
        drawListRow(CONTENT_Y + i * ROW_H, idx == _row, g_patchList[idx].name, val[0] ? val : nullptr);
    }
    char hint[26];
    TrackPatch& tp = patchForTrack(*_track);
    snprintf(hint, sizeof(hint), "T%u: %s", (unsigned)(*_track + 1), tp.loaded ? tp.info.name : "empty");
    drawFooter(hint);
}

void UI::drawKits() {
    _oled.setFont(u8g2_font_6x10_tr);
    char lbl[16], val[16];
    for (int i = 0; i < VIS_ROWS; i++) {
        int idx = _scroll + i;
        if (idx >= KIT_SLOTS) break;
        snprintf(lbl, sizeof(lbl), "KIT %02d", idx + 1);
        Kit k;
        if (_store->loadKit((uint8_t)idx, k)) snprintf(val, sizeof(val), "%s", k.name);
        else snprintf(val, sizeof(val), "--");
        drawListRow(CONTENT_Y + i * ROW_H, idx == _row, lbl, val);
    }
    drawFooter("PUSH=LOAD  SH+PUSH=SAVE");
}

void UI::drawKitSave() {
    _oled.setFont(u8g2_font_6x10_tr);
    char val[16];
    for (int i = 0; i < 3; i++) {
        const char* lbl = (i == 0) ? "Slot" : (i == 1) ? "Name" : "SAVE";
        val[0] = 0;
        if (i == 0) snprintf(val, sizeof(val), "K%02u", (unsigned)(_saveSlot + 1));
        else if (i == 1) snprintf(val, sizeof(val), "%s", _nameBuf);
        drawListRow(CONTENT_Y + i * ROW_H, i == _row, lbl, val[0] ? val : nullptr);
    }
    // Caret under the character being edited.
    if (_row == 1 && _editing) {
        int w = _oled.getStrWidth(_nameBuf);
        int x = DISP_W - 3 - w + _nameCursor * 6;
        _oled.drawHLine(x, CONTENT_Y + ROW_H + 9, 5);
    }
    drawFooter("SH+TURN = MOVE CARET");
}

void UI::drawMixer() {
    _oled.setFont(u8g2_font_6x10_tr);
    char lbl[18], val[16];
    for (int i = 0; i < VIS_ROWS; i++) {
        int idx = _scroll + i;
        if (idx >= NUM_TRACKS * NUM_ROMPLER_PARAMS) break;
        uint8_t track = (uint8_t)(idx / NUM_ROMPLER_PARAMS);
        uint8_t param = (uint8_t)(idx % NUM_ROMPLER_PARAMS);
        snprintf(lbl, sizeof(lbl), "T%u%s %s", (unsigned)(track + 1),
                 _seqs[track].muted() ? "m" : "", ROMPLER_PARAM_INFO[param].name);
        formatTrackParam(track, param, val, sizeof(val));
        drawListRow(CONTENT_Y + i * ROW_H, idx == _row, lbl, val);
    }
    drawFooter("MUTE BTN = MUTE ROW TRK");
}

void UI::drawMidiSet() {
    _oled.setFont(u8g2_font_6x10_tr);
    char val[10];
    for (int i = 0; i < VIS_ROWS; i++) {
        int idx = _scroll + i;
        if (idx >= NUM_MIDI_ROWS) break;
        val[0] = 0;
        switch (idx) {
        case 0: if (_gs->midiInChannel == 0) snprintf(val, sizeof(val), "OMNI");
                else snprintf(val, sizeof(val), "%u", (unsigned)_gs->midiInChannel);
                break;
        case 1: snprintf(val, sizeof(val), "%s", _gs->midiOutEnable ? "ON" : "OFF"); break;
        case 2: snprintf(val, sizeof(val), "%s", _gs->midiClockOut ? "ON" : "OFF"); break;
        case 3: snprintf(val, sizeof(val), "%s", _gs->extSync ? "ON" : "OFF"); break;
        case 4: snprintf(val, sizeof(val), "%s", _gs->midiThru ? "ON" : "OFF"); break;
        default: break;
        }
        drawListRow(CONTENT_Y + i * ROW_H, idx == _row, MIDI_LABELS[idx], val[0] ? val : nullptr);
    }
    drawFooter("MIDI SETTINGS");
}

void UI::drawSettings() {
    _oled.setFont(u8g2_font_6x10_tr);
    char val[10];
    for (int i = 0; i < VIS_ROWS; i++) {
        int idx = _scroll + i;
        if (idx >= NUM_SETTING_ROWS) break;
        val[0] = 0;
        switch (idx) {
        case 0: snprintf(val, sizeof(val), "%d%%", (int)(g_masterVol * 100.0f)); break;
        case 1: snprintf(val, sizeof(val), "%u", (unsigned)_gs->contrast); break;
        case 2: snprintf(val, sizeof(val), "%s", _gs->metronome ? "ON" : "OFF"); break;
        default: break;
        }
        drawListRow(CONTENT_Y + i * ROW_H, idx == _row, SETTING_LABELS[idx], val[0] ? val : nullptr);
    }
    drawFooter(_store->mounted() ? "SD OK" : "NO SD CARD");
}

void UI::drawPerf() {
    _oled.setFont(u8g2_font_6x10_tr);
    char lbl[10], val[20];
    for (int i = 0; i < VIS_ROWS; i++) {
        int idx = _scroll + i;
        if (idx >= NUM_PERF_ROWS) break;
        switch (idx) {
        case 0: snprintf(lbl, sizeof(lbl), "Heap"); snprintf(val, sizeof(val), "%uK", (unsigned)(ESP.getFreeHeap() / 1024)); break;
        case 1: snprintf(lbl, sizeof(lbl), "PSRAM");
                if (psramFound()) snprintf(val, sizeof(val), "%uK", (unsigned)(ESP.getFreePsram() / 1024));
                else snprintf(val, sizeof(val), "none");
                break;
        case 2: snprintf(lbl, sizeof(lbl), "SD Card"); snprintf(val, sizeof(val), "%s", _store->mounted() ? "OK" : "--"); break;
        case 3: snprintf(lbl, sizeof(lbl), "Patches"); snprintf(val, sizeof(val), "%u", (unsigned)g_patchCount); break;
        default: {
            uint8_t t = (uint8_t)(idx - 4);
            snprintf(lbl, sizeof(lbl), "T%u", (unsigned)(t + 1));
            TrackPatch& tp = patchForTrack(t);
            snprintf(val, sizeof(val), "%s", tp.loaded ? tp.info.name : "--");
            break;
        }
        }
        drawListRow(CONTENT_Y + i * ROW_H, idx == _row, lbl, val);
    }
    drawFooter("SYSTEM");
}

void UI::drawHelp() {
    _oled.setFont(u8g2_font_5x7_tr);
    for (int i = 0; i < VIS_ROWS; i++) {
        int idx = _scroll + i;
        if (idx >= NUM_HELP_ROWS) break;
        _oled.drawStr(2, CONTENT_Y + i * ROW_H + 7, HELP_LINES[idx]);
    }
    drawFooter("AMY ROMPLER");
}

void UI::drawAll() {
    _oled.clearBuffer();
    if (_screen != Screen::MAIN) drawHeader();
    switch (_screen) {
    case Screen::MAIN:         drawMain(); break;
    case Screen::MENU:         drawMenu(); break;
    case Screen::STEP_EDIT:    drawStepEdit(); break;
    case Screen::PLOCK_EDIT:   drawStepLocks(); break;
    case Screen::PATTERN_SEL:  drawPatternSel(); break;
    case Screen::PATTERN_OPTS: drawPatternOpts(); break;
    case Screen::CHAIN_EDIT:   drawChain(); break;
    case Screen::TOOLS:        drawTools(); break;
    case Screen::PATCH_BROWSE: drawPatchBrowse(); break;
    case Screen::KITS:         drawKits(); break;
    case Screen::KIT_SAVE:     drawKitSave(); break;
    case Screen::MIXER:        drawMixer(); break;
    case Screen::MIDI_SET:     drawMidiSet(); break;
    case Screen::SETTINGS:     drawSettings(); break;
    case Screen::PERF:         drawPerf(); break;
    case Screen::HELP:         drawHelp(); break;
    }
    _oled.sendBuffer();
}
