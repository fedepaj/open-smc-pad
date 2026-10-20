/*
 * BLE-MIDI (Apple / MMA "MIDI over Bluetooth LE" 1.0) on the JieLi BLE stack (BTstack + the
 * SDK's gatt_common module), as the stock firmware does it (its image carries the same
 * service and characteristic UUIDs).
 *
 *   service        03B80E5A-EDE8-4B33-A751-6CE34EC4C700
 *   MIDI I/O char  7772E5DB-3868-4112-A1A9-F2669D106BF3  read (empty) | write w/o response | notify
 *
 * Packets: header byte 0b10hhhhhh (timestamp bits 12..7), then per MIDI message a timestamp
 * byte 0b1lllllll (bits 6..0) and the message (running status allowed). We send one message
 * per packet; we parse what comes in and hand complete messages to blemidi_rx().
 *
 * The stack, the CCC and advertising are the runtime's (fw/smcpad/arduino/smcpad_ble.c); this
 * file gives it the profile and handles the MIDI characteristic.
 */
#include "system/app_core.h"
#include "system/includes.h"
#include "app_config.h"
#include "btstack/btstack_task.h"
#include "btstack/bluetooth.h"
#include "btcontroller_modules.h"
#include "le_common.h"
#include "le_gatt_common.h"
#include "app_comm_bt.h"
#include "smcpad_rt.h"

#if TCFG_USER_BLE_ENABLE

extern void smcpad_dbg_puts(const char *s);
extern void smcpad_dbg_kv(const char *tag, u32 value);
extern void smcpad_dbg_hex16(const char *tag, u32 value);

#define BLEMIDI_NAME            "SMC-PAD"

/* ATT database, BTstack's compiled format: entry = size(2) flags(2) handle(2) type(2 or 16)
 * value. Flags: READ 0x02, WRITE_WITHOUT_RESPONSE 0x04, WRITE 0x08, NOTIFY 0x10,
 * DYNAMIC 0x100, UUID128 0x200. 128-bit UUIDs are stored little-endian. */
#define H_GAP_NAME_VALUE        0x0003
#define H_MIDI_VALUE            0x0006
#define H_MIDI_CCC              0x0007

static const uint8_t blemidi_profile[] = {
    /* 0x0001 PRIMARY_SERVICE 1800 (GAP) */
    0x0a, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x28, 0x00, 0x18,
    /* 0x0002 CHARACTERISTIC 2a00 (device name) READ | DYNAMIC -> 0x0003 */
    0x0d, 0x00, 0x02, 0x00, 0x02, 0x00, 0x03, 0x28, 0x02, 0x03, 0x00, 0x00, 0x2a,
    /* 0x0003 VALUE 2a00 READ | DYNAMIC */
    0x08, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00, 0x2a,

    /* 0x0004 PRIMARY_SERVICE 03B80E5A-EDE8-4B33-A751-6CE34EC4C700 (MIDI) */
    0x18, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x28,
    0x00, 0xc7, 0xc4, 0x4e, 0xe3, 0x6c, 0x51, 0xa7, 0x33, 0x4b, 0xe8, 0xed, 0x5a, 0x0e, 0xb8, 0x03,
    /* 0x0005 CHARACTERISTIC 7772E5DB-... READ | WRITE_WITHOUT_RESPONSE | NOTIFY -> 0x0006 */
    0x1b, 0x00, 0x02, 0x00, 0x05, 0x00, 0x03, 0x28, 0x16, 0x06, 0x00,
    0xf3, 0x6b, 0x10, 0x9d, 0x66, 0xf2, 0xa9, 0xa1, 0x12, 0x41, 0x68, 0x38, 0xdb, 0xe5, 0x72, 0x77,
    /* 0x0006 VALUE 7772E5DB-... READ | WRITE_WITHOUT_RESPONSE | NOTIFY | DYNAMIC | UUID128 */
    0x16, 0x00, 0x16, 0x03, 0x06, 0x00,
    0xf3, 0x6b, 0x10, 0x9d, 0x66, 0xf2, 0xa9, 0xa1, 0x12, 0x41, 0x68, 0x38, 0xdb, 0xe5, 0x72, 0x77,
    /* 0x0007 CLIENT_CHARACTERISTIC_CONFIGURATION READ | WRITE | DYNAMIC */
    0x0a, 0x00, 0x0a, 0x01, 0x07, 0x00, 0x02, 0x29, 0x00, 0x00,

    /* end */
    0x00, 0x00,
};

static u16 con_handle;
static u8 notify_on;
static u32 rx_messages, tx_messages;

/* ------------------------------------------------------------------ incoming */

/* A complete MIDI message from the central (one or more per packet). */
static void blemidi_rx(const u8 *msg, u32 len)
{
    rx_messages++;
    if (len >= 3 && (msg[0] & 0xE0) == 0x80) {                 /* note off / on */
        smcpad_dbg_hex16((msg[0] & 0xF0) == 0x90 && msg[2] ? "ble-midi note on" : "ble-midi note off",
                         msg[1]);
    }
}

static u32 midi_len(u8 status)
{
    if (status < 0xF0) {
        return (status & 0xE0) == 0xC0 ? 2 : 3;     /* program change / channel pressure: 2 */
    }
    return status == 0xF2 ? 3 : (status == 0xF1 || status == 0xF3) ? 2 : 1;
}

/* header, then [timestamp] status data... / [timestamp] data (running status) / data
 * (running status, same timestamp). SysEx is skipped for now. */
static void blemidi_parse(const u8 *p, u32 n)
{
    u8 status = 0, msg[3];
    u32 i = 1, have = 0;

    if (n < 2 || (p[0] & 0xC0) != 0x80) {
        return;                                     /* not a BLE-MIDI header */
    }
    while (i < n) {
        u8 b = p[i++];

        if (b & 0x80) {                             /* a timestamp byte */
            if (i < n && (p[i] & 0x80)) {           /* ... followed by a status byte */
                u8 st = p[i++];

                if (st >= 0xF8) {                   /* real time: one byte, status unchanged */
                    blemidi_rx(&st, 1);
                    continue;
                }
                status = (st == 0xF0 || st == 0xF7) ? 0 : st;   /* SysEx: skip its data */
                have = 0;
                if (status && midi_len(status) == 1) {
                    blemidi_rx(&status, 1);
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
        if (have == midi_len(status)) {
            blemidi_rx(msg, have);
            have = 0;
        }
    }
}

/* ------------------------------------------------------------------ outgoing */

/* One MIDI message (1..3 bytes) as one BLE-MIDI packet; false when nobody listens. */
int blemidi_send(const u8 *msg, u32 len)
{
    u8 pkt[2 + 3];
    u32 ts = jiffies_msec() & 0x1FFF;

    if (!con_handle || !notify_on || len == 0 || len > 3) {
        return 0;
    }
    pkt[0] = 0x80 | (ts >> 7);
    pkt[1] = 0x80 | (ts & 0x7F);
    memcpy(pkt + 2, msg, len);
    if (!smcpad_ble_notify(H_MIDI_VALUE, pkt, len + 2)) {
        return 0;
    }
    tx_messages++;
    return 1;
}

int blemidi_connected(void)
{
    return con_handle && notify_on;
}

void blemidi_status(void)
{
    smcpad_dbg_kv("ble-midi connection handle", con_handle);
    smcpad_dbg_kv("ble-midi notify enabled", notify_on);
    smcpad_dbg_kv("ble-midi messages in", rx_messages);
    smcpad_dbg_kv("ble-midi messages out", tx_messages);
}

/* ------------------------------------------------------------------ GATT (smcpad_ble.c) */

static uint16_t blemidi_att_read(uint16_t att_handle, uint16_t offset, uint8_t *buffer,
                                 uint16_t buffer_size)
{
    const char *name = BLEMIDI_NAME;
    u16 len = strlen(name);

    if (att_handle != H_GAP_NAME_VALUE || offset >= len) {
        return 0;                                               /* MIDI reads are empty */
    }
    if (!buffer) {
        return len;
    }
    if (buffer_size > len - offset) {
        buffer_size = len - offset;
    }
    memcpy(buffer, name + offset, buffer_size);
    return buffer_size;
}

static void blemidi_att_write(uint16_t att_handle, uint16_t offset, const uint8_t *buffer,
                              uint16_t buffer_size)
{
    if (att_handle == H_MIDI_VALUE) {
        blemidi_parse(buffer, buffer_size);
    }
}

static void blemidi_event(int event, uint16_t handle, uint16_t value)
{
    switch (event) {
    case SMCPAD_BLE_CONNECTED:
        con_handle = handle;
        notify_on = 0;
        smcpad_dbg_hex16("ble-midi connected, handle", con_handle);
        break;
    case SMCPAD_BLE_DISCONNECTED:
        con_handle = 0;
        notify_on = 0;
        smcpad_dbg_puts("ble-midi disconnected\r\n");
        break;
    case SMCPAD_BLE_SUBSCRIBE:
        notify_on = value & 1;
        smcpad_dbg_kv("ble-midi notify", notify_on);
        break;
    }
}

static const smcpad_ble_callbacks_t blemidi_callbacks = {
    .read = blemidi_att_read,
    .write = blemidi_att_write,
    .event = blemidi_event,
};

/* Bring the stack up (console `Y`); advertising starts once it is ready (smcpad_ble.c). */
void blemidi_start(void)
{
    static const u8 midi_uuid_le[16] = {
        0x00, 0xc7, 0xc4, 0x4e, 0xe3, 0x6c, 0x51, 0xa7, 0x33, 0x4b, 0xe8, 0xed, 0x5a, 0x0e, 0xb8, 0x03,
    };
    static u8 started;
    u8 adv[3 + 18], rsp[2 + sizeof(BLEMIDI_NAME) - 1];

    if (started) {
        blemidi_status();
        return;
    }
    started = 1;
    adv[0] = 2;                                 /* flags: general discoverable, no BR/EDR */
    adv[1] = 0x01;
    adv[2] = 0x06;
    adv[3] = 17;                                /* complete list of 128-bit service UUIDs */
    adv[4] = 0x07;
    memcpy(adv + 5, midi_uuid_le, 16);
    rsp[0] = sizeof(rsp) - 1;                   /* complete local name */
    rsp[1] = 0x09;
    memcpy(rsp + 2, BLEMIDI_NAME, sizeof(rsp) - 2);
    smcpad_ble_set_name(BLEMIDI_NAME);
    smcpad_ble_set_profile(blemidi_profile, sizeof(blemidi_profile));
    smcpad_ble_set_adv(adv, sizeof(adv), rsp, sizeof(rsp), 48, 1);
    smcpad_ble_begin(&blemidi_callbacks);
    smcpad_ble_advertise(1);
    smcpad_dbg_puts("ble stack starting, advertising as " BLEMIDI_NAME "\r\n");
}

#else

void blemidi_start(void)
{
    smcpad_dbg_puts("built without BLE (TCFG_USER_BLE_ENABLE)\r\n");
}

int blemidi_send(const u8 *msg, u32 len)
{
    return 0;
}

#endif
