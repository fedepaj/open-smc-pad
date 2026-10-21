/*
 * The SMC-PAD runtime (smcpad_rt.h), emulated on the host: a sketch and the real libraries
 * (SMCPad, MIDIUSB, ArduinoBLE, BLEMIDI, EEPROM) and core (wiring.cpp, SerialUSB.cpp,
 * ArduinoCore-API) are compiled unchanged against it. Time is virtual; a scenario (the
 * gestures) drives the pads, buttons and knobs; LEDs and MIDI are recorded as frames.
 *
 * Scenario, one event per line (# comments), times in ms:
 *   <t> pad <0-15> down <velocity> <pressure>   <t> pad <n> up   <t> pressure <n> <0-1023>
 *   <t> button <0-9> down|up                     <t> knob <0-7> <delta>
 *   <t> midi <hex bytes...>                      (incoming, e.g. F8, or a whole SysEx)
 * Output (stdout, JSON lines): {"t":ms,"pads":[16 x 0xRRGGBB],"leds":[10 x 0/1]} per frame,
 * {"t":ms,"midi":[bytes]} per outgoing message.
 */
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "smcpad_rt.h"

// ---------------------------------------------------------------- the virtual pad

static uint64_t now_us;
static uint64_t end_us, next_frame_us, frame_us = 40000;     // 25 fps

static uint32_t rgb_pending[SMCPAD_NUM_PADS], rgb_shown[SMCPAD_NUM_PADS];
static int led[SMCPAD_NUM_BUTTONS];
static uint16_t pad_raw[SMCPAD_NUM_PADS];
static int button_held[SMCPAD_NUM_BUTTONS];
static int32_t encoder[SMCPAD_NUM_ENCODERS];
static std::deque<smcpad_event_t> events;
static std::deque<std::vector<uint8_t>> midi_in;           // whole messages
static std::deque<uint8_t> midi_in_packets;                // USB-MIDI packets, 4 bytes each

struct ScenarioEvent { uint64_t at; std::string kind; std::vector<std::string> args; };
static std::vector<ScenarioEvent> scenario;
static size_t scenario_pos;

static void push(uint8_t type, int index, int value)
{
    events.push_back(smcpad_event_t{type, (uint8_t)index, (int16_t)value});
}

// incoming MIDI message -> USB-MIDI packets (as the host would send them)
static void midi_in_message(const std::vector<uint8_t> &m)
{
    auto packet = [](uint8_t cin, uint8_t a, uint8_t b, uint8_t c) {
        midi_in_packets.push_back(cin);
        midi_in_packets.push_back(a);
        midi_in_packets.push_back(b);
        midi_in_packets.push_back(c);
    };
    if (m.empty()) return;
    if (m[0] == 0xF0) {
        for (size_t i = 0; i < m.size(); i += 3) {
            size_t n = m.size() - i < 3 ? m.size() - i : 3;
            uint8_t cin = m.size() - i > 3 ? 0x4 : (uint8_t)(0x4 + n);
            packet(cin, m[i], n > 1 ? m[i + 1] : 0, n > 2 ? m[i + 2] : 0);
        }
    } else if (m[0] >= 0xF8) {
        packet(0xF, m[0], 0, 0);
    } else {
        packet(m[0] >> 4, m[0], m.size() > 1 ? m[1] : 0, m.size() > 2 ? m[2] : 0);
    }
}

static void apply_scenario()
{
    while (scenario_pos < scenario.size() && scenario[scenario_pos].at <= now_us) {
        const ScenarioEvent &e = scenario[scenario_pos++];
        auto arg = [&](size_t i) { return i < e.args.size() ? atoi(e.args[i].c_str()) : 0; };
        if (e.kind == "pad") {
            int p = arg(0);
            if (e.args.size() > 1 && e.args[1] == "down") {
                pad_raw[p] = (uint16_t)arg(3);
                push(SMCPAD_EV_PAD_ON, p, arg(2));
            } else {
                pad_raw[p] = 0;
                push(SMCPAD_EV_PAD_OFF, p, 0);
            }
        } else if (e.kind == "pressure") {
            pad_raw[arg(0)] = (uint16_t)arg(1);
        } else if (e.kind == "button") {
            int b = arg(0);
            bool down = e.args.size() > 1 && e.args[1] == "down";
            button_held[b] = down;
            push(down ? SMCPAD_EV_BUTTON_DOWN : SMCPAD_EV_BUTTON_UP, b, 0);
        } else if (e.kind == "knob") {
            int k = arg(0), d = arg(1), step = d > 0 ? 1 : -1;
            encoder[k] += d;
            for (int i = 0; i != d; i += step) push(SMCPAD_EV_ENCODER, k, step);
        } else if (e.kind == "midi") {
            std::vector<uint8_t> m;
            for (auto &h : e.args) m.push_back((uint8_t)strtol(h.c_str(), nullptr, 16));
            midi_in_message(m);
        }
    }
}

static void frame()
{
    printf("{\"t\":%.1f,\"pads\":[", now_us / 1000.0);
    for (int i = 0; i < SMCPAD_NUM_PADS; i++) printf("%s%u", i ? "," : "", rgb_shown[i]);
    printf("],\"leds\":[");
    for (int i = 0; i < SMCPAD_NUM_BUTTONS; i++) printf("%s%d", i ? "," : "", led[i] ? 1 : 0);
    printf("]}\n");
}

// time goes on: scenario events, frames; the end of the scenario ends the program
static void advance(uint64_t us)
{
    now_us += us;
    apply_scenario();
    while (now_us >= next_frame_us) {
        frame();
        next_frame_us += frame_us;
    }
    if (now_us >= end_us) {
        fflush(stdout);
        exit(0);
    }
}

// ---------------------------------------------------------------- smcpad_rt.h

extern "C" {

uint32_t smcpad_millis(void) { return (uint32_t)(now_us / 1000); }
uint32_t smcpad_micros(void) { return (uint32_t)now_us; }
void smcpad_yield(void) { advance(100); }
void smcpad_delay(uint32_t ms)
{
    uint64_t until = now_us + (uint64_t)ms * 1000;
    while (now_us < until) advance(100);
}
void smcpad_delay_us(uint32_t us) { advance(us); }

int smcpad_serial_connected(void) { return 1; }
uint32_t smcpad_serial_write(const uint8_t *buf, uint32_t len)
{
    fwrite(buf, 1, len, stderr);
    return len;
}
int smcpad_serial_available(void) { return 0; }
int smcpad_serial_read(void) { return -1; }
int smcpad_serial_peek(void) { return -1; }
void smcpad_serial_flush(void) {}

int smcpad_usbmidi_write(const uint8_t packet[4])
{
    static const int len[16] = {0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1};
    int n = len[packet[0] & 15];
    printf("{\"t\":%.1f,\"midi\":[", now_us / 1000.0);
    for (int i = 0; i < n; i++) printf("%s%u", i ? "," : "", packet[1 + i]);
    printf("]}\n");
    return 1;
}
int smcpad_usbmidi_read(uint8_t packet[4])
{
    if (midi_in_packets.size() < 4) return 0;
    for (int i = 0; i < 4; i++) {
        packet[i] = midi_in_packets.front();
        midi_in_packets.pop_front();
    }
    return 1;
}
void smcpad_usbmidi_flush(void) {}

static std::map<int, std::vector<uint8_t>> store;
int smcpad_store_read(int key, void *buf, int len)
{
    auto it = store.find(key);
    if (it == store.end()) return -1;
    int n = (int)it->second.size() < len ? (int)it->second.size() : len;
    memcpy(buf, it->second.data(), n);
    return n;
}
int smcpad_store_write(int key, const void *buf, int len)
{
    store[key].assign((const uint8_t *)buf, (const uint8_t *)buf + len);
    return len;
}

static uint8_t flash[SMCPAD_FLASH_END - SMCPAD_FLASH_BEGIN];
static bool flash_ready;
static bool flash_range(uint32_t a, uint32_t n)
{
    if (!flash_ready) { memset(flash, 0xFF, sizeof(flash)); flash_ready = true; }
    return a >= SMCPAD_FLASH_BEGIN && a < SMCPAD_FLASH_END && n <= SMCPAD_FLASH_END - a;
}
int smcpad_flash_erase(uint32_t addr)
{
    if ((addr & 0xFFF) || !flash_range(addr, 0x1000)) return -1;
    memset(flash + addr - SMCPAD_FLASH_BEGIN, 0xFF, 0x1000);
    return 0;
}
int smcpad_flash_write(uint32_t addr, const void *data, uint32_t len)
{
    if (!flash_range(addr, len)) return -1;
    for (uint32_t i = 0; i < len; i++) flash[addr - SMCPAD_FLASH_BEGIN + i] &= ((const uint8_t *)data)[i];
    return (int)len;
}
int smcpad_flash_read(uint32_t addr, void *data, uint32_t len)
{
    if (!flash_range(addr, len)) return -1;
    memcpy(data, flash + addr - SMCPAD_FLASH_BEGIN, len);
    return (int)len;
}

void smcpad_gpio_mode(uint32_t, uint32_t) {}
void smcpad_gpio_write(uint32_t, uint32_t) {}
int smcpad_gpio_read(uint32_t) { return 0; }

void smcpad_hw_begin(void) {}
int smcpad_event_get(smcpad_event_t *ev)
{
    if (events.empty()) return 0;
    *ev = events.front();
    events.pop_front();
    return 1;
}
uint16_t smcpad_pad_raw(int pad) { return pad >= 0 && pad < SMCPAD_NUM_PADS ? pad_raw[pad] : 0; }
uint32_t smcpad_pad_rounds(void) { return (uint32_t)(now_us * 336 / 1000000); }
int smcpad_button(int b) { return b >= 0 && b < SMCPAD_NUM_BUTTONS ? button_held[b] : 0; }
int32_t smcpad_encoder(int e) { return e >= 0 && e < SMCPAD_NUM_ENCODERS ? encoder[e] : 0; }
uint32_t smcpad_matrix_raw(void) { return 0; }
void smcpad_led_set(int l, int on) { if (l >= 0 && l < SMCPAD_NUM_BUTTONS) led[l] = on; }
void smcpad_rgb_set(int pad, uint32_t rgb) { if (pad >= 0 && pad < SMCPAD_NUM_PADS) rgb_pending[pad] = rgb & 0xFFFFFF; }
void smcpad_rgb_show(void) { memcpy(rgb_shown, rgb_pending, sizeof(rgb_shown)); }

// BLE: a stack that comes up at once and never gets a central
static const smcpad_ble_callbacks_t *ble_cb;
void smcpad_ble_begin(const smcpad_ble_callbacks_t *cb) { ble_cb = cb; }
int smcpad_ble_ready(void) { return 1; }
void smcpad_ble_set_name(const char *) {}
void smcpad_ble_set_profile(const uint8_t *, uint16_t) {}
void smcpad_ble_set_adv(const uint8_t *, uint8_t, const uint8_t *, uint8_t, uint16_t, int) {}
void smcpad_ble_advertise(int) {}
uint16_t smcpad_ble_connection(void) { return 0; }
uint16_t smcpad_ble_mtu(void) { return 23; }
void smcpad_ble_address(uint8_t a[6]) { static const uint8_t x[6] = {1, 2, 3, 4, 5, 6}; memcpy(a, x, 6); }
void smcpad_ble_central_address(uint8_t a[6]) { memset(a, 0, 6); }
int smcpad_ble_rssi(void) { return 127; }
int smcpad_ble_notify(uint16_t, const uint8_t *, uint16_t) { return 0; }
int smcpad_ble_subscribed(uint16_t) { return 0; }
void smcpad_ble_disconnect(void) {}

uint32_t smcpad_battery_mv(void) { return 4100; }
int smcpad_usb_power(void) { return 1; }
void smcpad_enter_bootloader(void) { fflush(stdout); exit(0); }
void smcpad_reset(void) { fflush(stdout); exit(0); }
void smcpad_boot_ok(void) {}

}   // extern "C"

// ---------------------------------------------------------------- main

extern "C" void setup();
extern "C" void loop();

static void load_scenario(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        std::vector<std::string> words;
        for (char *w = strtok(line, " \t\r\n"); w; w = strtok(nullptr, " \t\r\n")) words.push_back(w);
        if (words.size() < 2) continue;
        ScenarioEvent e{(uint64_t)(atof(words[0].c_str()) * 1000), words[1], {}};
        e.args.assign(words.begin() + 2, words.end());
        scenario.push_back(e);
    }
    fclose(f);
    std::stable_sort(scenario.begin(), scenario.end(),
                     [](const ScenarioEvent &a, const ScenarioEvent &b) { return a.at < b.at; });
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <scenario> <duration ms> [fps]\n", argv[0]);
        return 2;
    }
    load_scenario(argv[1]);
    end_us = (uint64_t)(atof(argv[2]) * 1000);
    if (argc > 3) frame_us = (uint64_t)(1000000 / atof(argv[3]));
    setup();
    for (;;) {
        loop();
        smcpad_yield();                     // as smcpad_arduino_run() does after loop()
    }
}
