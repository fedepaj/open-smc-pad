// The pads as a Bluetooth MIDI drum controller: notes 36..51 on channel 10 with velocity, the
// pad lit while held; notes coming back light the pads too. The knobs send CC 70..77.
// On a Mac: Audio MIDI Setup > Window > Show MIDI Studio > Bluetooth > Connect "SMC-PAD".
#include <SMCPad.h>
#include <BLEMIDI.h>

const uint8_t CHANNEL = 9;      // 0-based: MIDI channel 10
int knobValue[8];

void padPressed(int pad, int velocity) {
  BLEMIDI.noteOn(CHANNEL, 36 + pad, velocity);
  SMCPad.padColor(pad, 0, velocity * 2, 0);
  SMCPad.show();
}

void padReleased(int pad) {
  BLEMIDI.noteOff(CHANNEL, 36 + pad);
  SMCPad.padColor(pad, 0);
  SMCPad.show();
}

void knobTurned(int knob, int delta) {
  knobValue[knob] = constrain(knobValue[knob] + delta, 0, 127);
  BLEMIDI.controlChange(CHANNEL, 70 + knob, knobValue[knob]);
}

void setup() {
  BLEMIDI.begin("SMC-PAD");
  SMCPad.onPadPress(padPressed);
  SMCPad.onPadRelease(padReleased);
  SMCPad.onKnob(knobTurned);
}

void loop() {
  SMCPad.update();
  SMCPad.buttonLed(0, BLEMIDI.connected());   // the bluetooth LED: on while a host listens
  midiEventPacket_t rx = BLEMIDI.read();
  if (rx.header == 0x09 || rx.header == 0x08) {             // note on / off from the host
    int pad = rx.byte2 - 36;
    if (pad >= 0 && pad < SMCPadClass::PADS) {
      SMCPad.padColor(pad, rx.header == 0x09 && rx.byte3 ? 0x0000A0 : 0);
      SMCPad.show();
    }
  }
}
