// Core self-test: runs once the USB serial is opened, prints "PASS name" / "FAIL name: detail"
// and ends with "DONE pass=N fail=M". host/core_test.sh builds, uploads and reads it, and looks
// for the BLE advertising ("SMC-TEST") at the same time.
#include <SMCPad.h>
#include <MIDIUSB.h>
#include <ArduinoBLE.h>

static int passed, failed;
extern "C" size_t smcpad_malloc_usable_size(void *p);

static void check(const char *name, bool ok, const String &detail = String())
{
  Serial.print(ok ? "PASS " : "FAIL ");
  Serial.print(name);
  if (detail.length()) {
    Serial.print(": ");
    Serial.print(detail);
  }
  Serial.println();
  ok ? passed++ : failed++;
}

// ---------------------------------------------------------------- startup state

int initialized = 12345;                    // .data
static uint8_t zeroed[4096];                // .bss (a .bss.* section with -fdata-sections)
static bool ctorRan;
struct Global { int v; Global() : v(42) { ctorRan = true; } } global;

struct Base { virtual int f() { return 1; } virtual ~Base() {} };
struct Derived : Base { int f() override { return 2; } };

static int staticLocal() { static int calls = 0; return ++calls; }

template <typename T> T twice(T v) { return v + v; }

static void testStartup()
{
  check("data initialised", initialized == 12345, String(initialized));
  bool allZero = true;
  for (unsigned i = 0; i < sizeof(zeroed); i++) allZero &= zeroed[i] == 0;
  check("bss zeroed", allZero);
  check("global constructor", ctorRan && global.v == 42);
  Base *b = new Derived;
  check("virtual call", b->f() == 2);
  delete b;
  staticLocal();
  check("static local", staticLocal() == 2);
  check("template", twice(21) == 42 && twice(1.5f) == 3.0f);
}

// ---------------------------------------------------------------- time

static void testTime()
{
  unsigned long t0 = millis();
  delay(100);
  unsigned long dt = millis() - t0;
  check("delay(100)", dt >= 100 && dt <= 103, String(dt) + " ms");

  unsigned long u0 = micros();
  delay(20);
  unsigned long du = micros() - u0;
  check("micros over 20 ms", du >= 19000 && du <= 22000, String(du) + " us");

  u0 = micros();
  for (int i = 0; i < 10; i++) delayMicroseconds(1000);
  du = micros() - u0;
  check("delayMicroseconds 10x1000", du >= 9000 && du <= 12000, String(du) + " us");
}

// ---------------------------------------------------------------- numbers and text

static void testNumbers()
{
  uint64_t a = 1234567890123ULL;
  check("64-bit division", a / 7 == 176366841446ULL && a % 7 == 1);
  int64_t s = -9876543210LL;
  check("64-bit signed", s / 3 == -3292181070LL);
  volatile float f = 2.0f;
  check("sqrt", fabs(sqrt(f) - 1.41421356f) < 1e-5);
  check("sin/cos", fabs(sin(1.0) - 0.841470985) < 1e-6 && fabs(cos(1.0) - 0.540302306) < 1e-6);
  check("pow", fabs(pow(2.0, 10.0) - 1024.0) < 1e-9);
  check("map/constrain", map(5, 0, 10, 0, 100) == 50 && constrain(300, 0, 255) == 255);
  randomSeed(1);
  long r = random(10, 20);
  check("random", r >= 10 && r < 20, String(r));

  check("String(int)", String(-1234) == "-1234");
  check("String(HEX)", String(255, HEX) == "ff");
  check("String(float, 3)", String(3.14159f, 3) == "3.142", String(3.14159f, 3));
  check("String(double)", String(-2.5) == "-2.50", String(-2.5));
  char buf[64];
  snprintf(buf, sizeof(buf), "%d %s %x %5u", -7, "ok", 0xBEEF, 42u);
  check("snprintf ints", strcmp(buf, "-7 ok beef    42") == 0, buf);
  struct { const char *expect; int n; } cases[] = {
    { "  -3.142", snprintf(buf, sizeof(buf), "%8.3f", -3.14159) },
    { "1.234568e+04", 0 }, { "0.0001", 0 }, { "1e-05", 0 }, { "1.23457e+08", 0 },
    { "-0042", 0 }, { "ab   |", 0 }, { "1099511627776", 0 }, { "0xff", 0 }, { "abc", 0 },
    { "+0.1", 0 }, { "0.000000", 0 }, { "100", 0 }, { "inf -inf", 0 }, { "3.5 2.25", 0 },
  };
  (void)cases;
  const char *fmts[] = { "%8.3f", "%e", "%g", "%g", "%g", "%05d", "%-5s|", "%llu", "%#x",
                         "%.3s", "%+.1f", "%f", "%.0f", "%g %g", "%g %g" };
  for (unsigned i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
    switch (i) {
      case 0: snprintf(buf, sizeof(buf), fmts[i], -3.14159); break;
      case 1: snprintf(buf, sizeof(buf), fmts[i], 12345.678); break;
      case 2: snprintf(buf, sizeof(buf), fmts[i], 0.0001); break;
      case 3: snprintf(buf, sizeof(buf), fmts[i], 1e-5); break;
      case 4: snprintf(buf, sizeof(buf), fmts[i], 123456789.0); break;
      case 5: snprintf(buf, sizeof(buf), fmts[i], -42); break;
      case 6: snprintf(buf, sizeof(buf), fmts[i], "ab"); break;
      case 7: snprintf(buf, sizeof(buf), fmts[i], 1ULL << 40); break;
      case 8: snprintf(buf, sizeof(buf), fmts[i], 255); break;
      case 9: snprintf(buf, sizeof(buf), fmts[i], "abcdef"); break;
      case 10: snprintf(buf, sizeof(buf), fmts[i], 0.05); break;
      case 11: snprintf(buf, sizeof(buf), fmts[i], 0.0); break;
      case 12: snprintf(buf, sizeof(buf), fmts[i], 99.5); break;
      case 13: snprintf(buf, sizeof(buf), fmts[i], 1.0 / 0.0, -1.0 / 0.0); break;
      case 14: snprintf(buf, sizeof(buf), fmts[i], 3.5f, 2.25); break;
    }
    String name = String("snprintf ") + fmts[i];
    check(name.c_str(), strcmp(buf, cases[i].expect) == 0, buf);
  }
  check("snprintf truncates", snprintf(buf, 4, "%d", 123456) == 6 && strcmp(buf, "123") == 0, buf);
}

static void testString()
{
  String s = "Hello";
  s += ", ";
  s += "world";
  s += '!';
  check("String concat", s == "Hello, world!", s);
  check("String substring/indexOf", s.substring(7, 12) == "world" && s.indexOf('w') == 7);
  String n = "  42  ";
  n.trim();
  check("String trim/toInt", n.toInt() == 42);
  check("String toFloat", fabs(String("2.75").toFloat() - 2.75f) < 1e-6);
  String rep = "a-b-c";
  rep.replace("-", "+");
  check("String replace", rep == "a+b+c", rep);

  // growth through realloc: content must survive every step
  String big;
  bool ok = true;
  for (int i = 0; i < 3000 && ok; i++) {
    big += (char)('a' + i % 26);
    ok = big.length() == (unsigned)(i + 1) && big[i] == 'a' + i % 26 && big[0] == 'a';
  }
  bool content = ok;
  for (unsigned i = 0; i < big.length() && content; i++) content = big[i] == 'a' + i % 26;
  check("String growth to 3000", ok && content, String(big.length()));
}

// ---------------------------------------------------------------- heap

static void testHeap()
{
  // what the allocator keeps in front of a block (for realloc)
  static const int sizes[] = {1, 13, 100, 1000};
  for (int n : sizes) {
    void *p = malloc(n);
    size_t usable = smcpad_malloc_usable_size(p);
    if (usable < (size_t)n || usable > (size_t)n + 16) {
      check("malloc block size", false, String(n) + " -> " + String(usable));
      return;
    }
    free(p);
  }

  check("malloc block size", true);

  uint8_t *c = (uint8_t *)calloc(100, 3);
  bool zero = c != nullptr;
  for (int i = 0; c && i < 300; i++) zero &= c[i] == 0;
  check("calloc zeroes", zero);
  free(c);

  uint8_t *p = (uint8_t *)malloc(10);
  for (int i = 0; i < 10; i++) p[i] = i;
  p = (uint8_t *)realloc(p, 5000);
  bool kept = p != nullptr;
  for (int i = 0; p && i < 10; i++) kept &= p[i] == i;
  check("realloc grow keeps data", kept);
  p = (uint8_t *)realloc(p, 4);
  kept = p != nullptr;
  for (int i = 0; p && i < 4; i++) kept &= p[i] == i;
  check("realloc shrink keeps data", kept);
  free(p);
  check("realloc(NULL)", (p = (uint8_t *)realloc(nullptr, 16)) != nullptr);
  free(p);

  int *arr = new int[100];
  for (int i = 0; i < 100; i++) arr[i] = i * i;
  check("new[]", arr[99] == 9801);
  delete[] arr;

  // churn: no corruption, nothing lost
  void *blocks[64];
  for (int round = 0; round < 20; round++) {
    for (int i = 0; i < 64; i++) {
      blocks[i] = malloc(16 + (i * 37 + round * 11) % 400);
      memset(blocks[i], i, 16);
    }
    for (int i = 0; i < 64; i += 2) free(blocks[i]);
    for (int i = 1; i < 64; i += 2) {
      if (((uint8_t *)blocks[i])[15] != i) { check("heap churn", false, String(i)); return; }
      free(blocks[i]);
    }
  }
  check("heap churn", true);
}

// ---------------------------------------------------------------- hardware service and USB

static void testHardware()
{
  uint32_t r0 = smcpad_pad_rounds();
  delay(1000);
  uint32_t rate = smcpad_pad_rounds() - r0;
  check("pad scan rate", rate >= 250 && rate <= 450, String(rate) + "/s");

  bool any = false;
  for (int pad = 0; pad < SMCPadClass::PADS; pad++) any |= SMCPad.padPressure(pad) > 150;
  check("pads at rest", !any);
  bool held = false;
  for (int b = 0; b < SMCPAD_NUM_BUTTONS; b++) held |= SMCPad.button(b);
  check("buttons at rest", !held);

  uint8_t packet[4] = {0x0B, 0xB0, 119, 0};   // CC 119 = 0 on channel 1: harmless
  check("USB-MIDI send", smcpad_usbmidi_write(packet) == 1);
}

static void testBLE()
{
  unsigned long t0 = millis();
  int ok = BLE.begin();
  check("BLE.begin", ok == 1, String(millis() - t0) + " ms");
  if (!ok) return;
  BLE.setLocalName("SMC-TEST");
  BLEService svc("19b10000-e8f2-537e-4f6c-d104768a1299");
  BLEByteCharacteristic ch("19b10001-e8f2-537e-4f6c-d104768a1299", BLERead | BLENotify);
  svc.addCharacteristic(ch);
  BLE.addService(svc);
  BLE.setAdvertisedService(svc);
  check("BLE.advertise", BLE.advertise() == 1);
  String addr = BLE.address();
  check("BLE.address", addr.length() == 17 && addr != "00:00:00:00:00:00", addr);
  Serial.println("info ble advertising as SMC-TEST " + addr);
}

void setup()
{
  while (!Serial) delay(10);
  delay(200);
  Serial.println("CoreTest start");
  testStartup();
  testTime();
  testNumbers();
  testString();
  testHeap();
  testHardware();
  testBLE();
  Serial.print("DONE pass=");
  Serial.print(passed);
  Serial.print(" fail=");
  Serial.println(failed);
}

void loop()
{
  BLE.poll();
}
