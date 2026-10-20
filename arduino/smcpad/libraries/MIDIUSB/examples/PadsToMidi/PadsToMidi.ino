// The pads as a USB-MIDI drum controller: notes 36..51 on channel 10 with velocity, the pad lit
// while held; notes coming back from the computer light the pads too.
#include <SMCPad.h>
#include <MIDIUSB.h>

const uint8_t CHANNEL = 9;      // 0-based: MIDI channel 10

void padPressed(int pad, int velocity) {
  MidiUSB.sendMIDI({0x09, (uint8_t)(0x90 | CHANNEL), (uint8_t)(36 + pad), (uint8_t)velocity});
  MidiUSB.flush();
  SMCPad.padColor(pad, 0, velocity * 2, 0);
  SMCPad.show();
}

void padReleased(int pad) {
  MidiUSB.sendMIDI({0x08, (uint8_t)(0x80 | CHANNEL), (uint8_t)(36 + pad), 0});
  MidiUSB.flush();
  SMCPad.padColor(pad, 0);
  SMCPad.show();
}

void setup() {
  SMCPad.onPadPress(padPressed);
  SMCPad.onPadRelease(padReleased);
}

void loop() {
  SMCPad.update();
  midiEventPacket_t rx = MidiUSB.read();
  if (rx.header == 0x09 || rx.header == 0x08) {             // note on / off from the host
    int pad = rx.byte2 - 36;
    if (pad >= 0 && pad < SMCPadClass::PADS) {
      SMCPad.padColor(pad, rx.header == 0x09 && rx.byte3 ? 0x0000A0 : 0);
      SMCPad.show();
    }
  }
}
