#include "Arduino.h"

SerialUSB_ Serial;

int SerialUSB_::available() { return smcpad_serial_available(); }
int SerialUSB_::peek() { return smcpad_serial_peek(); }
int SerialUSB_::read() { return smcpad_serial_read(); }
void SerialUSB_::flush() { smcpad_serial_flush(); }
size_t SerialUSB_::write(uint8_t c) { return smcpad_serial_write(&c, 1); }
size_t SerialUSB_::write(const uint8_t *buffer, size_t size) { return smcpad_serial_write(buffer, size); }
SerialUSB_::operator bool() { return smcpad_serial_connected(); }
