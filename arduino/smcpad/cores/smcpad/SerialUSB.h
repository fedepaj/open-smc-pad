/* Serial: the USB CDC port (TinyUSB, in the precompiled core). Baud rate and format are
 * ignored, as on every native-USB board. */
#pragma once

#include "api/HardwareSerial.h"

class SerialUSB_ : public arduino::HardwareSerial {
public:
    void begin(unsigned long) override {}
    void begin(unsigned long, uint16_t) override {}
    void end() override {}
    int available() override;
    int peek() override;
    int read() override;
    void flush() override;
    size_t write(uint8_t c) override;
    size_t write(const uint8_t *buffer, size_t size) override;
    using arduino::Print::write;
    operator bool() override;
};

extern SerialUSB_ Serial;
