// The SMC-PAD's stock behaviour as a sketch (docs/stock-behaviour.md), on USB-MIDI and
// BLE-MIDI at once:
//   - presets (SHIFT + pads 1-8): 1 Performance (notes, CCs), 2 DAW (Mackie Control),
//     3-8 Performance for now (editable later from the browser editor)
//   - PAD BANK / KNOB BANK: second bank; SHIFT + pads 9-12 velocity curve, 13/14 transpose,
//     15/16 pad page (16 notes) down/up, 15+16 back to the default page
//   - note repeat: hold NOTE REPEAT and play pads (with latch: NOTE REPEAT toggles);
//     SHIFT + NOTE REPEAT: its settings on the pads (rate, swing, latch, sync, tap tempo);
//     NOTE REPEAT + knobs 1-4: rate, swing, tempo, latch. The repeats follow the pad pressure.
//   - polyphonic aftertouch from the pad pressure; BT long press: Bluetooth on/off;
//     < and > flash on low battery.
#include <SMCPad.h>
#include "Settings.h"
#include "Midi.h"

// buttons and their LEDs, in panel order
enum { B_BT, B_PAD_BANK, B_KNOB_BANK, B_LEFT, B_RIGHT, B_PLAY, B_STOP, B_REC, B_SHIFT, B_REPEAT };

static const uint8_t CH = 0;                         // MIDI channel 1

// ---------------------------------------------------------------- presets

// Performance: one colour per 16-note page (the stock preset's)
static const uint32_t pageColor[8] = {0x96C8F0, 0xF0F000, 0xF000F0, 0x0000F0,
                                      0x00F000, 0xF00000, 0x00F0F0, 0x7878F0};
static const uint8_t perfKnobCC[2] = {30, 38};       // CC 30-37, bank 2: 38-45
static const uint8_t perfTransportCC[5] = {25, 26, 27, 28, 29};   // < > play stop rec

// DAW (Mackie Control): rows select, rec arm, solo, mute for tracks 1-4 (bank 2: 5-8)
static const uint8_t dawPadNote[16] = {0x18, 0x19, 0x1A, 0x1B, 0x00, 0x01, 0x02, 0x03,
                                       0x08, 0x09, 0x0A, 0x0B, 0x10, 0x11, 0x12, 0x13};
static const uint32_t dawRowColor[4] = {0x00FF88, 0xFF0000, 0x0010FF, 0xFFFF00};
static const uint8_t dawTransportNote[5] = {46, 47, 94, 93, 95};  // bank < >, play stop rec
static const uint8_t dawVpotCC = 16;                 // V-pots 1-8: CC 16-23, relative

static bool isDaw() { return settings.preset == PRESET_DAW; }

// ---------------------------------------------------------------- state

static bool padBank, knobBank;
static bool held[16];
static uint8_t heldVelocity[16];
static int8_t sentNote[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static uint8_t knobValue[2][8];
static bool mackieLed[128];                          // what the DAW lit (note on ch 1)
static bool nrPage;                                  // SHIFT + NOTE REPEAT: settings on the pads
static bool nrToggled;                               // with latch: NOTE REPEAT toggles
static bool latched[16];
static bool nrKnobUsed;
static int8_t knobAccum[4];
static uint32_t btDownAt;
static bool needShow = true;
static uint8_t lastAftertouch[16];
static uint32_t lastAftertouchAt[16];
static uint32_t flashUntil[16];                      // a repeat just fired: pad bright

// ---------------------------------------------------------------- velocity

static uint8_t applyCurve(int v) {
  v = constrain(v, 1, 127);
  switch (settings.curve) {
    case CURVE_SOFT: return constrain((int)(sqrt(v / 127.0) * 127 + 0.5), 1, 127);
    case CURVE_HARD: return constrain(v * v / 127, 1, 127);
    case CURVE_FULL: return 127;
    default: return v;
  }
}

static int pressureToLevel(int raw) {                // 0..127
  return constrain((raw - 150) * 127 / (960 - 150), 0, 127);
}

// ---------------------------------------------------------------- pads -> MIDI

static int padNote(int pad) {
  if (isDaw()) return dawPadNote[pad] + (padBank ? 4 : 0);
  int page = min(settings.page + (padBank ? 1 : 0), 7);
  int note = 4 + 16 * page + pad + settings.transpose;
  return note >= 0 && note <= 127 ? note : -1;
}

static void padNoteOn(int pad, uint8_t vel) {
  int note = sentNote[pad] >= 0 ? sentNote[pad] : padNote(pad);
  if (note < 0) return;
  if (sentNote[pad] >= 0) noteOff(CH, note);
  noteOn(CH, note, vel);
  sentNote[pad] = note;
}

static void padNoteOff(int pad) {
  if (sentNote[pad] >= 0) {
    noteOff(CH, sentNote[pad]);
    sentNote[pad] = -1;
  }
}

// ---------------------------------------------------------------- note repeat clock

static const uint8_t rateClocks[8] = {24, 16, 12, 8, 6, 4, 3, 2};   // at 24 per quarter
static uint32_t clockCount;
static uint32_t nextInternalUs;
static uint32_t lastExternalUs, externalIntervalUs = 20833;
static bool externalRunning;
static bool triggerPending;
static uint32_t triggerAt;
static uint32_t gateOffAt[16];

static bool nrActive() {
  return settings.latch ? nrToggled : SMCPad.button(B_REPEAT) && !nrPage;
}

static bool externalClockAlive() {
  return settings.sync && micros() - lastExternalUs < 500000;
}

static uint32_t clockIntervalUs() {
  return externalClockAlive() ? externalIntervalUs : 2500000UL / settings.bpm;
}

static void repeatFire() {
  uint32_t step = rateClocks[settings.rate] * clockIntervalUs();
  for (int pad = 0; pad < 16; pad++) {
    if (!(held[pad] || latched[pad])) continue;
    int vel = held[pad] ? applyCurve(max(pressureToLevel(SMCPad.padPressure(pad)), 1))
                        : heldVelocity[pad];
    padNoteOn(pad, vel);
    gateOffAt[pad] = (micros() + step / 2) | 1;
    flashUntil[pad] = millis() + 40;
    needShow = true;
  }
}

// one MIDI clock (24 per quarter note), internal or external
static void clockTick() {
  clockCount++;
  if (!nrActive()) return;
  uint8_t grid = rateClocks[settings.rate];
  if (clockCount % grid) return;
  uint32_t step = (clockCount / grid) & 1;
  // swing: every second step later, by 0 / 8 / 16 / 24 / 32 % of a step (50..66 % swing)
  uint32_t late = step ? grid * clockIntervalUs() * settings.swing * 8 / 100 : 0;
  triggerAt = micros() + late;
  triggerPending = true;
}

static void clockRun() {
  if (!externalClockAlive()) {
    uint32_t now = micros();
    if ((int32_t)(now - nextInternalUs) > 100000) nextInternalUs = now;   // restart
    while ((int32_t)(now - nextInternalUs) >= 0) {
      clockTick();
      nextInternalUs += 2500000UL / settings.bpm;
    }
  }
  if (triggerPending && (int32_t)(micros() - triggerAt) >= 0) {
    triggerPending = false;
    repeatFire();
  }
  for (int pad = 0; pad < 16; pad++) {
    if (gateOffAt[pad] && (int32_t)(micros() - gateOffAt[pad]) >= 0) {
      gateOffAt[pad] = 0;
      if (!held[pad] || nrActive()) padNoteOff(pad);
    }
  }
}

static void midiIn() {
  for (int i = 0; i < 32; i++) {
    midiEventPacket_t p = midiRead();
    if (p.header == 0) return;
    switch (p.byte1) {
      case 0xF8:
        if (settings.sync) {
          uint32_t now = micros();
          uint32_t dt = now - lastExternalUs;
          if (dt < 200000) externalIntervalUs = (externalIntervalUs * 3 + dt) / 4;
          lastExternalUs = now;
          clockTick();
        }
        break;
      case 0xFA: clockCount = 0; externalRunning = true; needShow = true; break;
      case 0xFB: externalRunning = true; break;
      case 0xFC: externalRunning = false; break;
      default:
        if ((p.byte1 & 0xF0) == 0x90 || (p.byte1 & 0xF0) == 0x80) {   // Mackie LEDs
          mackieLed[p.byte2 & 0x7F] = (p.byte1 & 0xF0) == 0x90 && p.byte3 > 0;
          needShow = true;
        }
    }
  }
}

// ---------------------------------------------------------------- tap tempo

static uint32_t taps[4];
static uint8_t tapCount;

static void tapTempo() {
  uint32_t now = millis();
  if (tapCount && now - taps[(tapCount - 1) % 4] > 2000) tapCount = 0;
  taps[tapCount % 4] = now;
  tapCount++;
  if (tapCount >= 2) {
    int n = min((int)tapCount, 4);
    uint32_t span = taps[(tapCount - 1) % 4] - taps[(tapCount - n) % 4];
    settings.bpm = constrain(60000UL * (n - 1) / span, 30, 300);
    settingsChanged();
  }
}

// ---------------------------------------------------------------- pad LEDs

static uint32_t scale(uint32_t c, int num, int den) {
  uint32_t r = ((c >> 16) & 0xFF) * num / den, g = ((c >> 8) & 0xFF) * num / den, b = (c & 0xFF) * num / den;
  return r << 16 | g << 8 | b;
}

static uint32_t shiftColor(int pad) {
  if (pad < 8) return pad == settings.preset ? 0x606060 : 0x080808;
  if (pad < 12) return pad - 8 == settings.curve ? 0x603000 : 0x0C0600;
  if (pad < 14) return (pad == 12 ? settings.transpose < 0 : settings.transpose > 0) ? 0x0030A0 : 0x000818;
  int page = settings.page;
  return (pad == 14 ? page < DEFAULT_PAGE : page > DEFAULT_PAGE) ? 0x00A020 : 0x001804;
}

static uint32_t nrPageColor(int pad) {
  if (pad < 8) return pad == settings.rate ? 0x00A0A0 : 0x001010;
  if (pad < 13) return pad - 8 == settings.swing ? 0xA000A0 : 0x100010;
  if (pad == 13) return settings.latch ? 0xA0A000 : 0x101000;
  if (pad == 14) return settings.sync ? (externalClockAlive() ? 0x00C000 : 0xC03000) : 0x001000;
  // tap tempo: flashes on the beat
  uint32_t beatMs = 60000UL / settings.bpm;
  return millis() % beatMs < beatMs / 4 ? 0x808080 : 0x080808;
}

static uint32_t padLedColor(int pad) {
  if (nrPage) return nrPageColor(pad);
  if (SMCPad.button(B_SHIFT)) return shiftColor(pad);
  bool bright = held[pad] || millis() < flashUntil[pad] || (latched[pad] && nrActive());
  if (isDaw()) {
    uint32_t c = dawRowColor[pad / 4];
    bright |= mackieLed[(dawPadNote[pad] + (padBank ? 4 : 0)) & 0x7F];
    return bright ? scale(c, 1, 2) : scale(c, 1, 16);
  }
  uint32_t c = pageColor[min(settings.page + (padBank ? 1 : 0), 7)];
  return bright ? scale(c, 1, 2) : scale(c, 1, 16);
}

static void showPads() {
  static uint32_t lastShow;
  bool animated = nrPage || settings.sync;
  if (!needShow && !(animated && millis() - lastShow > 30)) return;
  if (millis() - lastShow < 10) return;
  for (int pad = 0; pad < 16; pad++) SMCPad.padColor(pad, padLedColor(pad));
  SMCPad.show();
  lastShow = millis();
  needShow = false;
}

// ---------------------------------------------------------------- button LEDs

static bool lowBattery() {
  uint32_t mv = smcpad_battery_mv();
  return !smcpad_usb_power() && mv > 0 && mv < 3450;
}

static void showButtonLeds() {
  bool blink = millis() % 500 < 250;
  bool bleConnected = bleStarted && BLE.connected();
  SMCPad.buttonLed(B_BT, settings.bluetooth && (bleConnected || blink));
  SMCPad.buttonLed(B_PAD_BANK, padBank);
  SMCPad.buttonLed(B_KNOB_BANK, knobBank);
  bool low = lowBattery() && blink;
  SMCPad.buttonLed(B_LEFT, SMCPad.button(B_LEFT) || low);
  SMCPad.buttonLed(B_RIGHT, SMCPad.button(B_RIGHT) || low);
  SMCPad.buttonLed(B_PLAY, SMCPad.button(B_PLAY) || (isDaw() ? mackieLed[94] : externalRunning && settings.sync));
  SMCPad.buttonLed(B_STOP, SMCPad.button(B_STOP));
  SMCPad.buttonLed(B_REC, SMCPad.button(B_REC) || (isDaw() && mackieLed[95]));
  SMCPad.buttonLed(B_SHIFT, SMCPad.button(B_SHIFT));
  SMCPad.buttonLed(B_REPEAT, nrPage ? blink : nrActive());
}

// ---------------------------------------------------------------- Bluetooth

static void bluetoothApply() {
  if (settings.bluetooth) {
    if (!bleStarted) {
      bleStarted = BLEMIDI.begin("SMC-PAD");
    } else {
      BLE.advertise();
    }
  } else if (bleStarted) {
    BLE.stopAdvertise();
    BLE.disconnect();
  }
}

// ---------------------------------------------------------------- events

static void onPadPress(int pad, int velocity) {
  held[pad] = true;
  needShow = true;
  if (nrPage) {
    if (pad < 8) settings.rate = pad;
    else if (pad < 13) settings.swing = pad - 8;
    else if (pad == 13) { settings.latch = !settings.latch; nrToggled = false; }
    else if (pad == 14) settings.sync = !settings.sync;
    else tapTempo();
    settingsChanged();
    return;
  }
  if (SMCPad.button(B_SHIFT)) {
    if (pad < 8) {
      for (int p = 0; p < 16; p++) padNoteOff(p);
      settings.preset = pad;
    } else if (pad < 12) {
      settings.curve = pad - 8;
    } else if (pad == 12) {
      settings.transpose = max(settings.transpose - 1, -12);
    } else if (pad == 13) {
      settings.transpose = min(settings.transpose + 1, 12);
    } else if ((pad == 14 && held[15]) || (pad == 15 && held[14])) {
      settings.page = DEFAULT_PAGE;               // SHIFT + 15 + 16
    } else if (pad == 14) {
      if (settings.page > 0) settings.page--;
    } else {
      if (settings.page < 7) settings.page++;
    }
    settingsChanged();
    return;
  }
  uint8_t vel = applyCurve(velocity);
  heldVelocity[pad] = vel;
  if (nrActive() && settings.latch) {
    if (latched[pad]) {                            // a latched pad again: stop it
      latched[pad] = false;
      padNoteOff(pad);
      return;
    }
    latched[pad] = true;
  }
  padNoteOn(pad, vel);
  lastAftertouch[pad] = 0;
}

static void onPadRelease(int pad) {
  held[pad] = false;
  needShow = true;
  if (latched[pad] && nrActive()) return;        // keeps repeating
  if (!gateOffAt[pad] || !nrActive()) padNoteOff(pad);
}

static void transport(int index, bool down) {
  if (isDaw()) {
    if (down) noteOn(CH, dawTransportNote[index], 127);
    else noteOff(CH, dawTransportNote[index]);
  } else {
    cc(CH, perfTransportCC[index], down ? 127 : 0);
  }
}

static void onButtonPress(int b) {
  needShow = true;
  switch (b) {
    case B_BT: btDownAt = millis() | 1; break;
    case B_PAD_BANK:
      for (int p = 0; p < 16; p++) if (!latched[p]) padNoteOff(p);
      padBank = !padBank;
      break;
    case B_KNOB_BANK: knobBank = !knobBank; break;
    case B_LEFT: case B_RIGHT: case B_PLAY: case B_STOP: case B_REC:
      transport(b - B_LEFT, true);
      break;
    case B_REPEAT:
      nrKnobUsed = false;
      if (SMCPad.button(B_SHIFT)) { nrPage = !nrPage; break; }
      if (nrPage) { nrPage = false; break; }
      if (settings.latch) {
        nrToggled = !nrToggled;
        if (!nrToggled) {
          for (int p = 0; p < 16; p++) if (latched[p]) { latched[p] = false; padNoteOff(p); }
        }
      }
      clockCount = 0;
      break;
  }
}

static void onButtonRelease(int b) {
  needShow = true;
  switch (b) {
    case B_BT: btDownAt = 0; break;
    case B_LEFT: case B_RIGHT: case B_PLAY: case B_STOP: case B_REC:
      transport(b - B_LEFT, false);
      break;
    case B_REPEAT:
      if (!settings.latch) {
        for (int p = 0; p < 16; p++) if (!held[p]) padNoteOff(p);
      }
      break;
  }
}

static void onKnob(int knob, int delta) {
  if (SMCPad.button(B_REPEAT) && knob < 4) {     // note repeat settings
    nrKnobUsed = true;
    knobAccum[knob] += delta;
    int steps = knobAccum[knob] / 2;               // two detents per step
    if (knob == 2) steps = knobAccum[knob];        // tempo: one BPM per detent
    if (!steps) return;
    knobAccum[knob] = 0;
    switch (knob) {
      case 0: settings.rate = constrain(settings.rate + steps, 0, 7); break;
      case 1: settings.swing = constrain(settings.swing + steps, 0, 4); break;
      case 2: settings.bpm = constrain(settings.bpm + steps, 30, 300); break;
      case 3: settings.latch = steps > 0; nrToggled = false; break;
    }
    settingsChanged();
    needShow = true;
    return;
  }
  int bank = knobBank ? 1 : 0;
  if (isDaw() && bank == 0) {                      // Mackie V-pot: relative, sign-magnitude
    int d = constrain(abs(delta), 1, 15);
    cc(CH, dawVpotCC + knob, delta > 0 ? d : 0x40 | d);
    return;
  }
  uint8_t &v = knobValue[bank][knob];
  int nv = constrain(v + delta, 0, 127);
  if (nv == v) return;
  v = nv;
  cc(CH, perfKnobCC[bank] + knob, v);
}

// ---------------------------------------------------------------- aftertouch

static void aftertouch() {
  if (!settings.aftertouch || isDaw() || nrActive()) return;
  uint32_t now = millis();
  for (int pad = 0; pad < 16; pad++) {
    if (!held[pad] || sentNote[pad] < 0 || now - lastAftertouchAt[pad] < 20) continue;
    uint8_t level = pressureToLevel(SMCPad.padPressure(pad));
    if (abs(level - lastAftertouch[pad]) < 2) continue;
    midiSend(0xA0 | CH, sentNote[pad], level);
    lastAftertouch[pad] = level;
    lastAftertouchAt[pad] = now;
  }
}

// ---------------------------------------------------------------- main

void setup() {
  settingsLoad();
  SMCPad.onPadPress(onPadPress);
  SMCPad.onPadRelease(onPadRelease);
  SMCPad.onButtonPress(onButtonPress);
  SMCPad.onButtonRelease(onButtonRelease);
  SMCPad.onKnob(onKnob);
  bluetoothApply();
  nextInternalUs = micros();
}

void loop() {
  SMCPad.update();
  midiIn();
  clockRun();
  aftertouch();
  if (btDownAt && millis() - btDownAt > 800) {   // BT long press
    btDownAt = 0;
    settings.bluetooth = !settings.bluetooth;
    settingsChanged();
    bluetoothApply();
  }
  showPads();
  showButtonLeds();
  settingsSaveWhenIdle();
}
