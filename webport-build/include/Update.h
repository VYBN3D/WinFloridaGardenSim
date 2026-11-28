#pragma once
#include <Arduino.h>
class UpdateClass { public: bool begin(size_t); size_t writeStream(class WiFiClientSecure&); bool end(bool=false); void abort(); };
extern UpdateClass Update;
