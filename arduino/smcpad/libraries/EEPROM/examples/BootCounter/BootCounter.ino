// Counts the boots in EEPROM and prints the count on the USB serial; the pads light up to
// the count (mod 16). Survives power off and sketch uploads.
#include <EEPROM.h>
#include <SMCPad.h>

void setup() {
  uint32_t boots;
  EEPROM.get(0, boots);
  if (boots == 0xFFFFFFFF) boots = 0;       // never written
  boots++;
  EEPROM.put(0, boots);
  EEPROM.commit();
  for (int pad = 0; pad < 16; pad++) SMCPad.padColor(pad, pad < (int)(boots % 16) ? 0x002010 : 0);
  SMCPad.show();
  while (!Serial) delay(10);
  Serial.print("boot number ");
  Serial.println(boots);
}

void loop() {}
