// GrooveBox: a 16-track MIDI step sequencer for the SMC-PAD (docs/sequencer-design.md).
//
// Modes (the colours tell where you are):
//   PLAY    (default)          pads play the 16 tracks (and select them); live recording;
//                              note repeat; aftertouch. SHIFT + pads 9-16: velocity curve,
//                              transpose, octave (15 + 16: reset)
//   STEP    PAD BANK           pads = the 16 steps of the selected track's page; < > pages;
//                              hold steps + knobs: velocity, micro-timing, condition, ratchet,
//                              p-locks 1-4
//   MUTE    SHIFT + PAD BANK   pads = tracks, tap to mute
//   PATTERN SHIFT + KNOB BANK  pads = patterns: tap = next, hold one + tap others = chain,
//                              SHIFT + pad = copy the current pattern there
// Everywhere: PLAY start / stop, STOP stop, REC record, SHIFT + REC (1 s) clear the track,
// SHIFT + STOP (1 s) the kit back to notes 36-51 on channel 10,
// KNOB BANK knob page (A: velocity, gate, length, speed, -, -, euclid hits, euclid rotation;
// B: CCs 70-77), SHIFT + knobs 1-5: tempo, swing, pattern length, track note, track channel,
// SHIFT + NOTE REPEAT: note repeat settings (as stock: rate, swing, latch, sync, tap tempo),
// NOTE REPEAT outside PLAY = fill, BT long press = Bluetooth.
// With Live's SMC_PAD control surface (ableton/SMC_PAD) the pads take the Drum Rack's colours
// and empty drum pads stay dark.
#include <SMCPad.h>
#include "Settings.h"
#include "Midi.h"
#include "Model.h"
#include "Store.h"
#include "Engine.h"

enum { B_BT, B_PAD_BANK, B_KNOB_BANK, B_LEFT, B_RIGHT, B_PLAY, B_STOP, B_REC, B_SHIFT, B_REPEAT };
enum Mode { MODE_PLAY, MODE_STEP, MODE_MUTE, MODE_PATTERN };

static Mode mode = MODE_PLAY;
static int selectedTrack;
static int stepPage;
static bool knobPageB;
static bool recording;
static bool nrPage;                                  // note repeat settings on the pads
static bool held[16];
static int8_t sentNote[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static uint8_t sentChannel[16];
static uint8_t heldVelocity[16];
static uint32_t flashUntil[16];
static bool needShow = true;
static uint32_t btDownAt, recDownAt, stopDownAt;

// STEP mode: steps held for editing
static bool stepHeld[16];
static bool stepWasOn[16], stepEdited[16];

// PATTERN mode: a held pad starts a chain
static int chainAnchor = -1;
static int8_t chainBuild[PATTERNS];
static int chainBuildLength;

// the value a knob just changed, shown on the pads for a moment
enum { OV_NONE, OV_BAR, OV_CHOICE };
static uint8_t overlayKind;
static int overlayValue, overlayMax;
static uint32_t overlayColor, overlayUntil;

static void showValue(int value, int max, uint32_t color) {
  overlayKind = max < 16 ? OV_CHOICE : OV_BAR;
  overlayValue = value;
  overlayMax = max;
  overlayColor = color;
  overlayUntil = millis() + 900;
  needShow = true;
}

static void onTrackHit(int t) {
  flashUntil[t] = millis() + 60;
  needShow = true;
}

// ---------------------------------------------------------------- Ableton (ableton/SMC_PAD)
// The control surface script sends the Drum Rack's picture: per note, whether a drum pad holds
// something and its colour. While it is there, tracks take those colours and empty ones go dark.
static bool abletonActive;
static uint32_t abletonColor[128];               // 0: empty drum pad

static uint32_t colorOf(int t) {
  if (!abletonActive) return trackColor[t];
  return abletonColor[pattern.track[t].note & 0x7F];
}

static uint8_t sysex[600];
static int sysexLength = -1;                      // -1: not in a SysEx

static void sysexDone() {
  // F0 7D 53 50 cmd ...
  if (sysexLength < 6 || sysex[1] != 0x7D || sysex[2] != 0x53 || sysex[3] != 0x50) return;
  if (sysex[4] == 0x01 && sysexLength >= 6 + 128 * 4) {
    for (int n = 0; n < 128; n++) {
      const uint8_t *d = sysex + 5 + n * 4;
      uint32_t c = (uint32_t)d[1] << 17 | (uint32_t)d[2] << 9 | (uint32_t)d[3] << 1;
      abletonColor[n] = d[0] ? (c ? c : 0x404040) : 0;   // present but black: grey
    }
    abletonActive = true;
  } else if (sysex[4] == 0x02) {
    abletonActive = false;
  }
  needShow = true;
}

// USB-MIDI SysEx packets: CIN 4 = 3 bytes going on, 5 / 6 / 7 = the last 1 / 2 / 3
static void sysexPacket(const midiEventPacket_t &p) {
  uint8_t cin = p.header & 0x0F, b[3] = {p.byte1, p.byte2, p.byte3};
  int n = cin == 0x4 || cin == 0x7 ? 3 : cin == 0x6 ? 2 : 1;
  for (int i = 0; i < n; i++) {
    if (b[i] == 0xF0) sysexLength = 0;
    if (sysexLength < 0) continue;
    if (sysexLength < (int)sizeof(sysex)) sysex[sysexLength++] = b[i];
    if (b[i] == 0xF7) {
      sysexDone();
      sysexLength = -1;
    }
  }
}

// STEP mode: steps in reading order, step 1 top-left (pads count from bottom-left)
static int padToStep(int pad) {
  return stepPage * 16 + (3 - pad / 4) * 4 + pad % 4;
}

// ---------------------------------------------------------------- velocity, pressure

static uint8_t applyCurve(int v) {
  v = constrain(v, 1, 127);
  switch (settings.curve) {
    case CURVE_SOFT: return constrain((int)(sqrt(v / 127.0) * 127 + 0.5), 1, 127);
    case CURVE_HARD: return constrain(v * v / 127, 1, 127);
    case CURVE_FULL: return 127;
    default: return v;
  }
}

static int pressureLevel(int pad) {                  // 0..127
  return constrain((SMCPad.padPressure(pad) - 150) * 127 / (960 - 150), 0, 127);
}

// ---------------------------------------------------------------- PLAY: pads -> notes

static void padNoteOn(int pad, uint8_t vel) {
  Track &k = pattern.track[pad];
  if (sentNote[pad] >= 0) noteOff(sentChannel[pad], sentNote[pad]);
  int note = k.note + settings.transpose + 12 * settings.octave;
  if (note < 0 || note > 127) return;
  noteOn(k.channel, note, vel);
  sentNote[pad] = note;
  sentChannel[pad] = k.channel;
  onTrackHit(pad);
}

static void padNoteOff(int pad) {
  if (sentNote[pad] >= 0) {
    noteOff(sentChannel[pad], sentNote[pad]);
    sentNote[pad] = -1;
  }
}

// ---------------------------------------------------------------- note repeat (PLAY)

static const uint8_t rateClocks[8] = {24, 16, 12, 8, 6, 4, 3, 2};
static uint32_t nrClocks;
static bool nrToggled;
static bool latched[16];
static bool nrTriggerPending;
static uint32_t nrTriggerAt, nrGateOff[16];
static uint32_t nrNextUs;                            // own clock while the sequencer is stopped

static bool nrActive() {
  if (mode != MODE_PLAY || nrPage) return false;
  return settings.latch ? nrToggled : SMCPad.button(B_REPEAT);
}

static uint32_t clockUs() {                          // one 24 PPQN clock
  return extAlive() ? extIntervalUs : 2500000UL / bpm;
}

static void noteRepeatClock() {
  nrClocks++;
  if (!nrActive()) return;
  uint8_t grid = rateClocks[settings.rate];
  if (nrClocks % grid) return;
  uint32_t odd = (nrClocks / grid) & 1;
  nrTriggerAt = micros() + (odd ? grid * clockUs() * settings.swing * 8 / 100 : 0);
  nrTriggerPending = true;
}

static void noteRepeatRun() {
  if (!playing) {                                    // the sequencer is not clocking: own clock
    uint32_t now = micros();
    if (!extAlive()) {
      if ((int32_t)(now - nrNextUs) > 100000) nrNextUs = now;
      while ((int32_t)(now - nrNextUs) >= 0) {
        noteRepeatClock();
        nrNextUs += clockUs();
      }
    }
  }
  if (nrTriggerPending && (int32_t)(micros() - nrTriggerAt) >= 0) {
    nrTriggerPending = false;
    uint32_t step = rateClocks[settings.rate] * clockUs();
    for (int pad = 0; pad < 16; pad++) {
      if (!(held[pad] || latched[pad])) continue;
      uint8_t vel = held[pad] ? applyCurve(max(pressureLevel(pad), 1)) : heldVelocity[pad];
      padNoteOn(pad, vel);
      if (recording && playing) recordHit(pad, vel);
      nrGateOff[pad] = (micros() + step / 2) | 1;
    }
  }
  for (int pad = 0; pad < 16; pad++) {
    if (nrGateOff[pad] && (int32_t)(micros() - nrGateOff[pad]) >= 0) {
      nrGateOff[pad] = 0;
      if (!held[pad] || nrActive()) padNoteOff(pad);
    }
  }
}

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
    bpm = settings.bpm = constrain(60000UL * (n - 1) / span, 30, 300);
    settingsChanged();
  }
}

// ---------------------------------------------------------------- aftertouch (PLAY)

static uint8_t lastAt[16];
static uint32_t lastAtMs[16];

static void aftertouch() {
  if (!settings.aftertouch || mode != MODE_PLAY || nrActive()) return;
  uint32_t now = millis();
  for (int pad = 0; pad < 16; pad++) {
    if (!held[pad] || sentNote[pad] < 0 || now - lastAtMs[pad] < 20) continue;
    uint8_t level = pressureLevel(pad);
    if (abs(level - lastAt[pad]) < 2) continue;
    midiSend(0xA0 | sentChannel[pad], sentNote[pad], level);
    lastAt[pad] = level;
    lastAtMs[pad] = now;
  }
}

// ---------------------------------------------------------------- euclid

static void euclid(int t, int hits, int rotate) {
  Track &k = pattern.track[t];
  int n = k.length;
  hits = constrain(hits, 0, n);
  for (int s = 0; s < n; s++) {
    int i = (s + rotate) % n;
    bool on = hits && ((i * hits) % n) < hits;
    Step &st = pattern.step[t][s];
    if (on) st.flags |= STEP_ON; else st.flags &= ~STEP_ON;
  }
  patternDirty = true;
}
static int8_t euclidHits[TRACKS], euclidRotate[TRACKS];

// ---------------------------------------------------------------- pad LEDs

static uint32_t scale(uint32_t c, int num, int den) {
  return (((c >> 16) & 0xFF) * num / den) << 16 | (((c >> 8) & 0xFF) * num / den) << 8 | ((c & 0xFF) * num / den);
}

static uint32_t mix(uint32_t a, uint32_t b) {        // halfway between two colours
  return ((((a >> 16) & 0xFF) + ((b >> 16) & 0xFF)) / 2) << 16 |
         ((((a >> 8) & 0xFF) + ((b >> 8) & 0xFF)) / 2) << 8 | (((a & 0xFF) + (b & 0xFF)) / 2);
}

static uint32_t overlayLed(int pad) {
  if (overlayKind == OV_CHOICE) return pad == overlayValue ? overlayColor : (pad <= overlayMax ? scale(overlayColor, 1, 12) : 0);
  int lit = (overlayValue * 16 + overlayMax / 2) / max(overlayMax, 1);
  return pad < lit ? overlayColor : 0x020202;
}

static uint32_t nrPageLed(int pad) {
  if (pad < 8) return pad == settings.rate ? 0x00A0A0 : 0x001010;
  if (pad < 13) return pad - 8 == settings.swing ? 0xA000A0 : 0x100010;
  if (pad == 13) return settings.latch ? 0xA0A000 : 0x101000;
  if (pad == 14) return settings.sync ? (extAlive() ? 0x00C000 : 0xC03000) : 0x001000;
  uint32_t beatMs = 60000UL / bpm;
  return millis() % beatMs < beatMs / 4 ? 0x808080 : 0x080808;
}

static uint32_t shiftPlayLed(int pad) {
  if (pad < 8) return pad == selectedTrack ? scale(trackColor[pad], 1, 3) : 0;
  if (pad < 12) return pad - 8 == settings.curve ? 0x603000 : 0x0C0600;
  if (pad < 14) return (pad == 12 ? settings.transpose < 0 : settings.transpose > 0) ? 0x0030A0 : 0x000818;
  return (pad == 14 ? settings.octave < 0 : settings.octave > 0) ? 0x00A020 : 0x001804;
}

static uint32_t padLed(int pad) {
  uint32_t now = millis();
  if (now < overlayUntil) return overlayLed(pad);
  if (nrPage) return nrPageLed(pad);
  bool shift = SMCPad.button(B_SHIFT);
  switch (mode) {
    case MODE_PLAY: {
      if (shift) return shiftPlayLed(pad);
      Track &k = pattern.track[pad];
      bool bright = held[pad] || now < flashUntil[pad] || (latched[pad] && nrActive());
      uint32_t c = colorOf(pad);
      if (bright) return c;
      return pad == selectedTrack ? scale(c, 1, 2) : scale(c, 1, k.muted ? 20 : 5);
    }
    case MODE_STEP: {
      if (shift) return pad == selectedTrack ? scale(colorOf(pad), 1, 1) : scale(colorOf(pad), 1, 8);
      Track &k = pattern.track[selectedTrack];
      int s = padToStep(pad);
      if (s >= k.length) return 0;
      if (playing && playheadStep(selectedTrack) == s) return 0xC0C0C0;
      Step &st = pattern.step[selectedTrack][s];
      uint32_t c = colorOf(selectedTrack);
      if (!c) c = 0x606060;                         // an empty drum pad: still see the steps
      if (stepHeld[pad]) return c;
      if (!(st.flags & STEP_ON)) return s % 4 == 0 ? 0x0A0A0A : 0x020202;
      uint32_t on = scale(c, 2, 3);
      if (stepHasLocks(st)) on = mix(on, 0x303030);
      if (st.cond != COND_ALWAYS && (now / 300) % 2) on = scale(on, 1, 3);
      return on;
    }
    case MODE_MUTE: {
      if (shift) return pad == selectedTrack ? scale(colorOf(pad), 1, 1) : scale(colorOf(pad), 1, 8);
      uint32_t c = colorOf(pad);
      if (now < flashUntil[pad] && !pattern.track[pad].muted) return c;
      return pattern.track[pad].muted ? scale(c, 1, 30) : scale(c, 1, 3);
    }
    case MODE_PATTERN: {
      if (pad == currentSlot) return 0x00A040;
      if (pad == queuedSlot) return (now / 200) % 2 ? 0x00A040 : 0x001008;
      for (int i = 0; i < chainLength; i++) if (chain[i] == pad && chainLength > 1) return 0x003010;
      if (held[pad]) return 0x404040;
      return storeHas(pad) ? 0x002060 : 0x040404;
    }
  }
  return 0;
}

static void showPads() {
  static uint32_t lastShow;
  bool animated = playing || nrPage || millis() < overlayUntil + 50 || mode == MODE_PATTERN;
  if (!needShow && !(animated && millis() - lastShow > 25)) return;
  if (millis() - lastShow < 8) return;
  for (int pad = 0; pad < 16; pad++) SMCPad.padColor(pad, padLed(pad));
  SMCPad.show();
  lastShow = millis();
  needShow = false;
}

// ---------------------------------------------------------------- button LEDs

static void showButtonLeds() {
  uint32_t now = millis();
  bool blink = now % 500 < 250;
  bool bleConnected = bleStarted && BLE.connected();
  SMCPad.buttonLed(B_BT, settings.bluetooth && (bleConnected || blink));
  SMCPad.buttonLed(B_PAD_BANK, mode == MODE_STEP || (mode == MODE_MUTE && blink));
  SMCPad.buttonLed(B_KNOB_BANK, knobPageB || (mode == MODE_PATTERN && blink));
  uint32_t mv = smcpad_battery_mv();
  bool low = !smcpad_usb_power() && mv > 0 && mv < 3450 && blink;
  int pages = (pattern.track[selectedTrack].length + 15) / 16;
  SMCPad.buttonLed(B_LEFT, low || SMCPad.button(B_LEFT) || (mode == MODE_STEP && stepPage > 0));
  SMCPad.buttonLed(B_RIGHT, low || SMCPad.button(B_RIGHT) || (mode == MODE_STEP && stepPage < pages - 1));
  uint32_t beat = (tick - patternStartTick) % PPQN;
  SMCPad.buttonLed(B_PLAY, playing && beat < PPQN / 4 ? true : playing && beat >= PPQN / 4 ? (beat / 12) % 8 != 0 : false);
  SMCPad.buttonLed(B_STOP, SMCPad.button(B_STOP));
  SMCPad.buttonLed(B_REC, recording && (playing || blink));
  SMCPad.buttonLed(B_SHIFT, SMCPad.button(B_SHIFT));
  SMCPad.buttonLed(B_REPEAT, nrPage ? blink : mode == MODE_PLAY ? nrActive() : fillHeld);
}

// ---------------------------------------------------------------- Bluetooth

static void bluetoothApply() {
  if (settings.bluetooth) {
    if (!bleStarted) bleStarted = BLEMIDI.begin("SMC-PAD");
    else BLE.advertise();
  } else if (bleStarted) {
    BLE.stopAdvertise();
    BLE.disconnect();
  }
}

// ---------------------------------------------------------------- pads

static void onPadPress(int pad, int velocity) {
  held[pad] = true;
  needShow = true;
  uint8_t vel = applyCurve(velocity);
  heldVelocity[pad] = vel;
  if (nrPage) {
    if (pad < 8) settings.rate = pad;
    else if (pad < 13) settings.swing = pad - 8;
    else if (pad == 13) { settings.latch = !settings.latch; nrToggled = false; }
    else if (pad == 14) syncExternal = settings.sync = !settings.sync;
    else tapTempo();
    settingsChanged();
    return;
  }
  bool shift = SMCPad.button(B_SHIFT);
  switch (mode) {
    case MODE_PLAY:
      if (shift) {
        if (pad < 8) selectedTrack = pad;
        else if (pad < 12) settings.curve = pad - 8;
        else if (pad == 12) settings.transpose = max(settings.transpose - 1, -12);
        else if (pad == 13) settings.transpose = min(settings.transpose + 1, 12);
        else if ((pad == 14 && held[15]) || (pad == 15 && held[14])) { settings.octave = 0; settings.transpose = 0; }
        else if (pad == 14) settings.octave = max(settings.octave - 1, -3);
        else settings.octave = min(settings.octave + 1, 3);
        settingsChanged();
        return;
      }
      selectedTrack = pad;
      if (nrActive() && settings.latch) {
        if (latched[pad]) { latched[pad] = false; padNoteOff(pad); return; }
        latched[pad] = true;
      }
      padNoteOn(pad, vel);
      if (recording && playing) recordHit(pad, vel);
      lastAt[pad] = 0;
      return;
    case MODE_STEP: {
      if (shift) { selectedTrack = pad; return; }
      int s = padToStep(pad);
      if (s >= pattern.track[selectedTrack].length) return;
      Step &st = pattern.step[selectedTrack][s];
      stepHeld[pad] = true;
      stepEdited[pad] = false;
      stepWasOn[pad] = st.flags & STEP_ON;
      if (!stepWasOn[pad]) {
        st.flags |= STEP_ON;
        st.velocity = vel;
        patternDirty = true;
      }
      return;
    }
    case MODE_MUTE:
      if (shift) { selectedTrack = pad; return; }
      pattern.track[pad].muted = !pattern.track[pad].muted;
      patternDirty = true;
      return;
    case MODE_PATTERN:
      if (shift) {                                     // copy the current pattern here
        storeSave(pad, pattern);
        showValue(pad, 15, 0xA0A0A0);
        return;
      }
      if (chainAnchor < 0) {
        chainAnchor = pad;
        chainBuild[0] = pad;
        chainBuildLength = 1;
      } else if (chainBuildLength < PATTERNS) {
        chainBuild[chainBuildLength++] = pad;
      }
      return;
  }
}

static void onPadRelease(int pad) {
  held[pad] = false;
  needShow = true;
  switch (mode) {
    case MODE_PLAY:
      if (latched[pad] && nrActive()) return;
      if (!nrGateOff[pad] || !nrActive()) padNoteOff(pad);
      return;
    case MODE_STEP: {
      if (!stepHeld[pad]) return;
      stepHeld[pad] = false;
      int s = padToStep(pad);
      if (stepWasOn[pad] && !stepEdited[pad]) {        // a tap on a lit step: off
        pattern.step[selectedTrack][s].flags &= ~STEP_ON;
        patternDirty = true;
      }
      return;
    }
    case MODE_PATTERN:
      if (pad == chainAnchor) {                        // the first pad of the gesture let go
        if (chainBuildLength > 1) setChain(chainBuild, chainBuildLength);
        else { chainLength = 0; queuePattern(pad); }
        chainAnchor = -1;
        settings.slot = pad;
        settingsChanged();
      }
      return;
    default:
      return;
  }
}

// ---------------------------------------------------------------- buttons

static void allPadNotesOff() {
  for (int p = 0; p < 16; p++) { latched[p] = false; padNoteOff(p); }
}

static void setMode(Mode m) {
  if (m != MODE_PLAY) allPadNotesOff();
  mode = m;
  nrPage = false;
  for (int i = 0; i < 16; i++) stepHeld[i] = false;
  if (stepPage * 16 >= pattern.track[selectedTrack].length) stepPage = 0;
  needShow = true;
}

static void onButtonPress(int b) {
  bool shift = SMCPad.button(B_SHIFT);
  needShow = true;
  switch (b) {
    case B_BT: btDownAt = millis() | 1; break;
    case B_PAD_BANK:
      if (shift) setMode(mode == MODE_MUTE ? MODE_PLAY : MODE_MUTE);
      else setMode(mode == MODE_STEP ? MODE_PLAY : MODE_STEP);
      break;
    case B_KNOB_BANK:
      if (shift) setMode(mode == MODE_PATTERN ? MODE_PLAY : MODE_PATTERN);
      else knobPageB = !knobPageB;
      break;
    case B_LEFT:
    case B_RIGHT: {
      int dir = b == B_LEFT ? -1 : 1;
      if (mode == MODE_STEP) {
        int pages = (pattern.track[selectedTrack].length + 15) / 16;
        stepPage = constrain(stepPage + dir, 0, pages - 1);
        showValue(stepPage, pages - 1, trackColor[selectedTrack]);
      } else {
        selectedTrack = (selectedTrack + dir + TRACKS) % TRACKS;
        showValue(selectedTrack, 15, trackColor[selectedTrack]);
      }
      break;
    }
    case B_PLAY:
      if (extAlive()) break;                           // the external clock is in charge
      if (playing) transportStop(); else transportStart();
      break;
    case B_STOP:
      if (shift) { stopDownAt = millis() | 1; break; }
      if (!playing) allPadNotesOff();
      transportStop();
      break;
    case B_REC:
      if (shift) { recDownAt = millis() | 1; break; }
      recording = !recording;
      break;
    case B_REPEAT:
      if (shift) { nrPage = !nrPage; break; }
      if (nrPage) { nrPage = false; break; }
      if (mode != MODE_PLAY) { fillHeld = true; break; }
      if (settings.latch) {
        nrToggled = !nrToggled;
        if (!nrToggled) for (int p = 0; p < 16; p++) if (latched[p]) { latched[p] = false; padNoteOff(p); }
      }
      nrClocks = 0;
      break;
  }
}

static void onButtonRelease(int b) {
  needShow = true;
  switch (b) {
    case B_BT: btDownAt = 0; break;
    case B_REC: recDownAt = 0; break;
    case B_STOP: stopDownAt = 0; break;
    case B_REPEAT:
      fillHeld = false;
      if (mode == MODE_PLAY && !settings.latch) for (int p = 0; p < 16; p++) if (!held[p]) padNoteOff(p);
      break;
  }
}

// ---------------------------------------------------------------- knobs

static int knobAccum[8];

// detents per change: fine values every detent, choices every two
static int knobSteps(int knob, int delta, int perStep) {
  knobAccum[knob] += delta;
  int steps = knobAccum[knob] / perStep;
  knobAccum[knob] -= steps * perStep;
  return steps;
}

static void editHeldSteps(int knob, int delta) {
  Track &k = pattern.track[selectedTrack];
  int steps = knobSteps(knob, delta, knob == 0 || knob >= 4 ? 1 : 2);
  if (!steps) return;
  for (int pad = 0; pad < 16; pad++) {
    if (!stepHeld[pad]) continue;
    stepEdited[pad] = true;
    Step &st = pattern.step[selectedTrack][padToStep(pad)];
    switch (knob) {
      case 0: st.velocity = constrain(st.velocity + steps * 2, 1, 127); showValue(st.velocity, 127, 0xA06000); break;
      case 1: st.micro = constrain(st.micro + steps, -11, 11); showValue(st.micro + 11, 22, 0x00A0A0); break;
      case 2: {
        // 100% .. 1% probability, then 1:2 .. not fill
        int order = st.cond == COND_ALWAYS ? 0 : st.cond <= COND_PROB_MAX ? 100 - st.cond : st.cond;
        order = constrain(order + steps * (order < 100 ? 5 : 1), 0, (int)COND_LAST);
        if (order > 95 && order < 100) order = steps > 0 ? COND_1_2 : 95;
        st.cond = order == 0 ? COND_ALWAYS : order < 100 ? 100 - order : order;
        if (st.cond <= COND_PROB_MAX && st.cond) showValue(st.cond, 100, 0xA000A0);
        else showValue(st.cond == COND_ALWAYS ? 0 : st.cond - COND_1_2 + 1, 11, 0xA000A0);
        break;
      }
      case 3: st.ratchet = constrain(st.ratchet + steps, 1, 4); showValue(st.ratchet - 1, 3, 0xA0A000); break;
      default: {
        int i = knob - 4;
        int v = st.lock[i] == NO_LOCK ? (steps > 0 ? k.lockBase[i] : -1) : st.lock[i] + steps;
        st.lock[i] = v < 0 ? NO_LOCK : (uint8_t)min(v, 127);
        showValue(st.lock[i] == NO_LOCK ? 0 : st.lock[i], 127, st.lock[i] == NO_LOCK ? 0x200000 : 0x6060A0);
        break;
      }
    }
  }
  patternDirty = true;
}

static void editTrack(int knob, int delta) {
  Track &k = pattern.track[selectedTrack];
  uint32_t c = colorOf(selectedTrack);
  if (!c) c = 0x606060;
  int steps = knobSteps(knob, delta, knob <= 1 ? 1 : 2);
  if (!steps) return;
  switch (knob) {
    case 0: k.velocity = constrain(k.velocity + steps * 2, 1, 127); showValue(k.velocity, 127, c); break;
    case 1: k.gate = constrain(k.gate + steps * 5, 5, 100); showValue(k.gate, 100, c); break;
    case 2: k.length = constrain(k.length + steps, 1, MAX_STEPS); showValue(k.length, MAX_STEPS, c); break;
    case 3: k.speed = constrain(k.speed + steps, SPEED_HALF, SPEED_DOUBLE); showValue(k.speed, 2, c); break;
    case 4: case 5: return;                       // free for now
    case 6:
      euclidHits[selectedTrack] = constrain(euclidHits[selectedTrack] + steps, 0, k.length);
      euclid(selectedTrack, euclidHits[selectedTrack], euclidRotate[selectedTrack]);
      showValue(euclidHits[selectedTrack], max((int)k.length, 1), c);
      break;
    case 7:
      euclidRotate[selectedTrack] = (euclidRotate[selectedTrack] + steps + k.length) % k.length;
      euclid(selectedTrack, euclidHits[selectedTrack], euclidRotate[selectedTrack]);
      showValue(euclidRotate[selectedTrack], max((int)k.length - 1, 1), c);
      break;
  }
  patternDirty = true;
}

static uint8_t pageBValue[8] = {64, 64, 64, 64, 64, 64, 64, 64};

static void knobPageBTurn(int knob, int delta) {
  Track &k = pattern.track[selectedTrack];
  uint8_t &v = knob < LOCKS ? k.lockBase[knob] : pageBValue[knob];
  int nv = constrain(v + delta, 0, 127);
  if (nv == v) return;
  v = nv;
  uint8_t ccNum = knob < LOCKS ? k.lockCC[knob] : 70 + knob;
  cc(k.channel, ccNum, v);
  showValue(v, 127, 0x6060A0);
  if (knob < LOCKS) {
    patternDirty = true;
    if (recording && playing) {                        // a p-lock on the step playing now
      Step &st = pattern.step[selectedTrack][playheadStep(selectedTrack)];
      if (st.flags & STEP_ON) st.lock[knob] = v;
    }
  }
}

static void onKnob(int knob, int delta) {
  needShow = true;
  if (SMCPad.button(B_SHIFT) && knob < 5) {           // setup: global, and the track's voice
    int steps = knobSteps(knob, delta, knob == 0 || knob == 3 ? 1 : 2);
    if (!steps) return;
    Track &k = pattern.track[selectedTrack];
    if (knob == 3) { k.note = constrain(k.note + steps, 0, 127); showValue(k.note % 12, 11, colorOf(selectedTrack) ? colorOf(selectedTrack) : 0x606060); patternDirty = true; }
    if (knob == 4) { k.channel = constrain(k.channel + steps, 0, 15); showValue(k.channel, 15, 0x00A0A0); patternDirty = true; }
    if (knob == 0) { bpm = settings.bpm = constrain(settings.bpm + steps, 30, 300); showValue(bpm - 30, 270, 0xA0A0A0); settingsChanged(); }
    if (knob == 1) { pattern.swing = constrain(pattern.swing + steps, 0, 4); showValue(pattern.swing, 4, 0xA000A0); patternDirty = true; }
    if (knob == 2) { pattern.length = constrain(pattern.length + steps * 16, 16, 64); showValue(pattern.length / 16 - 1, 3, 0x00A040); patternDirty = true; }
    return;
  }
  if (mode == MODE_PLAY && SMCPad.button(B_REPEAT) && knob < 4) {   // note repeat, as stock
    int steps = knobSteps(knob, delta, knob == 2 ? 1 : 2);
    if (!steps) return;
    switch (knob) {
      case 0: settings.rate = constrain(settings.rate + steps, 0, 7); showValue(settings.rate, 7, 0x00A0A0); break;
      case 1: settings.swing = constrain(settings.swing + steps, 0, 4); showValue(settings.swing, 4, 0xA000A0); break;
      case 2: bpm = settings.bpm = constrain(settings.bpm + steps, 30, 300); showValue(bpm - 30, 270, 0xA0A0A0); break;
      case 3: settings.latch = steps > 0; nrToggled = false; showValue(settings.latch, 1, 0xA0A000); break;
    }
    settingsChanged();
    return;
  }
  bool anyStepHeld = false;
  for (int i = 0; i < 16; i++) anyStepHeld |= stepHeld[i];
  if (mode == MODE_STEP && anyStepHeld) editHeldSteps(knob, delta);
  else if (knobPageB) knobPageBTurn(knob, delta);
  else editTrack(knob, delta);
}

// ---------------------------------------------------------------- MIDI in

static void midiIn() {
  for (int i = 0; i < 32; i++) {
    midiEventPacket_t p = midiRead();
    if (p.header == 0) return;
    uint8_t cin = p.header & 0x0F;
    if (cin >= 0x4 && cin <= 0x7 && (cin != 0x5 || sysexLength >= 0 || p.byte1 == 0xF7)) {
      sysexPacket(p);
      continue;
    }
    switch (p.byte1) {
      case 0xF8: engineClockIn(); if (!playing && syncExternal) noteRepeatClock(); break;
      case 0xFA: engineStartIn(true); break;
      case 0xFB: engineStartIn(false); break;
      case 0xFC: engineStopIn(); break;
    }
  }
}

// ---------------------------------------------------------------- main

void setup() {
  settingsLoad();
  bpm = settings.bpm;
  syncExternal = settings.sync;
  storeBegin();
  currentSlot = settings.slot % PATTERNS;
  storeLoad(currentSlot, pattern);
  SMCPad.onPadPress(onPadPress);
  SMCPad.onPadRelease(onPadRelease);
  SMCPad.onButtonPress(onButtonPress);
  SMCPad.onButtonRelease(onButtonRelease);
  SMCPad.onKnob(onKnob);
  bluetoothApply();
}

void loop() {
  static uint32_t dirtySince;
  SMCPad.update();
  midiIn();
  engineRun();
  noteRepeatRun();
  aftertouch();
  uint32_t now = millis();
  if (btDownAt && now - btDownAt > 800) {
    btDownAt = 0;
    settings.bluetooth = !settings.bluetooth;
    settingsChanged();
    bluetoothApply();
  }
  if (recDownAt && now - recDownAt > 1000) {           // SHIFT + REC held: clear the track
    recDownAt = 0;
    trackClearSteps(pattern, selectedTrack);
    patternDirty = true;
    showValue(0, 127, trackColor[selectedTrack]);
  }
  if (stopDownAt && now - stopDownAt > 1000) {         // SHIFT + STOP held: the kit back to 36-51
    stopDownAt = 0;
    for (int t = 0; t < TRACKS; t++) {
      pattern.track[t].note = 36 + t;
      pattern.track[t].channel = 9;
    }
    patternDirty = true;
    showValue(15, 15, 0xA0A0A0);
  }
  // stopped and idle: save the edits (not while playing: a flash erase stalls the clock)
  if (patternDirty) {
    if (!dirtySince) dirtySince = now;
    if (!playing && now - dirtySince > 3000) { patternSave(); dirtySince = 0; }
  } else {
    dirtySince = 0;
  }
  showPads();
  showButtonLeds();
  settingsSaveWhenIdle();
}
