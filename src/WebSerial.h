#pragma once

#include <Arduino.h>
#include <MycilaWebSerial.h>

class AsyncWebServer;

// WebSerialClass mirrors Serial output to WebSerial (WS) and keeps Serial RX working.
class WebSerialClass : public Stream {
public:
  void begin(unsigned long baud = 115200) {
    Serial.begin(baud);
  }

  void begin(AsyncWebServer* server, unsigned long baud = 115200, size_t bufferSize = 100) {
    Serial.begin(baud);

    _ws.begin(server);
    _ws.setBuffer(bufferSize);

    _ws.onMessage([](const std::string& msg) {
      Serial.print("[WebSerial RX] ");
      Serial.println(msg.c_str());
    });
  }

  void onMessage(std::function<void(const std::string&)> cb) {
    _ws.onMessage(cb);
  }

  void setBuffer(size_t size) {
    _ws.setBuffer(size);
  }

  // Stream
  int available() override { return Serial.available(); }
  int read() override { return Serial.read(); }
  int peek() override { return Serial.peek(); }
  void flush() override { Serial.flush(); }

  size_t write(uint8_t b) override {
    Serial.write(b);
    _ws.write(&b, 1);
    return 1;
  }

  size_t write(const uint8_t* buffer, size_t size) override {
    Serial.write(buffer, size);
    _ws.write(buffer, size);
    return size;
  }

  using Print::write;
  operator bool() { return (bool)Serial; }

private:
  WebSerial _ws;
};

// Global instance (defined in webSerial.cpp)
extern WebSerialClass webSerial;

// Optional compatibility alias (if your code uses LogSerial already)
#define LogSerial webSerial
