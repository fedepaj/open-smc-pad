// What the user sets on the pad (preset, curve, transpose, note repeat...), kept in EEPROM.
#pragma once
#include <EEPROM.h>

enum { PRESET_PERFORMANCE = 0, PRESET_DAW = 1, NUM_PRESETS = 8 };
enum { CURVE_LINEAR = 0, CURVE_SOFT, CURVE_HARD, CURVE_FULL };

struct Settings {
  uint32_t magic;
  uint8_t preset;         // 0..7 (0 Performance, 1 DAW, 2..7 user: Performance for now)
  uint8_t curve;          // CURVE_*
  int8_t transpose;       // semitones, -12..12
  uint8_t page;           // pad note page (16 notes each): notes 4 + 16 * page..., 0..7
  uint8_t rate;           // note repeat: 0..7 = 1/4 1/4T 1/8 1/8T 1/16 1/16T 1/32 1/32T
  uint8_t swing;          // 0 off, 1..4
  bool latch;
  bool sync;              // follow incoming MIDI clock
  uint16_t bpm;           // internal tempo, 30..300
  bool aftertouch;        // polyphonic aftertouch from the pad pressure
  bool bluetooth;         // BLE-MIDI on at power up
};

static const uint32_t SETTINGS_MAGIC = 0x534D4301;   // "SMC" v1
static const uint8_t DEFAULT_PAGE = 2;               // notes 36..51 (MPC)

static Settings settings;
static uint32_t settingsChangedAt;                  // 0: saved

static void settingsDefaults() {
  settings = Settings{SETTINGS_MAGIC, PRESET_PERFORMANCE, CURVE_LINEAR, 0, DEFAULT_PAGE,
                      4 /* 1/16 */, 0, false, false, 120, true, false};
}

static void settingsLoad() {
  EEPROM.get(0, settings);
  if (settings.magic != SETTINGS_MAGIC) settingsDefaults();
}

static void settingsChanged() {
  settingsChangedAt = millis() | 1;
}

// written 2 s after the last change, so turning a knob does not wear the flash
static void settingsSaveWhenIdle() {
  if (settingsChangedAt && millis() - settingsChangedAt > 2000) {
    EEPROM.put(0, settings);
    EEPROM.commit();
    settingsChangedAt = 0;
  }
}
