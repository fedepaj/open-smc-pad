// Everything the SMC-PAD does, on the serial monitor; button LEDs follow the buttons, pads light
// up as hard as they are hit.
#include <SMCPad.h>

const char *buttonNames[] = { "bluetooth", "pad bank", "knob bank", "left", "right",
                              "play", "pause", "rec", "shift", "note repeat" };

void padPressed(int pad, int velocity) {
  Serial.print("pad "); Serial.print(pad); Serial.print(" velocity "); Serial.println(velocity);
  SMCPad.padColor(pad, 0, velocity * 2, velocity / 2);
  SMCPad.show();
}

void padReleased(int pad) {
  Serial.print("pad "); Serial.print(pad); Serial.println(" released");
  SMCPad.padColor(pad, 0);
  SMCPad.show();
}

void buttonPressed(int b) {
  Serial.print(buttonNames[b]); Serial.println(" pressed");
  SMCPad.buttonLed(b, true);
}

void buttonReleased(int b) {
  SMCPad.buttonLed(b, false);
}

void knobTurned(int knob, int delta) {
  Serial.print("knob "); Serial.print(knob + 1); Serial.print(delta > 0 ? " + " : " - ");
  Serial.println(SMCPad.knob(knob));
}

void setup() {
  Serial.begin(115200);
  SMCPad.onPadPress(padPressed);
  SMCPad.onPadRelease(padReleased);
  SMCPad.onButtonPress(buttonPressed);
  SMCPad.onButtonRelease(buttonReleased);
  SMCPad.onKnob(knobTurned);
}

void loop() {
  SMCPad.update();
}
