#ifndef _BBLPRINTERDISCOVERY_H
#define _BBLPRINTERDISCOVERY_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include "WebSerial.h"
#include "SettingsPrefs.h"

// Use the project's global instances (do not redeclare them elsewhere)
extern WebSerialClass webSerial;
extern Settings settings;

#ifndef BBL_SSDP_PORT
#define BBL_SSDP_PORT 2021
#endif

#ifndef BBL_SSDP_MCAST_IP
#define BBL_SSDP_MCAST_IP IPAddress(239, 255, 255, 250)
#endif

#ifndef BBL_MAX_PRINTERS
#define BBL_MAX_PRINTERS 10
#endif

struct BBLPrinter
{
  IPAddress ip;
  char usn[64];
};

class BBLPrinterDiscovery
{
public:
  BBLPrinterDiscovery();

  void begin();
  void end();

  // Call this from loop()
  void update();

  // Force a new scan soon (e.g. on MQTT failure / printer offline)
  void forceRescan(unsigned long minDelayMs = 0);

  // How often to run a scan in normal operation
  void setInterval(unsigned long intervalMs);

  // How long to listen for responses per scan
  void setListenWindow(unsigned long listenWindowMs);

  // Access last known printers
  int knownCount() const;
  const BBLPrinter* knownPrinters() const;

private:
  enum class State : uint8_t
  {
    IDLE,
    SEND_1,
    SEND_2,
    LISTEN
  };

  void ensureUdp();
  void sendMSearch();
  void readPacketsNonBlocking(unsigned long now);
  void drainPacket(int size);

  bool isKnown(IPAddress ip, int* index = nullptr);
  bool hasSeenThisSession(IPAddress ip);
  void markSeenThisSession(IPAddress ip);

private:
  // Config
  unsigned long intervalMs_     = 10000UL; // default 10s
  unsigned long listenWindowMs_ = 5000UL;  // default 5s
  unsigned long sendGapMs_      = 250UL;   // gap between m-search packets

  // Runtime
  bool enabled_     = false;
  bool udpReady_    = false;
  bool forceRescan_ = false;

  State state_ = State::IDLE;

  unsigned long nextRunMs_     = 0;
  unsigned long sendAtMs_      = 0;
  unsigned long listenUntilMs_ = 0;

  WiFiUDP udp_;

  // Session / known printers
  BBLPrinter knownPrinters_[BBL_MAX_PRINTERS];
  int knownCount_ = 0;

  IPAddress seenIPs_[BBL_MAX_PRINTERS];
  int seenCount_ = 0;

  int sessionFound_ = 0;
};

#endif
