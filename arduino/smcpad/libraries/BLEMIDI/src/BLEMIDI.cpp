#include "BLEMIDI.h"

BLEMIDI_ BLEMIDI;

/* bytes of a message by its status */
static int midi_length(uint8_t status)
{
    if (status < 0xF0) {
        return (status & 0xE0) == 0xC0 ? 2 : 3;     /* program change / channel pressure: 2 */
    }
    return status == 0xF2 ? 3 : (status == 0xF1 || status == 0xF3) ? 2 : 1;
}

/* USB-MIDI code index number of a message */
static uint8_t cin(uint8_t status)
{
    if (status < 0xF0) {
        return status >> 4;
    }
    if (status >= 0xF8) {
        return 0x0F;                                /* real time: single byte */
    }
    return midi_length(status) == 3 ? 0x03 : midi_length(status) == 2 ? 0x02 : 0x05;
}

/* bytes of a message by its code index number (0 = not one we send) */
static int cin_length(uint8_t c)
{
    switch (c & 0x0F) {
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xE: case 0x3:
        return 3;
    case 0xC: case 0xD: case 0x2:
        return 2;
    case 0x5: case 0xF:
        return 1;
    default:
        return 0;
    }
}

int BLEMIDI_::begin(const char *name)
{
    static bool added;

    if (!BLE.begin()) {
        return 0;
    }
    if (!added) {
        _io.setRawWriteCallback(onWrite, this);
        _service.addCharacteristic(_io);
        BLE.addService(_service);
        added = true;
    }
    BLE.setLocalName(name);
    BLE.setDeviceName(name);
    BLE.setAdvertisedService(_service);
    BLE.advertise();
    return 1;
}

bool BLEMIDI_::connected()
{
    return BLE.connected() && _io.subscribed();
}

/* ------------------------------------------------------------------ outgoing */

bool BLEMIDI_::send(uint8_t status, uint8_t data1, uint8_t data2)
{
    uint8_t pkt[5];
    uint32_t ts = millis() & 0x1FFF;
    int n = midi_length(status);

    if (!(status & 0x80) || status == 0xF0 || status == 0xF7) {
        return false;                               /* not a status, or SysEx */
    }
    pkt[0] = 0x80 | (ts >> 7);                      /* header: timestamp high */
    pkt[1] = 0x80 | (ts & 0x7F);                    /* timestamp low */
    pkt[2] = status;
    pkt[3] = data1 & 0x7F;
    pkt[4] = data2 & 0x7F;
    return _io.subscribed() && _io.writeValue(pkt, 2 + n);
}

void BLEMIDI_::sendMIDI(midiEventPacket_t event)
{
    if (cin_length(event.header)) {
        send(event.byte1, event.byte2, event.byte3);
    }
}

size_t BLEMIDI_::write(const uint8_t *buffer, size_t size)
{
    for (size_t i = 0; i + 4 <= size; i += 4) {
        sendMIDI({buffer[i], buffer[i + 1], buffer[i + 2], buffer[i + 3]});
    }
    return size - size % 4;
}

/* ------------------------------------------------------------------ incoming (BT task) */

void BLEMIDI_::received(const uint8_t *msg, int length)
{
    uint8_t next = (_head + 1) % 32;

    if (next == _tail) {
        return;                                     /* full: drop */
    }
    _rx[_head] = { cin(msg[0]), msg[0], (uint8_t)(length > 1 ? msg[1] : 0), (uint8_t)(length > 2 ? msg[2] : 0) };
    _head = next;
}

/* header, then [timestamp] status data... / [timestamp] data (running status) / data
 * (running status, same timestamp). SysEx is skipped. */
void BLEMIDI_::onWrite(const uint8_t *p, int n, void *arg)
{
    BLEMIDI_ *self = (BLEMIDI_ *)arg;
    uint8_t status = 0, msg[3];
    int i = 1, have = 0;

    if (n < 2 || (p[0] & 0xC0) != 0x80) {
        return;                                     /* not a BLE-MIDI header */
    }
    while (i < n) {
        uint8_t b = p[i++];

        if (b & 0x80) {                             /* a timestamp byte */
            if (i < n && (p[i] & 0x80)) {           /* ... followed by a status byte */
                uint8_t st = p[i++];

                if (st >= 0xF8) {                   /* real time: one byte, status unchanged */
                    self->received(&st, 1);
                    continue;
                }
                status = (st == 0xF0 || st == 0xF7) ? 0 : st;   /* SysEx: skip its data */
                have = 0;
                if (status && midi_length(status) == 1) {
                    self->received(&status, 1);
                    status = 0;
                }
            }
            continue;
        }
        if (!status) {
            continue;                               /* data with no status (or SysEx) */
        }
        if (!have) {
            msg[have++] = status;
        }
        msg[have++] = b;
        if (have == midi_length(status)) {
            self->received(msg, have);
            have = 0;
        }
    }
}

int BLEMIDI_::available()
{
    return (_head - _tail + 32) % 32;
}

midiEventPacket_t BLEMIDI_::read()
{
    midiEventPacket_t p = {0, 0, 0, 0};

    BLE.poll();
    if (_tail != _head) {
        p = _rx[_tail];
        _tail = (_tail + 1) % 32;
    }
    return p;
}
