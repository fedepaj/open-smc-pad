// jl-usbkey: forces a JieLi chip (SMC-PAD: AC6966C / BR25) into its MaskROM USB
// download mode ("UBOOT") by sending the USB_KEY on the USB data lines at power-up.
// Ref: tools/jielie/isp/usb/usb-key.md
//
// Wiring (Raspberry Pi Pico, arduino-pico core):
//   GP2 --[270R]-- USB D+  (green)   key clock
//   GP3 --[270R]-- USB D-  (white)   key data
//   GND ---------- USB GND (black)
// The SMC-PAD stays connected to the computer through the tapped cable.
//
// Use: open the serial monitor (or just watch the LED), then power on the SMC-PAD.
// LED fast blink = sending key, solid = chip entered UBOOT, slow blink = timed out.
// Press BOOTSEL or send 'r' over serial to start an attempt; 'x' aborts one, 'r' restarts it;
// 'd' replugs the pad as far as the host can tell.

#include <Arduino.h>

constexpr uint PIN_DP = 2;            // D+ : clock, data latched on rising edge
constexpr uint PIN_DM = 3;            // D- : data, MSB first
constexpr uint16_t USB_KEY = 0x16EF;
constexpr uint32_t HALF_PERIOD_US = 10;   // ~50 kHz clock
constexpr uint32_t SENSE_US = 3;          // settle time before reading released D+
constexpr uint32_t TIMEOUT_MS = 60000;
constexpr uint32_t ARM_LOW_MS = 10;       // D+ must stay low this long before keying

static void releaseBus() {
  gpio_set_dir(PIN_DP, GPIO_IN);
  gpio_set_dir(PIN_DM, GPIO_IN);
}

static void driveBus() {
  gpio_put(PIN_DP, 0);
  gpio_put(PIN_DM, 0);
  gpio_set_dir(PIN_DP, GPIO_OUT);
  gpio_set_dir(PIN_DM, GPIO_OUT);
}

// D+ is only ever released while the clock is low, so seeing it high means the chip
// has acknowledged the key and enabled its 1.5k pull-up for SOF calibration.
static bool chipPulledUp() {
  gpio_set_dir(PIN_DP, GPIO_IN);
  busy_wait_us_32(SENSE_US);
  bool up = gpio_get(PIN_DP);
  if (!up) {
    gpio_put(PIN_DP, 0);
    gpio_set_dir(PIN_DP, GPIO_OUT);
  }
  return up;
}

// Returns true once the chip has taken the bus.
static bool sendKeyOnce() {
  for (int bit = 15; bit >= 0; bit--) {
    if (chipPulledUp()) {
      releaseBus();
      return true;
    }
    gpio_put(PIN_DM, (USB_KEY >> bit) & 1);
    busy_wait_us_32(HALF_PERIOD_US - SENSE_US);
    gpio_put(PIN_DP, 1);
    busy_wait_us_32(HALF_PERIOD_US);
    gpio_put(PIN_DP, 0);
  }
  return false;
}

// D+ high before the chip was ever seen low is not an acknowledgement: it is a firmware still
// attached to the host (full-speed idle), or one that dropped off USB but left its pull-up on.
// Leave the lines alone until D+ goes low, then start keying.
static bool dpReleasedHigh() {
  gpio_set_dir(PIN_DP, GPIO_IN);
  gpio_set_dir(PIN_DM, GPIO_IN);
  busy_wait_us_32(SENSE_US);
  return gpio_get(PIN_DP);
}

// A character during an attempt: 'x' aborts it, 'r' starts it over. Never queue attempts.
static int pollSerial() {
  int c = -1;
  while (Serial.available()) c = Serial.read();
  return c;
}

static void runAttempt() {
  Serial.println("Sending USB_KEY 0x16EF - power on the SMC-PAD now...");
  releaseBus();
  bool armed = false;
  uint32_t start = millis(), keys = 0, lastBlink = 0;
  bool led = false;
  while (millis() - start < TIMEOUT_MS) {
    int c = pollSerial();
    if (c == 'x') {
      releaseBus();
      Serial.println("Aborted.");
      digitalWrite(LED_BUILTIN, LOW);
      return;
    }
    if (c == 'r') {
      releaseBus();
      armed = false;
      start = millis();
      keys = 0;
      Serial.println("Restarted.");
    }
    if (!armed) {
      // Low for a moment is not enough: a device still attached sees the host's packets
      // toggle D+. Only a detached bus stays low.
      uint32_t lowSince = millis();
      while (!dpReleasedHigh()) {
        if (millis() - lowSince >= ARM_LOW_MS) {
          armed = true;
          break;
        }
        busy_wait_us_32(50);
      }
      if (!armed) {
        delay(1);
        continue;
      }
      Serial.println("D+ low: keying.");
      driveBus();
    }
    if (sendKeyOnce()) {
      digitalWrite(LED_BUILTIN, HIGH);
      Serial.printf("OK: chip took the bus after %lu keys (%lu ms). Bus released.\n",
                    keys, millis() - start);
      Serial.println("The SMC-PAD should now enumerate as a UBOOT mass-storage device.");
      return;
    }
    keys++;
    if (millis() - lastBlink > 50) {
      led = !led;
      digitalWrite(LED_BUILTIN, led);
      lastBlink = millis();
    }
  }
  releaseBus();
  Serial.println("Timeout: no response. Check wiring, then press BOOTSEL or send 'r'.");
  for (int i = 0; i < 6; i++) {
    digitalWrite(LED_BUILTIN, i & 1);
    delay(500);
  }
}

// Hold both lines low (SE0) for a while and let go: to the host this is an unplug and a replug,
// which makes it enumerate the device again after it gave up on that port.
static void replug() {
  driveBus();
  delay(300);
  releaseBus();
  Serial.println("Replugged (SE0 300 ms).");
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  gpio_init(PIN_DP);
  gpio_init(PIN_DM);
  gpio_disable_pulls(PIN_DP);
  gpio_disable_pulls(PIN_DM);
  releaseBus();
  Serial.begin(115200);
  delay(1500);
  runAttempt();
}

void loop() {
  int c = pollSerial();
  if (c == 'd') replug();
  if (BOOTSEL || c == 'r') {
    while (BOOTSEL) delay(10);
    digitalWrite(LED_BUILTIN, LOW);
    runAttempt();
  }
}
