#pragma once
#include <Arduino.h>

#define WL_CONNECTED 3
#define WL_DISCONNECTED 6
#define WL_IDLE_STATUS 0

// The web build is treated as always-online (the OTA fetch goes straight out
// of the browser); ESP-NOW trading is not available in a browser.
class WiFiClass {
public:
  void mode(int m) { (void)m; }
  void begin(const char *, const char *) {}
  void disconnect(bool = true) {}
  int status();
  String SSID(int i = -1);
  int32_t RSSI(int i = -1);
  int scanComplete();
  void scanNetworks(bool async = false, bool show_hidden = false, bool passive = false, uint32_t max_ms_per_chan = 300);
  void scanDelete();
};
extern WiFiClass WiFi;

#define WIFI_MODE_NULL 0
#define WIFI_MODE_STA 1
#define WIFI_STA 1
#define WIFI_OFF 0
#define WIFI_PS_NONE 0
#define WIFI_SECOND_CHAN_NONE 0
