#include "controls.h"

// ─── RotaryEncoder ──────────────────────────────────────────────────────────
// Arduino-ESP32 ISRs are void(*)() with no argument (same restriction as
// arduino-pico); use a static instance ptr, same as the xtro original.
static RotaryEncoder* _encInstance = nullptr;

RotaryEncoder::RotaryEncoder(uint8_t pinA, uint8_t pinB, uint8_t pinSW)
    : _pinA(pinA), _pinB(pinB), _pinSW(pinSW) {}

void RotaryEncoder::begin() {
    _encInstance = this;
    pinMode(_pinA, INPUT_PULLUP);
    pinMode(_pinB, INPUT_PULLUP);
    pinMode(_pinSW, INPUT_PULLUP);
    _last = (digitalRead(_pinA) << 1) | digitalRead(_pinB);
    attachInterrupt(digitalPinToInterrupt(_pinA), isrA, CHANGE);
    attachInterrupt(digitalPinToInterrupt(_pinB), isrB, CHANGE);
}

void RotaryEncoder::isrA() {
    RotaryEncoder* e = _encInstance;
    if (!e) return;
    uint8_t a = digitalRead(e->_pinA);
    uint8_t b = digitalRead(e->_pinB);
    uint8_t s = (a << 1) | b;
    if (s != e->_last) {
        if ((e->_last == 0b11 && s == 0b01) || (e->_last == 0b01 && s == 0b00) ||
            (e->_last == 0b00 && s == 0b10) || (e->_last == 0b10 && s == 0b11))
            e->_count++;
        else
            e->_count--;
        e->_last = s;
    }
}
void RotaryEncoder::isrB() { isrA(); }

int RotaryEncoder::getDelta() {
    noInterrupts();
    int d = _count / ENC_TICKS_PER_DETENT;
    _count -= d * ENC_TICKS_PER_DETENT;
    interrupts();
    return d;
}

void RotaryEncoder::update() {
    bool raw = (digitalRead(_pinSW) == LOW);
    unsigned long now = millis();
    if (raw != _swRaw) { _swDebounceT = now; _swRaw = raw; }
    if ((now - _swDebounceT) > DEBOUNCE_MS) {
        if (raw && !_swState) { _swState = true; _swPressT = now; _longFired = false; }
        else if (!raw && _swState) {
            _swState = false;
            if (!_longFired) _pressedFlag = true;
        }
    }
    if (_swState && !_longFired && (now - _swPressT) >= LONG_PRESS_MS) {
        _longFired = true; _longFlag = true;
    }
}

bool RotaryEncoder::wasPressed()   { if (_pressedFlag) { _pressedFlag = false; return true; } return false; }
bool RotaryEncoder::wasLongPress() { if (_longFlag)    { _longFlag = false;    return true; } return false; }
bool RotaryEncoder::isHeld()       { return _swState; }

// ─── Button ─────────────────────────────────────────────────────────────────
Button::Button(uint8_t pin, bool activeLow) : _pin(pin), _activeLow(activeLow) {}

void Button::begin() { pinMode(_pin, _activeLow ? INPUT_PULLUP : INPUT_PULLDOWN); }

void Button::update() {
    uint8_t raw = digitalRead(_pin);
    bool active = _activeLow ? (raw == LOW) : (raw == HIGH);
    unsigned long now = millis();
    if (active != _lastState) _lastDebounce = now;
    _lastState = active;
    if ((now - _lastDebounce) > DEBOUNCE_MS) {
        if (active && !_state) { _state = true; _pressTime = now; _longFired = false; _lpReported = false; }
        else if (!active && _state) {
            _state = false; _releasedFlag = true;
            if (!_longFired) _pressedFlag = true;
        }
    }
    if (_state && !_longFired && (now - _pressTime) >= LONG_PRESS_MS) _longFired = true;
}

bool Button::wasPressed()  { if (_pressedFlag)  { _pressedFlag = false;  return true; } return false; }
bool Button::wasReleased() { if (_releasedFlag) { _releasedFlag = false; return true; } return false; }
bool Button::isHeld()      { return _state; }
bool Button::wasLongPress() {
    if (_longFired && !_lpReported) { _lpReported = true; return true; }
    if (!_state) _lpReported = false;
    return false;
}

// ─── Controls ───────────────────────────────────────────────────────────────
Controls::Controls()
    : encoder(PIN_ENC_A, PIN_ENC_B, PIN_ENC_SW),
      play(PIN_BTN_PLAY), rec(PIN_BTN_MUTE), track(PIN_BTN_TRACK),
      page(PIN_BTN_PAGE), shift(PIN_BTN_SHIFT) {}

void Controls::begin() {
    encoder.begin();
    play.begin(); rec.begin(); track.begin(); page.begin(); shift.begin();
}

void Controls::update() {
    encoder.update();
    play.update(); rec.update(); track.update(); page.update(); shift.update();
}
