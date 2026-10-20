/*
 * BLE peripheral for the runtime (smcpad_rt.h, "BLE"): a GATT server on the SDK's BLE stack
 * (BTstack + the gatt_common module, closed controller in btctrler.a). The ATT database comes
 * from the caller (the ArduinoBLE library, or the console's BLE-MIDI), in BTstack's compiled
 * format; reads and writes of its dynamic attributes go to the caller's callbacks, the CCCs
 * (UUID 0x2902) are kept here.
 *
 * Start: smcpad_ble_begin() -> clk_set("sys", BT_NORMAL_HZ) (24 MHz, already the boot clock),
 * bt_pll_para, btstack_ble_start_before_init, btstack_init; BT_STATUS_INIT_OK then reaches
 * app_idle.c's event handler -> btstack_ble_start_after_init -> bt_ble_init() below, which
 * applies the profile and starts advertising if it was asked for meanwhile.
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
#include "smcpad_rt.h"

#if TCFG_USER_BLE_ENABLE

extern int le_controller_get_mac(void *addr);
extern s8 ble_vendor_get_peer_rssi(u16 conn_handle);

#define ATT_LOCAL_MTU_SIZE      (247)
#define ATT_PACKET_NUMS_MAX     (2)
#define ATT_SEND_CBUF_SIZE      (ATT_PACKET_NUMS_MAX * (ATT_PACKET_HEAD_SIZE + ATT_LOCAL_MTU_SIZE))

/* until the caller gives one: GAP service, no name characteristic */
static const u8 empty_profile[] = {
    0x0a, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x28, 0x00, 0x18,
    0x00, 0x00,
};

static const smcpad_ble_callbacks_t *cb;
static const u8 *profile = empty_profile;
static u16 profile_len = sizeof(empty_profile);
static u32 ccc_map[8];                      /* CCC handles 0..255 of the profile */
static u16 service_changed;                  /* its 0x2A05 value, if it has one */
static char name[32] = "SMC-PAD";
static u8 started, ready, module_on, want_adv;
static volatile u16 con_handle, mtu = 23;
static u8 central_addr[6];

static adv_cfg_t adv;
static u8 adv_data[ADV_RSP_PACKET_MAX], rsp_data[ADV_RSP_PACKET_MAX];

static int is_ccc(u16 h)
{
    return h < 256 && (ccc_map[h >> 5] >> (h & 31) & 1);
}

/* ------------------------------------------------------------------ GATT callbacks
 * (BT stack task) */

static uint16_t ble_att_read(hci_con_handle_t handle, uint16_t att_handle, uint16_t offset,
                             uint8_t *buffer, uint16_t buffer_size)
{
    if (is_ccc(att_handle)) {
        if (buffer) {
            u16 v = ble_gatt_server_characteristic_ccc_get(handle, att_handle);

            buffer[0] = v;
            buffer[1] = v >> 8;
        }
        return 2;
    }
    return cb && cb->read ? cb->read(att_handle, offset, buffer, buffer_size) : 0;
}

static int ble_att_write(hci_con_handle_t handle, uint16_t att_handle, uint16_t mode,
                         uint16_t offset, uint8_t *buffer, uint16_t buffer_size)
{
    if (is_ccc(att_handle)) {
        u16 v = buffer_size ? buffer[0] | (buffer_size > 1 ? buffer[1] << 8 : 0) : 0;

        ble_gatt_server_characteristic_ccc_set(handle, att_handle, v);
        if (cb && cb->event) {
            cb->event(SMCPAD_BLE_SUBSCRIBE, att_handle - 1, v);  /* the value handle */
        }
        return 0;
    }
    if (cb && cb->write) {
        cb->write(att_handle, offset, buffer, buffer_size);
    }
    return 0;
}

static int ble_event(int event, u8 *packet, u16 size, u8 *ext_param)
{
    switch (event) {
    case GATT_COMM_EVENT_CONNECTION_COMPLETE:
        con_handle = little_endian_read_16(packet, 0);
        mtu = 23;
        if (ext_param) {                    /* the HCI LE connection complete event */
            memcpy(central_addr, ext_param + 8, 6);
        }
        if (service_changed) {
            /* A bonded central (macOS) keeps the GATT database it found the first time;
             * whatever the sketch serves now, have it look again: whole range */
            static const u8 range[4] = { 0x01, 0x00, 0xff, 0xff };

            ble_comm_att_send_data(con_handle, service_changed, (u8 *)range, 4, ATT_OP_INDICATE);
        }
        if (cb && cb->event) {
            cb->event(SMCPAD_BLE_CONNECTED, con_handle, 0);
        }
        break;
    case GATT_COMM_EVENT_DISCONNECT_COMPLETE:
        if (con_handle == little_endian_read_16(packet, 0)) {
            u16 h = con_handle;

            con_handle = 0;
            if (cb && cb->event) {
                cb->event(SMCPAD_BLE_DISCONNECTED, h, 0);
            }
        }
        break;
    case GATT_COMM_EVENT_MTU_EXCHANGE_COMPLETE:
        mtu = little_endian_read_16(packet, 2);
        break;
    default:
        break;
    }
    return 0;
}

/* ------------------------------------------------------------------ stack configuration */

static const sm_cfg_t ble_sm_config = {
    .slave_security_auto_req = 0,
    .slave_set_wait_security = 0,
    .io_capabilities = IO_CAPABILITY_NO_INPUT_NO_OUTPUT,       /* "just works" pairing */
    .authentication_req_flags = SM_AUTHREQ_BONDING,
    .min_key_size = 7,
    .max_key_size = 16,
    .sm_cb_packet_handler = NULL,
};

static const gatt_server_cfg_t ble_server_config = {
    .att_read_cb = &ble_att_read,
    .att_write_cb = &ble_att_write,
    .event_packet_handler = &ble_event,
};

static gatt_ctrl_t ble_gatt_control = {
    .mtu_size = ATT_LOCAL_MTU_SIZE,
    .cbuffer_size = ATT_SEND_CBUF_SIZE,
    .multi_dev_flag = 0,
    .server_config = &ble_server_config,
    .client_config = NULL,
    .sm_config = &ble_sm_config,
    .hci_cb_packet_handler = NULL,
};

/* The address: the chip's, with the low three bytes mixed with a hash of the GATT database.
 * Centrals (macOS) keep the database they found under an address, and one without Service
 * Changed is never looked at again: so each database gets an address of its own, and sketches
 * that serve the same one (all BLE-MIDI sketches) keep theirs, with the pairing. */
static void set_address(void)
{
    static u8 base[6], have_base;
    u32 h = 2166136261u;                    /* FNV-1a */

    if (!have_base) {
        le_controller_get_mac(base);
        have_base = 1;
    }
    for (u32 i = 0; i < profile_len; i++) {
        h = (h ^ profile[i]) * 16777619u;
    }
    memcpy(adv.local_address_info + 1, base, 6);
    adv.local_address_info[1] ^= h;
    adv.local_address_info[2] ^= h >> 8;
    adv.local_address_info[3] ^= h >> 16;
    adv.set_local_addr_tag = USE_SET_LOCAL_ADDRESS_TAG;
}

static void apply(void)
{
    set_address();
    ble_comm_set_config_name(name, 0);
    ble_gatt_server_set_profile(profile, profile_len);
    ble_gatt_server_set_adv_config(&adv);
}

static void adv_enable(int on)
{
    adv.adv_auto_do = on;                   /* also: advertise again after a disconnection */
    if (!module_on) {
        if (on) {
            apply();
            ble_comm_module_enable(1);      /* starts advertising (adv_auto_do) */
            module_on = 1;
        }
        return;
    }
    ble_gatt_server_set_adv_config(&adv);
    if (!con_handle) {
        ble_gatt_server_adv_enable(on);
    }
}

/* Called by the stack (modules/bt/app_comm_ble.c) before / after its init. */
void bt_ble_before_start_init(void)
{
    ble_comm_init(&ble_gatt_control);
}

void bt_ble_init(void)
{
    con_handle = 0;
    apply();
    ready = 1;
    if (want_adv) {
        adv_enable(1);
    }
}

void bt_ble_exit(void)
{
    ble_module_enable(0);
    ble_comm_exit();
}

void ble_module_enable(u8 en)
{
    ble_comm_module_enable(en);
    module_on = en;
}

/* What modules/bt/app_comm_ble.c sets up for HID over GATT: nothing to do here. */
void le_hogp_set_icon(u16 class_type)
{
}

void le_hogp_set_ReportMap(u8 *map, u16 size)
{
}

void le_hogp_regiest_get_battery(u8 (*get_battery_cbk)(void))
{
}

/* ------------------------------------------------------------------ API */

void smcpad_ble_begin(const smcpad_ble_callbacks_t *callbacks)
{
    static const ble_init_cfg_t cfg = { .same_address = 0, .appearance = 0 };
    extern void bt_pll_para(u32 osc, u32 sys, u8 low_power, u8 xosc);

    cb = callbacks;
    if (started) {
        return;
    }
    started = 1;
    if (!adv.adv_data) {
        smcpad_ble_set_adv(NULL, 0, NULL, 0, 0, 1);
    }
    clk_set("sys", BT_NORMAL_HZ);
    bt_pll_para(TCFG_CLOCK_OSC_HZ, clk_get("sys"), 0, 0);
    btstack_ble_start_before_init(&cfg, 0);
    btstack_init();
}

int smcpad_ble_ready(void)
{
    return ready;
}

void smcpad_ble_set_name(const char *n)
{
    strncpy(name, n, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    if (ready) {
        ble_comm_set_config_name(name, 0);
    }
}

/* Applied on the next smcpad_ble_advertise(1) (or at stack start); db must stay valid. */
void smcpad_ble_set_profile(const uint8_t *db, uint16_t len)
{
    memset(ccc_map, 0, sizeof(ccc_map));
    service_changed = 0;
    for (u32 i = 0; i + 8 <= len; ) {
        u16 size = little_endian_read_16(db, i), flags = little_endian_read_16(db, i + 2);
        u16 h = little_endian_read_16(db, i + 4), type = little_endian_read_16(db, i + 6);

        if (size < 8) {
            break;
        }
        if (!(flags & 0x200) && type == 0x2902 && h < 256) {
            ccc_map[h >> 5] |= 1u << (h & 31);
        }
        if (!(flags & 0x200) && type == 0x2a05) {
            service_changed = h;
        }
        i += size;
    }
    profile = db;
    profile_len = len;
    if (module_on && !con_handle) {
        set_address();
        ble_gatt_server_set_profile(profile, profile_len);
    }
}

/* adv / rsp: raw AD structures (len, type, data...); interval in 0.625 ms units (0: 30 ms). */
void smcpad_ble_set_adv(const uint8_t *a, uint8_t alen, const uint8_t *r, uint8_t rlen,
                        uint16_t interval, int connectable)
{
    alen = alen > sizeof(adv_data) ? sizeof(adv_data) : alen;
    rlen = rlen > sizeof(rsp_data) ? sizeof(rsp_data) : rlen;
    if (a) {
        memcpy(adv_data, a, alen);
    }
    if (r) {
        memcpy(rsp_data, r, rlen);
    }
    adv.adv_data = adv_data;
    adv.adv_data_len = alen;
    adv.rsp_data = rsp_data;
    adv.rsp_data_len = rlen;
    adv.adv_interval = interval ? interval : 48;
    adv.adv_type = connectable ? ADV_IND : ADV_SCAN_IND;
    adv.adv_channel = ADV_CHANNEL_ALL;
    memset(adv.direct_address_info, 0, 7);
    if (ready) {
        set_address();
    }
    if (module_on) {
        ble_gatt_server_set_adv_config(&adv);
    }
}

void smcpad_ble_advertise(int on)
{
    want_adv = on;
    if (ready) {
        adv_enable(on);
    }
}

uint16_t smcpad_ble_connection(void)
{
    return con_handle;
}

uint16_t smcpad_ble_mtu(void)
{
    return mtu;
}

void smcpad_ble_address(uint8_t addr[6])
{
    if (adv.set_local_addr_tag == USE_SET_LOCAL_ADDRESS_TAG) {
        memcpy(addr, adv.local_address_info + 1, 6);
    } else {
        le_controller_get_mac(addr);
    }
}

void smcpad_ble_central_address(uint8_t addr[6])
{
    memcpy(addr, central_addr, 6);
}

int smcpad_ble_rssi(void)
{
    return con_handle ? ble_vendor_get_peer_rssi(con_handle) : 127;
}

/* Notify or indicate, as the central's CCC asks; waits up to 50 ms for room in the queue.
 * 1 when sent. */
int smcpad_ble_notify(uint16_t handle, const uint8_t *data, uint16_t len)
{
    u32 start = smcpad_millis();

    if (!con_handle || len > mtu - 3) {
        return 0;
    }
    while (!ble_comm_att_check_send(con_handle, len)) {
        if (!con_handle || smcpad_millis() - start > 50) {
            return 0;
        }
        smcpad_yield();
    }
    return ble_comm_att_send_data(con_handle, handle, (u8 *)data, len, ATT_OP_AUTO_READ_CCC) == 0;
}

/* 1 when the central has notifications (bit 0) or indications (bit 1) on for this value. */
int smcpad_ble_subscribed(uint16_t handle)
{
    return con_handle && is_ccc(handle + 1) ? ble_gatt_server_characteristic_ccc_get(con_handle, handle + 1) : 0;
}

void smcpad_ble_disconnect(void)
{
    if (con_handle) {
        ble_comm_disconnect(con_handle);
    }
}

#else

void smcpad_ble_begin(const smcpad_ble_callbacks_t *callbacks)
{
}

int smcpad_ble_ready(void)
{
    return 0;
}

#endif
