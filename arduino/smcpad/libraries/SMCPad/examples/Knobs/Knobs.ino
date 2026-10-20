// The eight knobs as values 0..127 (like MIDI CCs) on the serial monitor. Clockwise goes up.
#include <SMCPad.h>

int value[SMCPadClass::KNOBS];

void knobTurned(int knob, int delta) {
  value[knob] = constrain(value[knob] + delta, 0, 127);
  Serial.print("knob "); Serial.print(knob + 1); Serial.print(" = "); Serial.println(value[knob]);
}

void setup() {
  Serial.begin(115200);
  SMCPad.onKnob(knobTurned);
}

void loop() {
  SMCPad.update();
}
