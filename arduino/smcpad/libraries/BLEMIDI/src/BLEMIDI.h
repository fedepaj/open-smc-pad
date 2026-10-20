/*
 * BLE-MIDI (Apple / MMA "MIDI over Bluetooth LE" 1.0) on ArduinoBLE, with the MIDIUSB API: code
 * written for MidiUSB works on BLEMIDI by changing the name.
 *
 *   service        03B80E5A-EDE8-4B33-A751-6CE34EC4C700
 *   MIDI I/O char  7772E5DB-3868-4112-A1A9-F2669D106BF3  read | write w/o response | notify
 *
 * Other BLE services can live alongside: add them to BLE before BLEMIDI.begin(). SysEx is not
 * handled yet (incoming SysEx is skipped).
 */
#pragma once
#include <ArduinoBLE.h>

#ifndef MIDI_EVENT_PACKET_T_DEFINED         /* shared with MIDIUSB */
#define MIDI_EVENT_PACKET_T_DEFINED
typedef struct {
    uint8_t header;         /* cable number << 4 | code index number (usually status >> 4) */
    uint8_t byte1;
    uint8_t byte2;
    uint8_t byte3;
} midiEventPacket_t;
#endif

class BLEMIDI_ {
public:
    /* starts BLE (if needed) and advertises as a MIDI device; 1 when BLE is up */
    int begin(const char *name = "SMC-PAD");
    bool connected();                       /* a central listens (subscribed) */

    /* MIDIUSB API */
    midiEventPacket_t read();               /* header 0 when nothing is waiting */
    void sendMIDI(midiEventPacket_t event);
    size_t write(const uint8_t *buffer, size_t size);   /* whole 4-byte packets */
    void flush() {}                         /* every message goes out at once */
    int available();

    /* channel 0..15 */
    void noteOn(uint8_t channel, uint8_t note, uint8_t velocity) { send(0x90 | channel, note, velocity); }
    void noteOff(uint8_t channel, uint8_t note, uint8_t velocity = 0) { send(0x80 | channel, note, velocity); }
    void controlChange(uint8_t channel, uint8_t control, uint8_t value) { send(0xB0 | channel, control, value); }
    void programChange(uint8_t channel, uint8_t program) { send(0xC0 | channel, program, 0); }
    void pitchBend(uint8_t channel, int value)          /* -8192..8191 */
    {
        value += 8192;
        send(0xE0 | channel, value & 0x7F, (value >> 7) & 0x7F);
    }
    /* any message of 1..3 bytes (status first) */
    bool send(uint8_t status, uint8_t data1 = 0, uint8_t data2 = 0);

    BLEService &service() { return _service; }

private:
    static void onWrite(const uint8_t *data, int length, void *arg);
    void received(const uint8_t *msg, int length);

    BLEService _service{"03B80E5A-EDE8-4B33-A751-6CE34EC4C700"};
    BLECharacteristic _io{"7772E5DB-3868-4112-A1A9-F2669D106BF3",
                          BLERead | BLEWriteWithoutResponse | BLENotify, 20};
    midiEventPacket_t _rx[32];
    volatile uint8_t _head = 0, _tail = 0;
};

extern BLEMIDI_ BLEMIDI;
