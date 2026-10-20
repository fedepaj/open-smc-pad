/*
 * ArduinoBLE for the SMC-PAD. The services the sketch adds become an ATT database in BTstack's
 * compiled format (smcpad_ble_set_profile); the runtime calls back here from the BT stack's
 * task for reads and writes, and with connection / subscription events. Those callbacks only
 * copy data and bump counters; poll(), in the sketch's task, turns the counters into the
 * sketch's event handlers.
 */
#include "ArduinoBLE.h"

#define GAP_NAME_HANDLE     3

struct BLEDescriptorImpl {
    BLEUuidValue uuid;
    uint8_t *value;
    uint16_t len;
    BLEDescriptorImpl *next;
};

struct BLECharacteristicImpl {
    BLEUuidValue uuid;
    uint16_t props;
    uint16_t size;
    bool fixed;
    uint8_t *value;
    volatile uint16_t len;
    uint16_t handle;                        /* the value's; its CCC, if any, is handle + 1 */
    volatile uint16_t ccc;
    volatile uint8_t writes, subs, reads;   /* bumped by the BT stack's task */
    uint8_t seen_writes, seen_subs, seen_reads, seen_written;
    uint16_t last_ccc;
    BLECharacteristicEventHandler handlers[BLECharacteristicEventLast];
    BLECharacteristic::RawWriteCallback raw_write;
    void *raw_arg;
    BLEDescriptorImpl *descriptors;
    BLECharacteristicImpl *next;            /* in its service */
};

struct BLEServiceImpl {
    BLEUuidValue uuid;
    BLECharacteristicImpl *characteristics;
    BLEServiceImpl *next;
};

BLELocalDevice BLE;

static BLEServiceImpl *services;
static char device_name[32] = "SMC-PAD";
static char local_name[30] = "SMC-PAD";
static uint16_t appearance;
static BLEUuidValue adv_uuid;
static uint8_t manufacturer[27], manufacturer_len;
static uint8_t service_data[27], service_data_len;
static uint16_t adv_interval = 48;          /* 30 ms */
static bool connectable = true;
static uint8_t *db;
static uint16_t db_len;
static bool db_dirty = true;

static volatile uint16_t conn_handle;
static volatile uint8_t connects, disconnects;
static uint8_t seen_connects, seen_disconnects;
static uint8_t central_addr[6];
static uint16_t last_handle;
static uint8_t last_addr[6];
static BLEDeviceEventHandler device_handlers[BLEDeviceLastEvent];

/* ------------------------------------------------------------------ UUIDs */

static int hex(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* "180F", "0000180F", or "19B10000-E8F2-537E-4F6C-D104768A1214"; stored little-endian */
static void parse_uuid(const char *s, BLEUuidValue &u)
{
    static const uint8_t base[16] = {   /* 00000000-0000-1000-8000-00805F9B34FB */
        0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0, 0, 0, 0,
    };
    uint8_t digits[32];
    int n = 0;

    strncpy(u.str, s ? s : "", sizeof(u.str) - 1);
    u.str[sizeof(u.str) - 1] = 0;
    for (const char *p = u.str; *p && n < 32; p++) {
        if (hex(*p) >= 0) {
            digits[n++] = hex(*p);
        }
    }
    memset(u.data, 0, sizeof(u.data));
    if (n <= 4) {
        uint16_t v = 0;

        for (int i = 0; i < n; i++) {
            v = v << 4 | digits[i];
        }
        u.len = 2;
        u.data[0] = v;
        u.data[1] = v >> 8;
        return;
    }
    if (n <= 8) {
        uint32_t v = 0;

        for (int i = 0; i < n; i++) {
            v = v << 4 | digits[i];
        }
        memcpy(u.data, base, 16);
        u.data[12] = v;
        u.data[13] = v >> 8;
        u.data[14] = v >> 16;
        u.data[15] = v >> 24;
        u.len = 16;
        return;
    }
    for (int i = 0; i < 16; i++) {
        u.data[15 - i] = digits[2 * i] << 4 | digits[2 * i + 1];
    }
    u.len = 16;
}

static bool uuid_equal(const BLEUuidValue &u, const char *s)
{
    BLEUuidValue v;

    parse_uuid(s, v);
    return u.len == v.len && !memcmp(u.data, v.data, u.len);
}

/* ------------------------------------------------------------------ the ATT database */

struct DbWriter {
    uint8_t *p;
    int n;

    void u8(uint8_t v) { if (p) p[n] = v; n++; }
    void u16(uint16_t v) { u8(v); u8(v >> 8); }
    void bytes(const uint8_t *b, int len) { for (int i = 0; i < len; i++) u8(b[i]); }

    /* size, flags, handle, type (2 or 16 bytes), value */
    void entry(uint16_t flags, uint16_t handle, const uint8_t *type, int type_len, const uint8_t *value, int value_len)
    {
        u16(6 + type_len + value_len);
        u16(flags | (type_len == 16 ? 0x200 : 0));
        u16(handle);
        bytes(type, type_len);
        bytes(value, value_len);
    }
    void entry16(uint16_t flags, uint16_t handle, uint16_t type, const uint8_t *value, int value_len)
    {
        uint8_t t[2] = { (uint8_t)type, (uint8_t)(type >> 8) };

        entry(flags, handle, t, 2, value, value_len);
    }
};

enum { F_READ = 0x02, F_WRITE = 0x08, F_DYNAMIC = 0x100 };

static void build_db(DbWriter &w)
{
    uint16_t h = 1;
    uint8_t v[19];

    /* GAP: device name (served by the read callback), appearance */
    v[0] = 0x00; v[1] = 0x18;
    w.entry16(F_READ, h++, 0x2800, v, 2);
    v[0] = F_READ; v[1] = GAP_NAME_HANDLE; v[2] = 0; v[3] = 0x00; v[4] = 0x2a;
    w.entry16(F_READ, h++, 0x2803, v, 5);
    w.entry16(F_READ | F_DYNAMIC, h++, 0x2a00, nullptr, 0);
    v[0] = F_READ; v[1] = h + 1; v[2] = 0; v[3] = 0x01; v[4] = 0x2a;
    w.entry16(F_READ, h++, 0x2803, v, 5);
    v[0] = appearance; v[1] = appearance >> 8;
    w.entry16(F_READ, h++, 0x2a01, v, 2);

    /* GATT: service changed, indicated by the runtime on each connection (smcpad_ble.c) */
    v[0] = 0x01; v[1] = 0x18;
    w.entry16(F_READ, h++, 0x2800, v, 2);
    v[0] = BLEIndicate; v[1] = h + 1; v[2] = 0; v[3] = 0x05; v[4] = 0x2a;
    w.entry16(F_READ, h++, 0x2803, v, 5);
    w.entry16(BLEIndicate | F_DYNAMIC, h++, 0x2a05, nullptr, 0);
    v[0] = v[1] = 0;
    w.entry16(F_READ | F_WRITE | F_DYNAMIC, h++, 0x2902, v, 2);

    for (BLEServiceImpl *s = services; s; s = s->next) {
        w.entry16(F_READ, h++, 0x2800, s->uuid.data, s->uuid.len);
        for (BLECharacteristicImpl *c = s->characteristics; c; c = c->next) {
            uint8_t props = c->props & 0xFF;

            v[0] = props;
            v[1] = h + 1;
            v[2] = (h + 1) >> 8;
            memcpy(v + 3, c->uuid.data, c->uuid.len);
            w.entry16(F_READ, h++, 0x2803, v, 3 + c->uuid.len);
            c->handle = h;
            w.entry(props | F_DYNAMIC, h++, c->uuid.data, c->uuid.len, nullptr, 0);
            if (props & (BLENotify | BLEIndicate)) {
                v[0] = v[1] = 0;
                w.entry16(F_READ | F_WRITE | F_DYNAMIC, h++, 0x2902, v, 2);
            }
            for (BLEDescriptorImpl *d = c->descriptors; d; d = d->next) {
                w.entry(F_READ, h++, d->uuid.data, d->uuid.len, d->value, d->len);
            }
        }
    }
    w.u16(0);
}

static void rebuild_db()
{
    DbWriter w = { nullptr, 0 };

    build_db(w);
    free(db);
    db = (uint8_t *)malloc(w.n);
    db_len = w.n;
    w.p = db;
    w.n = 0;
    build_db(w);
    db_dirty = false;
}

static BLECharacteristicImpl *find_characteristic(uint16_t handle)
{
    for (BLEServiceImpl *s = services; s; s = s->next) {
        for (BLECharacteristicImpl *c = s->characteristics; c; c = c->next) {
            if (c->handle == handle) {
                return c;
            }
        }
    }
    return nullptr;
}

/* ------------------------------------------------------------------ runtime callbacks
 * (BT stack task) */

static uint16_t serve(const uint8_t *value, uint16_t len, uint16_t offset, uint8_t *buf, uint16_t size)
{
    if (!buf) {
        return len;
    }
    if (offset >= len) {
        return 0;
    }
    if (size > len - offset) {
        size = len - offset;
    }
    memcpy(buf, value + offset, size);
    return size;
}

static uint16_t on_read(uint16_t handle, uint16_t offset, uint8_t *buf, uint16_t size)
{
    if (handle == GAP_NAME_HANDLE) {
        return serve((const uint8_t *)device_name, strlen(device_name), offset, buf, size);
    }
    BLECharacteristicImpl *c = find_characteristic(handle);

    if (!c) {
        return 0;
    }
    if (buf && offset == 0) {
        c->reads++;
    }
    if (c->raw_write) {
        return 0;                           /* a stream (BLE-MIDI): reads are empty */
    }
    return serve(c->value, c->len, offset, buf, size);
}

static void on_write(uint16_t handle, uint16_t offset, const uint8_t *data, uint16_t len)
{
    BLECharacteristicImpl *c = find_characteristic(handle);

    if (!c) {
        return;
    }
    if (c->raw_write) {
        if (offset == 0) {
            c->raw_write(data, len, c->raw_arg);
        }
        c->writes++;
        return;
    }
    if (offset >= c->size) {
        return;
    }
    if (len > c->size - offset) {
        len = c->size - offset;
    }
    memcpy(c->value + offset, data, len);
    c->len = c->fixed ? c->size : offset + len;
    c->writes++;
}

static void on_event(int event, uint16_t handle, uint16_t value)
{
    switch (event) {
    case SMCPAD_BLE_CONNECTED:
        smcpad_ble_central_address(central_addr);
        conn_handle = handle;
        connects++;
        break;
    case SMCPAD_BLE_DISCONNECTED:
        conn_handle = 0;
        for (BLEServiceImpl *s = services; s; s = s->next) {
            for (BLECharacteristicImpl *c = s->characteristics; c; c = c->next) {
                if (c->ccc) {
                    c->ccc = 0;
                    c->subs++;
                }
            }
        }
        disconnects++;
        break;
    case SMCPAD_BLE_SUBSCRIBE: {
        BLECharacteristicImpl *c = find_characteristic(handle);

        if (c) {
            c->ccc = value;
            c->subs++;
        }
        break;
    }
    }
}

static const smcpad_ble_callbacks_t callbacks = { on_read, on_write, on_event };

/* ------------------------------------------------------------------ BLELocalDevice */

int BLELocalDevice::begin()
{
    smcpad_ble_begin(&callbacks);
    for (unsigned long start = millis(); !smcpad_ble_ready() && millis() - start < 5000; ) {
        delay(5);
    }
    return smcpad_ble_ready();
}

void BLELocalDevice::end()
{
    smcpad_ble_advertise(0);
    smcpad_ble_disconnect();
}

static bool pending()
{
    if (connects != seen_connects || disconnects != seen_disconnects) {
        return true;
    }
    for (BLEServiceImpl *s = services; s; s = s->next) {
        for (BLECharacteristicImpl *c = s->characteristics; c; c = c->next) {
            if (c->writes != c->seen_writes || c->subs != c->seen_subs || c->reads != c->seen_reads) {
                return true;
            }
        }
    }
    return false;
}

void BLELocalDevice::poll(unsigned long timeout)
{
    for (unsigned long start = millis(); timeout && !pending() && millis() - start < timeout; ) {
        yield();
    }
    if (connects != seen_connects) {
        seen_connects = connects;
        last_handle = conn_handle;
        memcpy(last_addr, central_addr, 6);
        if (device_handlers[BLEConnected]) {
            device_handlers[BLEConnected](BLEDevice(last_handle, last_addr));
        }
    }
    for (BLEServiceImpl *s = services; s; s = s->next) {
        for (BLECharacteristicImpl *c = s->characteristics; c; c = c->next) {
            BLECharacteristic ch;

            ch._c = c;
            if (c->subs != c->seen_subs) {
                uint16_t ccc = c->ccc;

                c->seen_subs = c->subs;
                if ((ccc != 0) != (c->last_ccc != 0)) {
                    int ev = ccc ? BLESubscribed : BLEUnsubscribed;

                    c->last_ccc = ccc;
                    if (c->handlers[ev]) {
                        c->handlers[ev](BLEDevice(last_handle, last_addr), ch);
                    }
                }
            }
            if (c->reads != c->seen_reads) {
                c->seen_reads = c->reads;
                if (c->handlers[BLERead]) {
                    c->handlers[BLERead](BLEDevice(last_handle, last_addr), ch);
                }
            }
            if (c->writes != c->seen_writes) {
                c->seen_writes = c->writes;
                if (c->handlers[BLEWritten]) {
                    c->handlers[BLEWritten](BLEDevice(last_handle, last_addr), ch);
                }
            }
        }
    }
    if (disconnects != seen_disconnects) {
        seen_disconnects = disconnects;
        if (device_handlers[BLEDisconnected]) {
            device_handlers[BLEDisconnected](BLEDevice(last_handle, last_addr));
        }
    }
}

bool BLELocalDevice::connected() const
{
    BLE.poll();
    return conn_handle != 0;
}

bool BLELocalDevice::disconnect()
{
    if (!conn_handle) {
        return false;
    }
    smcpad_ble_disconnect();
    return true;
}

static String format_address(const uint8_t a[6])
{
    char s[18];
    static const char digits[] = "0123456789abcdef";

    for (int i = 0; i < 6; i++) {
        s[3 * i] = digits[a[5 - i] >> 4];
        s[3 * i + 1] = digits[a[5 - i] & 15];
        s[3 * i + 2] = i < 5 ? ':' : 0;
    }
    return String(s);
}

String BLELocalDevice::address() const
{
    uint8_t a[6];

    smcpad_ble_address(a);
    return format_address(a);
}

int BLELocalDevice::rssi()
{
    return smcpad_ble_rssi();
}

bool BLELocalDevice::setAdvertisedServiceUuid(const char *uuid)
{
    parse_uuid(uuid, adv_uuid);
    return true;
}

bool BLELocalDevice::setAdvertisedService(const BLEService &service)
{
    if (!service._s) {
        return false;
    }
    adv_uuid = service._s->uuid;
    return true;
}

bool BLELocalDevice::setManufacturerData(const uint8_t data[], int len)
{
    if (len < 0 || len > (int)sizeof(manufacturer)) {
        return false;
    }
    memcpy(manufacturer, data, len);
    manufacturer_len = len;
    return true;
}

bool BLELocalDevice::setManufacturerData(const uint16_t companyId, const uint8_t data[], int len)
{
    if (len < 0 || len + 2 > (int)sizeof(manufacturer)) {
        return false;
    }
    manufacturer[0] = companyId;
    manufacturer[1] = companyId >> 8;
    memcpy(manufacturer + 2, data, len);
    manufacturer_len = len + 2;
    return true;
}

bool BLELocalDevice::setAdvertisedServiceData(uint16_t uuid, const uint8_t data[], int len)
{
    if (len < 0 || len + 2 > (int)sizeof(service_data)) {
        return false;
    }
    service_data[0] = uuid;
    service_data[1] = uuid >> 8;
    memcpy(service_data + 2, data, len);
    service_data_len = len + 2;
    return true;
}

bool BLELocalDevice::setLocalName(const char *name)
{
    strncpy(local_name, name, sizeof(local_name) - 1);
    local_name[sizeof(local_name) - 1] = 0;
    return true;
}

void BLELocalDevice::setDeviceName(const char *name)
{
    strncpy(device_name, name, sizeof(device_name) - 1);
    device_name[sizeof(device_name) - 1] = 0;
    smcpad_ble_set_name(device_name);
}

void BLELocalDevice::setAppearance(uint16_t a)
{
    appearance = a;
    db_dirty = true;
}

void BLELocalDevice::addService(BLEService &service)
{
    BLEServiceImpl **p = &services;

    if (!service._s) {
        return;
    }
    while (*p) {
        if (*p == service._s) {
            return;
        }
        p = &(*p)->next;
    }
    *p = service._s;
    db_dirty = true;
}

static void ad(uint8_t *buf, uint8_t &n, uint8_t type, const uint8_t *data, int len)
{
    if (n + 2 + len > 31) {
        return;
    }
    buf[n++] = len + 1;
    buf[n++] = type;
    memcpy(buf + n, data, len);
    n += len;
}

int BLELocalDevice::advertise()
{
    uint8_t adv[31], rsp[31], an = 0, rn = 0;
    static const uint8_t flags = 0x06;          /* general discoverable, no BR/EDR */
    int name_len = strlen(local_name);

    if (db_dirty) {
        rebuild_db();
    }
    ad(adv, an, 0x01, &flags, 1);
    if (adv_uuid.len) {
        ad(adv, an, adv_uuid.len == 2 ? 0x03 : 0x07, adv_uuid.data, adv_uuid.len);
    }
    if (manufacturer_len) {
        ad(adv, an, 0xFF, manufacturer, manufacturer_len);
    }
    if (service_data_len) {
        ad(adv, an, 0x16, service_data, service_data_len);
    }
    if (name_len) {
        ad(rsp, rn, name_len > 29 ? 0x08 : 0x09, (const uint8_t *)local_name, name_len > 29 ? 29 : name_len);
    }
    smcpad_ble_set_name(device_name);
    smcpad_ble_set_profile(db, db_len);
    smcpad_ble_set_adv(adv, an, rsp, rn, adv_interval, connectable);
    smcpad_ble_advertise(1);
    return 1;
}

void BLELocalDevice::stopAdvertise()
{
    smcpad_ble_advertise(0);
}

BLEDevice BLELocalDevice::central()
{
    poll();
    return conn_handle ? BLEDevice(conn_handle, central_addr) : BLEDevice();
}

void BLELocalDevice::setEventHandler(BLEDeviceEvent event, BLEDeviceEventHandler handler)
{
    if (event < BLEDeviceLastEvent) {
        device_handlers[event] = handler;
    }
}

void BLELocalDevice::setAdvertisingInterval(uint16_t interval)
{
    adv_interval = interval;
}

void BLELocalDevice::setConnectable(bool c)
{
    connectable = c;
}

/* ------------------------------------------------------------------ BLEDevice */

void BLEDevice::poll(unsigned long timeout)
{
    BLE.poll(timeout);
}

bool BLEDevice::connected() const
{
    BLE.poll();
    return _handle && conn_handle == _handle;
}

bool BLEDevice::disconnect()
{
    if (!connected()) {
        return false;
    }
    smcpad_ble_disconnect();
    return true;
}

String BLEDevice::address() const
{
    return format_address(_addr);
}

int BLEDevice::rssi()
{
    return connected() ? smcpad_ble_rssi() : 127;
}

BLEDevice::operator bool() const
{
    static const uint8_t zero[6] = { 0 };

    return _handle != 0 || memcmp(_addr, zero, 6) != 0;
}

/* ------------------------------------------------------------------ BLEDescriptor */

BLEDescriptor::BLEDescriptor(const char *uuid, const uint8_t value[], int valueSize)
    : _d(new BLEDescriptorImpl())
{
    parse_uuid(uuid, _d->uuid);
    _d->len = valueSize > 0 ? valueSize : 0;
    _d->value = (uint8_t *)malloc(_d->len ? _d->len : 1);
    memcpy(_d->value, value, _d->len);
}

BLEDescriptor::BLEDescriptor(const char *uuid, const char *value)
    : BLEDescriptor(uuid, (const uint8_t *)value, strlen(value))
{
}

const char *BLEDescriptor::uuid() const
{
    return _d ? _d->uuid.str : "";
}

int BLEDescriptor::valueSize() const
{
    return _d ? _d->len : 0;
}

const uint8_t *BLEDescriptor::value() const
{
    return _d ? _d->value : nullptr;
}

int BLEDescriptor::readValue(uint8_t value[], int length)
{
    int n = valueSize() < length ? valueSize() : length;

    memcpy(value, this->value(), n);
    return n;
}

/* ------------------------------------------------------------------ BLECharacteristic */

BLECharacteristic::BLECharacteristic(const char *uuid, uint16_t properties, int valueSize, bool fixedLength)
    : _c(new BLECharacteristicImpl())
{
    parse_uuid(uuid, _c->uuid);
    _c->props = properties;
    _c->size = valueSize > 0 ? valueSize : 1;
    _c->fixed = fixedLength;
    _c->value = (uint8_t *)calloc(1, _c->size);
    _c->len = fixedLength ? _c->size : 0;
}

BLECharacteristic::BLECharacteristic(const char *uuid, uint16_t properties, const char *value)
    : BLECharacteristic(uuid, properties, strlen(value))
{
    writeValue(value);
}

const char *BLECharacteristic::uuid() const
{
    return _c ? _c->uuid.str : "";
}

uint16_t BLECharacteristic::properties() const
{
    return _c ? _c->props : 0;
}

int BLECharacteristic::valueSize() const
{
    return _c ? _c->size : 0;
}

const uint8_t *BLECharacteristic::value() const
{
    return _c ? _c->value : nullptr;
}

int BLECharacteristic::valueLength() const
{
    return _c ? _c->len : 0;
}

int BLECharacteristic::readValue(uint8_t value[], int length)
{
    int n = valueLength() < length ? valueLength() : length;

    if (n > 0) {
        memcpy(value, this->value(), n);
    }
    return n;
}

int BLECharacteristic::readValue(uint8_t &v) { return readValue(&v, sizeof(v)); }
int BLECharacteristic::readValue(int8_t &v) { return readValue((uint8_t *)&v, sizeof(v)); }
int BLECharacteristic::readValue(uint16_t &v) { return readValue((uint8_t *)&v, sizeof(v)); }
int BLECharacteristic::readValue(int16_t &v) { return readValue((uint8_t *)&v, sizeof(v)); }
int BLECharacteristic::readValue(uint32_t &v) { return readValue((uint8_t *)&v, sizeof(v)); }
int BLECharacteristic::readValue(int32_t &v) { return readValue((uint8_t *)&v, sizeof(v)); }

int BLECharacteristic::writeValue(const uint8_t value[], int length, bool withResponse)
{
    if (!_c || length < 0) {
        return 0;
    }
    if (length > _c->size) {
        length = _c->size;
    }
    memcpy(_c->value, value, length);
    _c->len = _c->fixed ? _c->size : length;
    if (_c->handle && smcpad_ble_subscribed(_c->handle)) {
        uint16_t n = _c->len, room = smcpad_ble_mtu() - 3;

        return smcpad_ble_notify(_c->handle, _c->value, n < room ? n : room);
    }
    return 1;
}

int BLECharacteristic::writeValue(const void *value, int length, bool withResponse)
{
    return writeValue((const uint8_t *)value, length, withResponse);
}

int BLECharacteristic::writeValue(const char *value, bool withResponse)
{
    return writeValue((const uint8_t *)value, strlen(value), withResponse);
}

int BLECharacteristic::writeValue(uint8_t v, bool r) { return writeValue((const uint8_t *)&v, sizeof(v), r); }
int BLECharacteristic::writeValue(int8_t v, bool r) { return writeValue((const uint8_t *)&v, sizeof(v), r); }
int BLECharacteristic::writeValue(uint16_t v, bool r) { return writeValue((const uint8_t *)&v, sizeof(v), r); }
int BLECharacteristic::writeValue(int16_t v, bool r) { return writeValue((const uint8_t *)&v, sizeof(v), r); }
int BLECharacteristic::writeValue(uint32_t v, bool r) { return writeValue((const uint8_t *)&v, sizeof(v), r); }
int BLECharacteristic::writeValue(int32_t v, bool r) { return writeValue((const uint8_t *)&v, sizeof(v), r); }

bool BLECharacteristic::written()
{
    if (!_c) {
        return false;
    }
    BLE.poll();
    if (_c->writes != _c->seen_written) {
        _c->seen_written = _c->writes;
        return true;
    }
    return false;
}

bool BLECharacteristic::subscribed()
{
    return _c && _c->handle && smcpad_ble_subscribed(_c->handle);
}

void BLECharacteristic::addDescriptor(BLEDescriptor &descriptor)
{
    BLEDescriptorImpl **p;

    if (!_c || !descriptor._d) {
        return;
    }
    for (p = &_c->descriptors; *p; p = &(*p)->next) {
        if (*p == descriptor._d) {
            return;
        }
    }
    *p = descriptor._d;
    db_dirty = true;
}

int BLECharacteristic::descriptorCount() const
{
    int n = 0;

    for (BLEDescriptorImpl *d = _c ? _c->descriptors : nullptr; d; d = d->next) {
        n++;
    }
    return n;
}

BLEDescriptor BLECharacteristic::descriptor(int index) const
{
    BLEDescriptor out;

    for (BLEDescriptorImpl *d = _c ? _c->descriptors : nullptr; d; d = d->next) {
        if (index-- == 0) {
            out._d = d;
            break;
        }
    }
    return out;
}

BLEDescriptor BLECharacteristic::descriptor(const char *uuid) const
{
    BLEDescriptor out;

    for (BLEDescriptorImpl *d = _c ? _c->descriptors : nullptr; d; d = d->next) {
        if (uuid_equal(d->uuid, uuid)) {
            out._d = d;
            break;
        }
    }
    return out;
}

void BLECharacteristic::setEventHandler(int event, BLECharacteristicEventHandler handler)
{
    if (_c && event >= 0 && event < BLECharacteristicEventLast) {
        _c->handlers[event] = handler;
    }
}

void BLECharacteristic::setRawWriteCallback(RawWriteCallback callback, void *arg)
{
    if (_c) {
        _c->raw_arg = arg;
        _c->raw_write = callback;
    }
}

/* ------------------------------------------------------------------ BLEService */

BLEService::BLEService(const char *uuid) : _s(new BLEServiceImpl())
{
    parse_uuid(uuid, _s->uuid);
}

const char *BLEService::uuid() const
{
    return _s ? _s->uuid.str : "";
}

void BLEService::addCharacteristic(BLECharacteristic &characteristic)
{
    BLECharacteristicImpl **p;

    if (!_s || !characteristic._c) {
        return;
    }
    for (p = &_s->characteristics; *p; p = &(*p)->next) {
        if (*p == characteristic._c) {
            return;
        }
    }
    *p = characteristic._c;
    db_dirty = true;
}

int BLEService::characteristicCount() const
{
    int n = 0;

    for (BLECharacteristicImpl *c = _s ? _s->characteristics : nullptr; c; c = c->next) {
        n++;
    }
    return n;
}

BLECharacteristic BLEService::characteristic(int index) const
{
    BLECharacteristic out;

    for (BLECharacteristicImpl *c = _s ? _s->characteristics : nullptr; c; c = c->next) {
        if (index-- == 0) {
            out._c = c;
            break;
        }
    }
    return out;
}

BLECharacteristic BLEService::characteristic(const char *uuid) const
{
    return characteristic(uuid, 0);
}

BLECharacteristic BLEService::characteristic(const char *uuid, int index) const
{
    BLECharacteristic out;

    for (BLECharacteristicImpl *c = _s ? _s->characteristics : nullptr; c; c = c->next) {
        if (uuid_equal(c->uuid, uuid) && index-- == 0) {
            out._c = c;
            break;
        }
    }
    return out;
}
