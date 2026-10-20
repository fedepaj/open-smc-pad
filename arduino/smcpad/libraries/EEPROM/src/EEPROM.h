/*
 * EEPROM for the SMC-PAD: a RAM copy of up to 4096 bytes, kept in flash (the runtime's
 * storage keys 40-47, 512 bytes each) by commit(); only the changed 512-byte chunks are
 * written. As on ESP32 / RP2040: call commit() after writing, not in a tight loop (each commit
 * writes flash). Unwritten bytes read 0xFF.
 */
#pragma once
#include <Arduino.h>

class EEPROMClass {
public:
    static const int MAX_SIZE = 4096;

    void begin(size_t size = MAX_SIZE);         /* loads; optional (first use loads all) */
    bool commit();                              /* true when everything is in flash */
    void end() { commit(); }

    uint8_t read(int address);
    void write(int address, uint8_t value);
    void update(int address, uint8_t value) { write(address, value); }
    uint8_t &operator[](int address);
    uint16_t length() { load(); return _size; }

    template <typename T> T &get(int address, T &t)
    {
        load();
        if (address >= 0 && address + sizeof(T) <= _size) {
            memcpy(&t, _data + address, sizeof(T));
        }
        return t;
    }
    template <typename T> const T &put(int address, const T &t)
    {
        load();
        if (address >= 0 && address + sizeof(T) <= _size) {
            for (size_t i = 0; i < sizeof(T); i++) {
                write(address + i, ((const uint8_t *)&t)[i]);
            }
        }
        return t;
    }

private:
    void load();
    uint8_t *_data = nullptr;
    uint16_t _size = 0;
    uint8_t _dirty = 0;                         /* one bit per 512-byte chunk */
    uint8_t _dummy;
};

extern EEPROMClass EEPROM;
