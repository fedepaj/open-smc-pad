/*
 * A custom BLE service: the last pad hit (notify: pad number and velocity) and the pad colour
 * (write 3 bytes, R G B). Any BLE app (nRF Connect, LightBlue...) can talk to it.
 */
#include <ArduinoBLE.h>
#include <SMCPad.h>

BLEService padService("19b10000-e8f2-537e-4f6c-d104768a1214");
BLECharacteristic hitChar("19b10001-e8f2-537e-4f6c-d104768a1214", BLERead | BLENotify, 2, true);
BLECharacteristic colorChar("19b10002-e8f2-537e-4f6c-d104768a1214", BLERead | BLEWrite | BLEWriteWithoutResponse, 3, true);
BLEDescriptor hitLabel("2901", "pad, velocity");

uint32_t color = 0x000020;

void showColor() {
  for (int pad = 0; pad < 16; pad++) SMCPad.padColor(pad, color);
  SMCPad.show();
}

void onHit(int pad, int velocity) {
  uint8_t v[2] = { (uint8_t)pad, (uint8_t)velocity };
  hitChar.writeValue(v, 2);
}

void onColor(BLEDevice central, BLECharacteristic c) {
  const uint8_t *v = c.value();
  color = (uint32_t)v[0] << 16 | (uint32_t)v[1] << 8 | v[2];
  showColor();
}

void onConnect(BLEDevice central) {
  Serial.print("connected: ");
  Serial.println(central.address());
  SMCPad.buttonLed(0, true);            // the bluetooth LED
}

void onDisconnect(BLEDevice central) {
  Serial.println("disconnected");
  SMCPad.buttonLed(0, false);
}

void setup() {
  Serial.begin(115200);
  if (!BLE.begin()) {
    Serial.println("BLE did not start");
    while (1) delay(1000);
  }
  BLE.setLocalName("SMC-PAD");
  BLE.setAdvertisedService(padService);
  hitChar.addDescriptor(hitLabel);
  padService.addCharacteristic(hitChar);
  padService.addCharacteristic(colorChar);
  BLE.addService(padService);
  uint8_t c[3] = { 0, 0, 0x20 };
  colorChar.writeValue(c, 3);
  colorChar.setEventHandler(BLEWritten, onColor);
  BLE.setEventHandler(BLEConnected, onConnect);
  BLE.setEventHandler(BLEDisconnected, onDisconnect);
  BLE.advertise();
  SMCPad.onPadPress(onHit);
  showColor();
  Serial.print("advertising, address ");
  Serial.println(BLE.address());
}

void loop() {
  BLE.poll();
  SMCPad.update();
}
