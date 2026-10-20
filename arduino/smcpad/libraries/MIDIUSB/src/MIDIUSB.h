/* Arduino's MIDIUSB API on the SMC-PAD (the USB-MIDI interface of the core's TinyUSB stack). */
#pragma once
#include <Arduino.h>

typedef struct {
    uint8_t header;         /* cable number << 4 | code index number (usually status >> 4) */
    uint8_t byte1;
    uint8_t byte2;
    uint8_t byte3;
} midiEventPacket_t;

class MIDI_ {
public:
    midiEventPacket_t read();               /* header 0 when nothing is waiting */
    void sendMIDI(midiEventPacket_t event);
    size_t write(const uint8_t *buffer, size_t size);   /* whole 4-byte packets */
    void flush() { smcpad_usbmidi_flush(); }
    int available();
};

extern MIDI_ MidiUSB;
