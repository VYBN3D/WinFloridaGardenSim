#pragma once
#include <Arduino.h>
#include <WiFi.h>

// Minimal blocking HTTP(S) GET through the browser (asyncified fetch).
class WiFiClientSecure {
public:
  void setInsecure() {}
  void setTimeout(int ms) { (void)ms; }
  const uint8_t *streamData() const;   // raw body bytes for Update.writeStream
  size_t streamSize() const;
  WiFiClientSecure *getStreamPtr() { return this; }
};

class HTTPClient {
public:
  void setFollowRedirects(int) {}
  void setTimeout(int) {}
  bool begin(WiFiClientSecure &, const String &url);
  bool begin(const String &url);
  int GET();
  int getSize();
  WiFiClientSecure *getStreamPtr();
  String getString();
  void end();
};
#define HTTPC_STRICT_FOLLOW_REDIRECTS 2
