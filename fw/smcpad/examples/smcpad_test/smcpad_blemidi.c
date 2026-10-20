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
 * Replaces modules/bt/ble_hogp.c (HID over GATT) in the build: it provides the hooks the SDK's
 * BLE startup calls (bt_ble_before_start_init, bt_ble_init, bt_ble_exit, ble_module_enable)
 * and stubs for the HID setters modules/bt/app_comm_ble.c still calls.
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

#if TCFG_USER_BLE_ENABLE

extern void smcpad_dbg_puts(const char *s);
extern void smcpad_dbg_kv(const char *tag, u32 value);
extern void smcpad_dbg_hex16(const char *tag, u32 value);

#define BLEMIDI_NAME            "SMC-PAD"

/* BLE-MIDI packets are small; as ble_hogp.c sizes it */
#define ATT_LOCAL_MTU_SIZE      (64)
#define ATT_PACKET_NUMS_MAX     (2)
#define ATT_SEND_CBUF_SIZE      (ATT_PACKET_NUMS_MAX * (ATT_PACKET_HEAD_SIZE + ATT_LOCAL_MTU_SIZE))

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
    if (ble_comm_att_send_data(con_handle, H_MIDI_VALUE, pkt, len + 2, ATT_OP_AUTO_READ_CCC)) {
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

/* ------------------------------------------------------------------ GATT callbacks */

static uint16_t blemidi_att_read(hci_con_handle_t handle, uint16_t att_handle, uint16_t offset,
                                 uint8_t *buffer, uint16_t buffer_size)
{
    switch (att_handle) {
    case H_GAP_NAME_VALUE: {
        const char *name = BLEMIDI_NAME;
        u16 len = strlen(name);

        if (offset >= len) {
            return 0;
        }
        if (buffer) {
            if (buffer_size > len - offset) {
                buffer_size = len - offset;
            }
            memcpy(buffer, name + offset, buffer_size);
            return buffer_size;
        }
        return len;
    }
    case H_MIDI_VALUE:
        return 0;                                               /* the spec: reads are empty */
    case H_MIDI_CCC:
        if (buffer) {
            buffer[0] = ble_gatt_server_characteristic_ccc_get(handle, att_handle);
            buffer[1] = 0;
        }
        return 2;
    default:
        return 0;
    }
}

static int blemidi_att_write(hci_con_handle_t handle, uint16_t att_handle, uint16_t mode,
                             uint16_t offset, uint8_t *buffer, uint16_t buffer_size)
{
    switch (att_handle) {
    case H_MIDI_VALUE:
        blemidi_parse(buffer, buffer_size);
        break;
    case H_MIDI_CCC:
        ble_gatt_server_characteristic_ccc_set(handle, att_handle, buffer[0]);
        notify_on = buffer[0] & 1;
        smcpad_dbg_kv("ble-midi notify", notify_on);
        break;
    default:
        break;
    }
    return 0;
}

static int blemidi_event(int event, u8 *packet, u16 size, u8 *ext_param)
{
    switch (event) {
    case GATT_COMM_EVENT_CONNECTION_COMPLETE:
        con_handle = little_endian_read_16(packet, 0);
        notify_on = 0;
        smcpad_dbg_hex16("ble-midi connected, handle", con_handle);
        break;
    case GATT_COMM_EVENT_DISCONNECT_COMPLETE:
        if (con_handle == little_endian_read_16(packet, 0)) {
            con_handle = 0;
            notify_on = 0;
            smcpad_dbg_puts("ble-midi disconnected\r\n");
        }
        break;
    case GATT_COMM_EVENT_ENCRYPTION_CHANGE:
        smcpad_dbg_puts("ble-midi link encrypted\r\n");
        break;
    case GATT_COMM_EVENT_MTU_EXCHANGE_COMPLETE:
        smcpad_dbg_kv("ble-midi ATT MTU", little_endian_read_16(packet, 2));
        break;
    default:
        break;
    }
    return 0;
}

/* ------------------------------------------------------------------ stack configuration */

static const sm_cfg_t blemidi_sm_config = {
    .slave_security_auto_req = 0,
    .slave_set_wait_security = 0,
    .io_capabilities = IO_CAPABILITY_NO_INPUT_NO_OUTPUT,       /* "just works" pairing */
    .authentication_req_flags = SM_AUTHREQ_BONDING,
    .min_key_size = 7,
    .max_key_size = 16,
    .sm_cb_packet_handler = NULL,
};

static const gatt_server_cfg_t blemidi_server_config = {
    .att_read_cb = &blemidi_att_read,
    .att_write_cb = &blemidi_att_write,
    .event_packet_handler = &blemidi_event,
};

static gatt_ctrl_t blemidi_gatt_control = {
    .mtu_size = ATT_LOCAL_MTU_SIZE,
    .cbuffer_size = ATT_SEND_CBUF_SIZE,
    .multi_dev_flag = 0,
    .server_config = &blemidi_server_config,
    .client_config = NULL,
    .sm_config = &blemidi_sm_config,
    .hci_cb_packet_handler = NULL,
};

static adv_cfg_t blemidi_adv;
static u8 adv_data[ADV_RSP_PACKET_MAX], rsp_data[ADV_RSP_PACKET_MAX];

static void blemidi_adv_setup(void)
{
    static const u8 midi_uuid_le[16] = {
        0x00, 0xc7, 0xc4, 0x4e, 0xe3, 0x6c, 0x51, 0xa7, 0x33, 0x4b, 0xe8, 0xed, 0x5a, 0x0e, 0xb8, 0x03,
    };
    const char *name = BLEMIDI_NAME;
    u8 off = 0;

    off += make_eir_packet_val(&adv_data[off], off, HCI_EIR_DATATYPE_FLAGS,
                               FLAGS_GENERAL_DISCOVERABLE_MODE | FLAGS_EDR_NOT_SUPPORTED, 1);
    off += make_eir_packet_data(&adv_data[off], off, HCI_EIR_DATATYPE_COMPLETE_128BIT_SERVICE_UUIDS,
                                (void *)midi_uuid_le, 16);
    blemidi_adv.adv_data = adv_data;
    blemidi_adv.adv_data_len = off;

    off = 0;
    off += make_eir_packet_data(&rsp_data[off], off, HCI_EIR_DATATYPE_COMPLETE_LOCAL_NAME,
                                (void *)name, strlen(name));
    blemidi_adv.rsp_data = rsp_data;
    blemidi_adv.rsp_data_len = off;

    blemidi_adv.adv_interval = 48;                              /* 30 ms, in 0.625 ms units */
    blemidi_adv.adv_auto_do = 1;
    blemidi_adv.adv_type = ADV_IND;
    blemidi_adv.adv_channel = ADV_CHANNEL_ALL;
    memset(blemidi_adv.direct_address_info, 0, 7);
    ble_gatt_server_set_adv_config(&blemidi_adv);
}

/* Called by the stack (modules/bt/app_comm_ble.c) before / after its init. */
void bt_ble_before_start_init(void)
{
    ble_comm_init(&blemidi_gatt_control);
}

void bt_ble_init(void)
{
    ble_comm_set_config_name(BLEMIDI_NAME, 0);
    con_handle = 0;
    ble_gatt_server_set_profile(blemidi_profile, sizeof(blemidi_profile));
    blemidi_adv_setup();
    ble_module_enable(1);
    smcpad_dbg_puts("ble-midi advertising as " BLEMIDI_NAME "\r\n");
}

void bt_ble_exit(void)
{
    ble_module_enable(0);
    ble_comm_exit();
}

void ble_module_enable(u8 en)
{
    ble_comm_module_enable(en);
}

/* What modules/bt/app_comm_ble.c sets up for HID over GATT: nothing to do for MIDI. */
void le_hogp_set_icon(u16 class_type)
{
}

void le_hogp_set_ReportMap(u8 *map, u16 size)
{
}

void le_hogp_regiest_get_battery(u8 (*get_battery_cbk)(void))
{
}

/* Bring the stack up (console `Y`). The system clock goes to BT_NORMAL_HZ, as the SDK's BLE
 * applications do; BT_STATUS_INIT_OK then reaches app_idle.c's event handler, which calls
 * btstack_ble_start_after_init() -> bt_ble_init() above. */
void blemidi_start(void)
{
    static u8 started;
    static const ble_init_cfg_t cfg = { .same_address = 0, .appearance = 0 };
    extern void bt_pll_para(u32 osc, u32 sys, u8 low_power, u8 xosc);

    if (started) {
        blemidi_status();
        return;
    }
    started = 1;
    clk_set("sys", BT_NORMAL_HZ);
    bt_pll_para(TCFG_CLOCK_OSC_HZ, clk_get("sys"), 0, 0);
    btstack_ble_start_before_init(&cfg, 0);
    btstack_init();
    smcpad_dbg_puts("ble stack starting\r\n");
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
