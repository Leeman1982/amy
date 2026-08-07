#pragma once
#include <Arduino.h>
#include <U8g2lib.h>
#include "config.h"
#include "sequencer.h"
#include "sd_storage.h"
#include "patch.h"

// SH1106 128x64, full-buffer, hardware I2C. Ported from xtro/ui.h, which
// already parameterized this exact choice (config.h's OLED_DRIVER_* defines)
// — here it is simply the only option, since this board always carries a
// SH1106 panel.
typedef U8G2_SH1106_128X64_NONAME_F_HW_I2C SynthOled;

enum class Screen : uint8_t {
    MAIN = 0,
    MENU,
    STEP_EDIT,
    PLOCK_EDIT,
    PATTERN_SEL,
    PATTERN_OPTS,
    CHAIN_EDIT,
    TOOLS,
    PATCH_BROWSE,
    KITS,
    KIT_SAVE,
    MIXER,
    MIDI_SET,
    SETTINGS,
    PERF,
    HELP
};

// Main-menu order must match MENU_LABELS in ui.cpp.
enum class MenuItem : uint8_t {
    STEP_EDIT = 0, PATTERN, PAT_OPTS, CHAIN, TOOLS,
    PATCH, KITS, MIXER, MIDI, SETTINGS, PERF, HELP,
    COUNT
};

// STEP EDIT rows.
enum class StepField : uint8_t {
    ACTIVE = 0, NOTE1, NOTE2, NOTE3, NOTE4, NOTECOUNT,
    VELOCITY, GATE, PROB, MICRO, RATCHET,
    ACCENT, SLIDE, TIE, LOCKS,
    COUNT
};

class UI {
public:
    // Display bring-up is deliberately split from everything else and is the
    // FIRST thing setup() does, so a stall anywhere later still leaves a
    // readable screen behind it (see xtro/ui.cpp's original rationale).
    void beginDisplay();
    void bootStatus(const char* msg);
    void bind(Sequencer* seqs, uint8_t* activeTrack, SdStorage* storage, GlobalSettings* gs);
    void applyContrast(uint8_t c);

    void update();                 // throttled redraw
    void markDirty() { _dirty = true; }
    void toast(const char* msg);

    // ── Input events, dispatched from loop() ───────────────────────────────
    void onEncoder(int delta);
    void onEncPress();
    void onEncLong();
    void onPlay();       void onPlayLong();
    void onPage();       void onPageLong();
    void onTrack();      void onTrackLong();
    void onMute();       void onMuteLong();
    void setShift(bool held) { _shift = held; }
    bool shift() const { return _shift; }

    Screen screen() const { return _screen; }
    void   gotoScreen(Screen s);

private:
    // ── Bound state ────────────────────────────────────────────────────────
    Sequencer*      _seqs = nullptr;
    uint8_t*        _track = nullptr;
    SdStorage*      _store = nullptr;
    GlobalSettings* _gs = nullptr;
    SynthOled       _oled{U8G2_R0, U8X8_PIN_NONE};

    Sequencer& seq()      { return _seqs[*_track]; }
    Pattern&   pat()      { return seq().currentPatternRef(); }
    Step&      curStep()  { return pat().steps[_selStep % NUM_STEPS]; }

    // ── Navigation ─────────────────────────────────────────────────────────
    Screen  _screen = Screen::MAIN;
    bool    _dirty  = true;
    bool    _shift  = false;
    bool    _editing = false;         // encoder edits the value, not the cursor
    unsigned long _lastDraw = 0;

    int8_t  _row = 0;                 // selected row on the current screen
    int8_t  _scroll = 0;

    uint8_t _selStep = 0;
    uint8_t _saveSlot = 0;
    uint8_t _nameCursor = 0;
    char    _nameBuf[14] = "KIT 01";
    uint8_t _eucHits = 4, _eucRot = 0, _randDensity = 45, _humanAmt = 12;

    Pattern _clip;                    // pattern clipboard (copy/paste across tracks)
    bool    _clipFull = false;

    char          _toastMsg[22] = {0};
    unsigned long _toastUntil = 0;

    // ── Helpers ────────────────────────────────────────────────────────────
    int  rowCount() const;
    void moveRow(int delta);
    void editValue(int delta);
    void activateRow();               // encoder press on a non-edit row
    void ensureVisible(int visibleRows);

    void drawAll();
    void drawHeader();
    void drawFooter(const char* hint);
    void drawMain();
    void drawMenu();
    void drawStepEdit();
    void drawStepLocks();
    void drawPatternSel();
    void drawPatternOpts();
    void drawChain();
    void drawTools();
    void drawPatchBrowse();
    void drawKits();
    void drawKitSave();
    void drawMixer();
    void drawMidiSet();
    void drawSettings();
    void drawPerf();
    void drawHelp();
    void drawListRow(int y, bool sel, const char* label, const char* value);
    void drawBar(int x, int y, int w, float v);
    void drawStepCell(uint8_t step, int x, int y, bool cursor, bool playing);
};
