#pragma once
#include <Arduino.h>
#include "config.h"

// ============================================================================
//  midi.h — MIDI OUT/IN over a HardwareSerial UART.
//
//  Ported from xtro/midi.h. xtro needed arduino-pico's SerialPIO because
//  MIDI OUT had to land on a GPIO that isn't a hardware UART TX pin on
//  RP2350. ESP32-S3's GPIO matrix routes any of its UARTs to any pin
//  directly, so this uses a plain HardwareSerial — no software-UART trick
//  needed. Everything above the transport (running-status parser/dispatch,
//  message builders) is unchanged.
// ============================================================================

// Real-time status bytes
#define MIDI_RT_CLOCK     0xF8
#define MIDI_RT_START     0xFA
#define MIDI_RT_CONTINUE  0xFB
#define MIDI_RT_STOP      0xFC
#define MIDI_RT_ACTIVE    0xFE
#define MIDI_RT_RESET     0xFF

typedef void (*MidiNoteFn)(uint8_t ch, uint8_t note, uint8_t vel);
typedef void (*MidiCCFn)(uint8_t ch, uint8_t cc, uint8_t val);
typedef void (*MidiPCFn)(uint8_t ch, uint8_t prog);
typedef void (*MidiBendFn)(uint8_t ch, int16_t bend14);      // -8192..8191
typedef void (*MidiRTFn)(uint8_t status);

class MidiIO {
public:
    void begin();

    // ── OUT ────────────────────────────────────────────────────────────────
    void noteOn(uint8_t ch, uint8_t note, uint8_t vel);
    void noteOff(uint8_t ch, uint8_t note);
    void controlChange(uint8_t ch, uint8_t cc, uint8_t val);
    void programChange(uint8_t ch, uint8_t prog);
    void pitchBend(uint8_t ch, int16_t value);
    void allNotesOff(uint8_t ch);
    void clock();
    void start();
    void stop();
    void continueMsg();

    void setOutEnabled(bool en) { _outEnabled = en; }
    bool outEnabled() const     { return _outEnabled; }

    // ── IN ─────────────────────────────────────────────────────────────────
    void poll();                       // call often; drains the RX FIFO
    void setNoteOn(MidiNoteFn f)  { _onNoteOn = f; }
    void setNoteOff(MidiNoteFn f) { _onNoteOff = f; }
    void setCC(MidiCCFn f)        { _onCC = f; }
    void setPC(MidiPCFn f)        { _onPC = f; }
    void setBend(MidiBendFn f)    { _onBend = f; }
    void setRealtime(MidiRTFn f)  { _onRT = f; }

    // Input channel filter: 0 = omni, 1..16 = that channel only.
    void setInChannel(uint8_t ch) { _inChannel = (ch > 16) ? 0 : ch; }
    uint8_t inChannel() const     { return _inChannel; }

    bool activityIn()  { bool a = _actIn;  _actIn  = false; return a; }
    bool activityOut() { bool a = _actOut; _actOut = false; return a; }

private:
    bool    _outEnabled = true;
    uint8_t _inChannel  = 0;
    bool    _actIn = false, _actOut = false;

    // Running-status parser state
    uint8_t _status = 0, _data[2] = {0, 0}, _dataIdx = 0, _dataNeeded = 0;

    MidiNoteFn _onNoteOn = nullptr, _onNoteOff = nullptr;
    MidiCCFn   _onCC = nullptr;
    MidiPCFn   _onPC = nullptr;
    MidiBendFn _onBend = nullptr;
    MidiRTFn   _onRT = nullptr;

    void write1(uint8_t b);
    void write2(uint8_t a, uint8_t b);
    void write3(uint8_t a, uint8_t b, uint8_t c);
    void dispatch();
};
