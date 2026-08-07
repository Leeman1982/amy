#pragma once
// ============================================================================
//  controls.h — rotary encoder (ISR quadrature decode + debounced push) and
//  momentary buttons with short/long-press detection.
//
//  Ported from xtro/controls.h. The debounce/long-press/quadrature logic is
//  plain digitalRead()/attachInterrupt()/millis(), so it is unchanged from the
//  RP2350 original — only config.h's pin numbers differ. ESP32-S3 supports a
//  GPIO interrupt on essentially any pin, so attachInterrupt() ports directly.
// ============================================================================
#include <Arduino.h>
#include "config.h"

class RotaryEncoder {
public:
    RotaryEncoder(uint8_t pinA, uint8_t pinB, uint8_t pinSW);
    void begin();
    int  getDelta();            // detents since last call (signed)
    void update();              // poll push switch (call from loop)
    bool wasPressed();          // short press (consumed)
    bool wasLongPress();        // long press (consumed)
    bool isHeld();

    static void isrA();
    static void isrB();

private:
    uint8_t _pinA, _pinB, _pinSW;
    volatile int _count = 0;
    volatile uint8_t _last = 0;
    // push switch state
    bool _swRaw = false, _swState = false, _longFired = false;
    bool _pressedFlag = false, _longFlag = false;
    unsigned long _swDebounceT = 0, _swPressT = 0;
};

class Button {
public:
    explicit Button(uint8_t pin, bool activeLow = true);
    void begin();
    void update();
    bool wasPressed();
    bool wasReleased();
    bool wasLongPress();
    bool isHeld();

private:
    uint8_t _pin; bool _activeLow;
    bool _lastState = false, _state = false;
    bool _pressedFlag = false, _releasedFlag = false;
    bool _longFired = false, _lpReported = false;
    unsigned long _lastDebounce = 0, _pressTime = 0;
};

class Controls {
public:
    Controls();
    void begin();
    void update();

    RotaryEncoder encoder;
    Button play, rec, track, page, shift;
};
