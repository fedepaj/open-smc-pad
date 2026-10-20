// A slowly turning rainbow on the pads; a hit flashes the pad white.
#include <SMCPad.h>

uint32_t flashUntil[SMCPadClass::PADS];

uint32_t wheel(uint8_t pos) {
  if (pos < 85) return ((uint32_t)(255 - pos * 3) << 16) | ((uint32_t)(pos * 3) << 8);
  if (pos < 170) { pos -= 85; return ((uint32_t)(255 - pos * 3) << 8) | (pos * 3); }
  pos -= 170; return ((uint32_t)(pos * 3) << 16) | (255 - pos * 3);
}

void padPressed(int pad, int velocity) {
  flashUntil[pad] = millis() + 150;
}

void setup() {
  SMCPad.onPadPress(padPressed);
}

void loop() {
  SMCPad.update();
  static uint32_t next;
  if (millis() >= next) {
    next = millis() + 20;
    uint8_t base = millis() / 20;
    for (int p = 0; p < SMCPadClass::PADS; p++) {
      uint32_t c = millis() < flashUntil[p] ? 0xFFFFFF : wheel(base + p * 16);
      SMCPad.padColor(p, (c >> 18 & 0x3F) << 16 | (c >> 10 & 0x3F) << 8 | (c >> 2 & 0x3F));  // 1/4 brightness
    }
    SMCPad.show();
  }
}
