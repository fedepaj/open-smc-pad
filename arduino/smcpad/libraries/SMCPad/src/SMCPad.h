/*
 * SMCPad: the M-Vave SMC-PAD's controls and lights.
 *
 *   pads     0..15, 0 = bottom-left, row by row upwards (MPC style)
 *   buttons  SMCPad.BLUETOOTH, PAD_BANK, KNOB_BANK, LEFT, RIGHT, PLAY, PAUSE, REC, SHIFT,
 *            NOTE_REPEAT (panel order, left to right); each has an LED
 *   knobs    0..7 = knobs 1..8 (endless), +1 per step clockwise
 *
 * The hardware is scanned in the background (pads ~300 times a second each, keys every 4 ms);
 * call SMCPad.update() in loop() to have the callbacks called, or read the events yourself
 * with SMCPad.read().
 */
#pragma once
#include <Arduino.h>

class SMCPadClass {
public:
    enum Button {
        BLUETOOTH, PAD_BANK, KNOB_BANK, LEFT, RIGHT, PLAY, PAUSE, REC, SHIFT, NOTE_REPEAT,
    };
    static const int PADS = SMCPAD_NUM_PADS;
    static const int BUTTONS = SMCPAD_NUM_BUTTONS;
    static const int KNOBS = SMCPAD_NUM_ENCODERS;

    typedef void (*PadCallback)(int pad, int velocity);
    typedef void (*IndexCallback)(int index);
    typedef void (*KnobCallback)(int knob, int delta);

    void begin() { smcpad_hw_begin(); }
    void update();
    bool read(smcpad_event_t &event) { return smcpad_event_get(&event); }

    void onPadPress(PadCallback cb) { padPress = cb; }
    void onPadRelease(IndexCallback cb) { padRelease = cb; }
    void onButtonPress(IndexCallback cb) { buttonPress = cb; }
    void onButtonRelease(IndexCallback cb) { buttonRelease = cb; }
    void onKnob(KnobCallback cb) { knobTurn = cb; }

    int padPressure(int pad) { return smcpad_pad_raw(pad); }    /* 0..1023, 0 at rest */
    bool button(int button) { return smcpad_button(button); }
    long knob(int knob) { return smcpad_encoder(knob); }

    void buttonLed(int button, bool on) { smcpad_led_set(button, on); }
    void padColor(int pad, uint32_t rgb) { smcpad_rgb_set(pad, rgb); }
    void padColor(int pad, uint8_t r, uint8_t g, uint8_t b) { smcpad_rgb_set(pad, (uint32_t)r << 16 | (uint32_t)g << 8 | b); }
    void clearPads();
    void show() { smcpad_rgb_show(); }      /* sends the pad colours set so far */

private:
    PadCallback padPress = nullptr;
    IndexCallback padRelease = nullptr, buttonPress = nullptr, buttonRelease = nullptr;
    KnobCallback knobTurn = nullptr;
};

extern SMCPadClass SMCPad;
