#include "EEPROM.h"

#define CHUNK       512
#define FIRST_KEY   40

EEPROMClass EEPROM;

/* the RAM copy is whole chunks, so chunks go to and from flash in place */
static int chunks(int size)
{
    return (size + CHUNK - 1) / CHUNK;
}

void EEPROMClass::begin(size_t size)
{
    if (_data) {
        return;
    }
    _size = size == 0 || size > MAX_SIZE ? MAX_SIZE : size;
    _data = (uint8_t *)malloc(chunks(_size) * CHUNK);
    memset(_data, 0xFF, chunks(_size) * CHUNK);
    for (int c = 0; c < chunks(_size); c++) {
        if (smcpad_store_read(FIRST_KEY + c, _data + c * CHUNK, CHUNK) <= 0) {
            memset(_data + c * CHUNK, 0xFF, CHUNK);
        }
    }
    _dirty = 0;
}

void EEPROMClass::load()
{
    if (!_data) {
        begin(MAX_SIZE);
    }
}

uint8_t EEPROMClass::read(int address)
{
    load();
    return address >= 0 && address < _size ? _data[address] : 0xFF;
}

void EEPROMClass::write(int address, uint8_t value)
{
    load();
    if (address >= 0 && address < _size && _data[address] != value) {
        _data[address] = value;
        _dirty |= 1 << (address / CHUNK);
    }
}

uint8_t &EEPROMClass::operator[](int address)
{
    load();
    if (address < 0 || address >= _size) {
        return _dummy;
    }
    _dirty |= 1 << (address / CHUNK);           /* may be written through the reference */
    return _data[address];
}

bool EEPROMClass::commit()
{
    bool ok = true;

    if (!_data) {
        return true;
    }
    for (int c = 0; c < chunks(_size); c++) {
        if (_dirty & (1 << c)) {
            if (smcpad_store_write(FIRST_KEY + c, _data + c * CHUNK, CHUNK) > 0) {
                _dirty &= ~(1 << c);
            } else {
                ok = false;
            }
        }
    }
    return ok;
}
