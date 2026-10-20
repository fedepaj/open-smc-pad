/*
 * ArduinoBLE for the SMC-PAD: the ArduinoBLE API (peripheral role) on the runtime's GATT server
 * (smcpad_ble_*). Sketches written for ArduinoBLE peripherals build unchanged.
 *
 * Differences from ArduinoBLE:
 *  - peripheral only: scan() and the central API compile, but never find anything;
 *  - BLERead handlers run after the read is served (poll()), not before it;
 *  - event handlers, written() and subscribed() are updated by BLE.poll(), which
 *    central(), connected() and written() also call.
 */
#ifndef ARDUINO_BLE_H
#define ARDUINO_BLE_H

#include <Arduino.h>

enum BLEProperty {
    BLEBroadcast            = 0x01,
    BLERead                 = 0x02,
    BLEWriteWithoutResponse = 0x04,
    BLEWrite                = 0x08,
    BLENotify               = 0x10,
    BLEIndicate             = 0x20,
    BLEAuthSignedWrite      = 1 << 6,
    BLEExtProp              = 1 << 7,
};

enum BLEDeviceEvent {
    BLEConnected = 0,
    BLEDisconnected,
    BLEDiscovered,
    BLEDeviceLastEvent
};

/* as in ArduinoBLE, event 2 is the property BLERead (0x02) */
enum BLECharacteristicEvent {
    BLESubscribed = 0,
    BLEUnsubscribed = 1,
    BLEWritten = 3,
    BLEUpdated = BLEWritten,
    BLECharacteristicEventLast
};

class BLEDevice;
class BLECharacteristic;

typedef void (*BLEDeviceEventHandler)(BLEDevice device);
typedef void (*BLECharacteristicEventHandler)(BLEDevice device, BLECharacteristic characteristic);

struct BLEUuidValue {
    uint8_t len;                    /* 2 or 16 */
    uint8_t data[16];               /* little-endian */
    char str[37];
};

/* ------------------------------------------------------------------ device */

class BLEDevice {
public:
    BLEDevice() : _handle(0) { memset(_addr, 0, 6); }
    BLEDevice(uint16_t handle, const uint8_t addr[6]) : _handle(handle) { memcpy(_addr, addr, 6); }

    void poll(unsigned long timeout = 0);
    bool connected() const;
    bool disconnect();
    String address() const;
    int rssi();

    /* central role: nothing is ever discovered */
    bool hasLocalName() const { return false; }
    bool hasAdvertisedServiceUuid() const { return false; }
    bool hasAdvertisedServiceUuid(int) const { return false; }
    int advertisedServiceUuidCount() const { return 0; }
    String localName() const { return String(); }
    String advertisedServiceUuid() const { return String(); }
    String advertisedServiceUuid(int) const { return String(); }
    bool connect() { return false; }
    bool discoverAttributes() { return false; }
    bool discoverService(const char *) { return false; }
    String deviceName() { return String(); }
    int appearance() { return 0; }
    int serviceCount() const { return 0; }
    bool hasService(const char *) const { return false; }
    int characteristicCount() const { return 0; }
    bool hasCharacteristic(const char *) const { return false; }

    operator bool() const;
    bool operator==(const BLEDevice &rhs) const { return _handle == rhs._handle && !memcmp(_addr, rhs._addr, 6); }
    bool operator!=(const BLEDevice &rhs) const { return !(*this == rhs); }

private:
    uint16_t _handle;
    uint8_t _addr[6];
};

/* ------------------------------------------------------------------ descriptor */

struct BLEDescriptorImpl;

class BLEDescriptor {
public:
    BLEDescriptor() : _d(nullptr) {}
    BLEDescriptor(const char *uuid, const uint8_t value[], int valueSize);
    BLEDescriptor(const char *uuid, const char *value);

    const char *uuid() const;
    int valueSize() const;
    const uint8_t *value() const;
    int valueLength() const { return valueSize(); }
    uint8_t operator[](int offset) const { return value()[offset]; }
    int readValue(uint8_t value[], int length);
    operator bool() const { return _d != nullptr; }

private:
    friend class BLECharacteristic;
    friend class BLELocalDevice;
    BLEDescriptorImpl *_d;
};

/* ------------------------------------------------------------------ characteristic */

struct BLECharacteristicImpl;

class BLECharacteristic {
public:
    BLECharacteristic() : _c(nullptr) {}
    BLECharacteristic(const char *uuid, uint16_t properties, int valueSize, bool fixedLength = false);
    BLECharacteristic(const char *uuid, uint16_t properties, const char *value);

    const char *uuid() const;
    uint16_t properties() const;

    int valueSize() const;
    const uint8_t *value() const;
    int valueLength() const;
    uint8_t operator[](int offset) const { return value()[offset]; }
    int readValue(uint8_t value[], int length);
    int readValue(void *value, int length) { return readValue((uint8_t *)value, length); }
    int readValue(uint8_t &value);
    int readValue(int8_t &value);
    int readValue(uint16_t &value);
    int readValue(int16_t &value);
    int readValue(uint32_t &value);
    int readValue(int32_t &value);

    /* sets the value; notifies / indicates it when the central subscribed. 1 = done */
    int writeValue(const uint8_t value[], int length, bool withResponse = true);
    int writeValue(const void *value, int length, bool withResponse = true);
    int writeValue(const char *value, bool withResponse = true);
    int writeValue(uint8_t value, bool withResponse = true);
    int writeValue(int8_t value, bool withResponse = true);
    int writeValue(uint16_t value, bool withResponse = true);
    int writeValue(int16_t value, bool withResponse = true);
    int writeValue(uint32_t value, bool withResponse = true);
    int writeValue(int32_t value, bool withResponse = true);

    /* ArduinoBLE's deprecated names */
    int setValue(const uint8_t value[], int length) { return writeValue(value, length); }
    int setValue(const char *value) { return writeValue(value); }

    int broadcast() { return 0; }

    bool written();                 /* once per write by the central */
    bool subscribed();
    bool valueUpdated() { return false; }   /* central role */
    bool canRead() { return false; }
    bool read() { return false; }
    bool canWrite() { return false; }
    bool canSubscribe() { return false; }
    bool subscribe() { return false; }
    bool canUnsubscribe() { return false; }
    bool unsubscribe() { return false; }

    void addDescriptor(BLEDescriptor &descriptor);
    int descriptorCount() const;
    BLEDescriptor descriptor(int index) const;
    BLEDescriptor descriptor(const char *uuid) const;
    bool hasDescriptor(const char *uuid) const { return (bool)descriptor(uuid); }

    void setEventHandler(int event, BLECharacteristicEventHandler handler);

    /* Not in ArduinoBLE: called for each write as it arrives, in the BT stack's task (keep it
     * short, no BLE calls); for streams such as BLE-MIDI, where poll() would only see the last
     * value. Such a characteristic keeps no value: reads are empty. */
    typedef void (*RawWriteCallback)(const uint8_t *data, int length, void *arg);
    void setRawWriteCallback(RawWriteCallback callback, void *arg = nullptr);

    operator bool() const { return _c != nullptr; }

private:
    friend class BLEService;
    friend class BLELocalDevice;
    friend struct BLECharacteristicImpl;
    BLECharacteristicImpl *_c;
};

/* ------------------------------------------------------------------ service */

struct BLEServiceImpl;

class BLEService {
public:
    BLEService() : _s(nullptr) {}
    BLEService(const char *uuid);

    const char *uuid() const;
    void addCharacteristic(BLECharacteristic &characteristic);
    int characteristicCount() const;
    bool hasCharacteristic(const char *uuid) const { return (bool)characteristic(uuid); }
    bool hasCharacteristic(const char *uuid, int index) const { return (bool)characteristic(uuid, index); }
    BLECharacteristic characteristic(int index) const;
    BLECharacteristic characteristic(const char *uuid) const;
    BLECharacteristic characteristic(const char *uuid, int index) const;

    operator bool() const { return _s != nullptr; }

private:
    friend class BLELocalDevice;
    BLEServiceImpl *_s;
};

/* ------------------------------------------------------------------ the local device */

class BLELocalDevice {
public:
    int begin();                    /* 1 once the stack is up */
    void end();
    void poll(unsigned long timeout = 0);

    bool connected() const;
    bool disconnect();
    String address() const;
    int rssi();

    bool setAdvertisedServiceUuid(const char *advertisedServiceUuid);
    bool setAdvertisedService(const BLEService &service);
    bool setManufacturerData(const uint8_t manufacturerData[], int manufacturerDataLength);
    bool setManufacturerData(const uint16_t companyId, const uint8_t manufacturerData[], int manufacturerDataLength);
    bool setAdvertisedServiceData(uint16_t uuid, const uint8_t data[], int length);
    bool setLocalName(const char *localName);
    void setDeviceName(const char *deviceName);
    void setAppearance(uint16_t appearance);

    void addService(BLEService &service);

    int advertise();
    void stopAdvertise();

    /* central role: nothing is ever found */
    int scan(bool withDuplicates = false) { return 0; }
    int scanForName(String, bool withDuplicates = false) { return 0; }
    int scanForUuid(String, bool withDuplicates = false) { return 0; }
    int scanForAddress(String, bool withDuplicates = false) { return 0; }
    void stopScan() {}

    BLEDevice central();
    BLEDevice available() { return BLEDevice(); }

    void setEventHandler(BLEDeviceEvent event, BLEDeviceEventHandler eventHandler);

    void setAdvertisingInterval(uint16_t advertisingInterval);     /* 0.625 ms units */
    void setConnectionInterval(uint16_t minimumConnectionInterval, uint16_t maximumConnectionInterval) {}
    void setSupervisionTimeout(uint16_t) {}
    void setConnectable(bool connectable);
    void setTimeout(unsigned long timeout) {}

    void debug(Stream &stream) {}
    void noDebug() {}
};

extern BLELocalDevice BLE;

/* ------------------------------------------------------------------ typed characteristics */

template <typename T> class BLETypedCharacteristic : public BLECharacteristic {
public:
    BLETypedCharacteristic(const char *uuid, unsigned int permissions)
        : BLECharacteristic(uuid, permissions, sizeof(T), true)
    {
        T v = T();
        BLECharacteristic::writeValue((const uint8_t *)&v, sizeof(T));
    }

    int writeValue(T value) { return BLECharacteristic::writeValue((const uint8_t *)&value, sizeof(T)); }
    int setValue(T value) { return writeValue(value); }
    T value()
    {
        T v = T();
        int n = valueLength() < (int)sizeof(T) ? valueLength() : (int)sizeof(T);

        memcpy(&v, BLECharacteristic::value(), n);
        return v;
    }
    int writeValueLE(T value) { return writeValue(value); }
    int setValueLE(T value) { return writeValue(value); }
    T valueLE() { return value(); }
    int writeValueBE(T value) { return writeValue(byteSwap(value)); }
    int setValueBE(T value) { return writeValueBE(value); }
    T valueBE() { return byteSwap(value()); }

private:
    static T byteSwap(T value)
    {
        T out;
        const uint8_t *in = (const uint8_t *)&value;
        uint8_t *o = (uint8_t *)&out;

        for (unsigned i = 0; i < sizeof(T); i++) {
            o[i] = in[sizeof(T) - 1 - i];
        }
        return out;
    }
};

class BLEBoolCharacteristic : public BLETypedCharacteristic<bool> {
public: BLEBoolCharacteristic(const char *uuid, unsigned char properties) : BLETypedCharacteristic<bool>(uuid, properties) {}
};
class BLEByteCharacteristic : public BLETypedCharacteristic<byte> {
public: BLEByteCharacteristic(const char *uuid, unsigned char properties) : BLETypedCharacteristic<byte>(uuid, properties) {}
};
class BLECharCharacteristic : public BLETypedCharacteristic<char> {
public: BLECharCharacteristic(const char *uuid, unsigned char properties) : BLETypedCharacteristic<char>(uuid, properties) {}
};
class BLEUnsignedCharCharacteristic : public BLETypedCharacteristic<unsigned char> {
public: BLEUnsignedCharCharacteristic(const char *uuid, unsigned char properties) : BLETypedCharacteristic<unsigned char>(uuid, properties) {}
};
class BLEShortCharacteristic : public BLETypedCharacteristic<short> {
public: BLEShortCharacteristic(const char *uuid, unsigned char properties) : BLETypedCharacteristic<short>(uuid, properties) {}
};
class BLEUnsignedShortCharacteristic : public BLETypedCharacteristic<unsigned short> {
public: BLEUnsignedShortCharacteristic(const char *uuid, unsigned char properties) : BLETypedCharacteristic<unsigned short>(uuid, properties) {}
};
class BLEIntCharacteristic : public BLETypedCharacteristic<int> {
public: BLEIntCharacteristic(const char *uuid, unsigned char properties) : BLETypedCharacteristic<int>(uuid, properties) {}
};
class BLEUnsignedIntCharacteristic : public BLETypedCharacteristic<unsigned int> {
public: BLEUnsignedIntCharacteristic(const char *uuid, unsigned char properties) : BLETypedCharacteristic<unsigned int>(uuid, properties) {}
};
class BLELongCharacteristic : public BLETypedCharacteristic<long> {
public: BLELongCharacteristic(const char *uuid, unsigned char properties) : BLETypedCharacteristic<long>(uuid, properties) {}
};
class BLEUnsignedLongCharacteristic : public BLETypedCharacteristic<unsigned long> {
public: BLEUnsignedLongCharacteristic(const char *uuid, unsigned char properties) : BLETypedCharacteristic<unsigned long>(uuid, properties) {}
};
class BLEFloatCharacteristic : public BLETypedCharacteristic<float> {
public: BLEFloatCharacteristic(const char *uuid, unsigned char properties) : BLETypedCharacteristic<float>(uuid, properties) {}
};
class BLEDoubleCharacteristic : public BLETypedCharacteristic<double> {
public: BLEDoubleCharacteristic(const char *uuid, unsigned char properties) : BLETypedCharacteristic<double>(uuid, properties) {}
};
class BLEWordCharacteristic : public BLETypedCharacteristic<word> {
public: BLEWordCharacteristic(const char *uuid, unsigned char properties) : BLETypedCharacteristic<word>(uuid, properties) {}
};

class BLEStringCharacteristic : public BLECharacteristic {
public:
    BLEStringCharacteristic(const char *uuid, unsigned char properties, int valueSize)
        : BLECharacteristic(uuid, properties, valueSize) {}

    int writeValue(const String &value) { return BLECharacteristic::writeValue(value.c_str()); }
    int setValue(const String &value) { return writeValue(value); }
    String value()
    {
        String s;
        const uint8_t *v = BLECharacteristic::value();

        for (int i = 0; i < valueLength(); i++) {
            s += (char)v[i];
        }
        return s;
    }
};

#endif
