// MIDI out to every connected port (USB, and BLE when a host listens), MIDI in from both.
#pragma once
#include <MIDIUSB.h>
#include <BLEMIDI.h>

static bool bleStarted;

static void midiSend(uint8_t status, uint8_t d1 = 0, uint8_t d2 = 0) {
  MidiUSB.sendMIDI({(uint8_t)(status >> 4), status, d1, d2});
  MidiUSB.flush();
  if (bleStarted && BLEMIDI.connected()) BLEMIDI.send(status, d1, d2);
}

static void noteOn(uint8_t ch, uint8_t note, uint8_t vel) { midiSend(0x90 | ch, note, vel); }
static void noteOff(uint8_t ch, uint8_t note) { midiSend(0x80 | ch, note, 0); }
static void cc(uint8_t ch, uint8_t num, uint8_t value) { midiSend(0xB0 | ch, num, value); }

// the next incoming packet from USB, else BLE; header 0 when none
static midiEventPacket_t midiRead() {
  midiEventPacket_t p = MidiUSB.read();
  if (p.header == 0 && bleStarted) p = BLEMIDI.read();
  return p;
}

// a SysEx (F0 ... F7) to USB only, packed in USB-MIDI packets (CIN 4 = 3 bytes going on,
// 5 / 6 / 7 = the last 1 / 2 / 3)
static void sysexSend(const uint8_t *msg, int len) {
  for (int i = 0; i < len; i += 3) {
    int n = len - i < 3 ? len - i : 3;
    uint8_t cin = len - i > 3 ? 0x4 : 0x4 + n;
    MidiUSB.sendMIDI({cin, msg[i], (uint8_t)(n > 1 ? msg[i + 1] : 0), (uint8_t)(n > 2 ? msg[i + 2] : 0)});
  }
  MidiUSB.flush();
}
