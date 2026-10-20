#include <Arduino.h>

String greeting = "hello from Arduino";     // global with a constructor: .init_array
static int n;

void setup()
{
    Serial.begin(115200);
}

void loop()
{
    Serial.print(greeting);
    Serial.print(", n=");
    Serial.print(n++);
    Serial.print(", pi=");
    Serial.print(3.14159, 3);
    Serial.print(", millis=");
    Serial.println(millis());
    delay(1000);
}
