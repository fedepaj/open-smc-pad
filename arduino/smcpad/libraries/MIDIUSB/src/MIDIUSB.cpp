#include "MIDIUSB.h"

MIDI_ MidiUSB;

midiEventPacket_t MIDI_::read()
{
    midiEventPacket_t p = { 0, 0, 0, 0 };
    smcpad_usbmidi_read((uint8_t *)&p);
    return p;
}

void MIDI_::sendMIDI(midiEventPacket_t event)
{
    smcpad_usbmidi_write((const uint8_t *)&event);
}

size_t MIDI_::write(const uint8_t *buffer, size_t size)
{
    size_t n = 0;
    for (; n + 4 <= size; n += 4) {
        smcpad_usbmidi_write(buffer + n);
    }
    return n;
}

int MIDI_::available()
{
    return 0;   /* read() returns header 0 when empty, as in Arduino's library */
}
