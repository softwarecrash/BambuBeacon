#include "bblPrinterDiscovery.h"
#include "BambuMqttClient.h"

extern BambuMqttClient bambu;

BBLPrinterDiscovery::BBLPrinterDiscovery() {}

void BBLPrinterDiscovery::begin()
{
  enabled_ = true;
  udpReady_ = false;
  state_ = State::IDLE;
  knownCount_ = 0;
  nextRunMs_ = millis() + 1500; // small grace after boot
}

void BBLPrinterDiscovery::end()
{
  enabled_ = false;
  state_ = State::IDLE;
  if (udpReady_)
  {
    udp_.stop();
    udpReady_ = false;
  }
}

void BBLPrinterDiscovery::setInterval(unsigned long intervalMs)
{
  intervalMs_ = intervalMs;
}

void BBLPrinterDiscovery::setListenWindow(unsigned long listenWindowMs)
{
  listenWindowMs_ = listenWindowMs;
}

int BBLPrinterDiscovery::knownCount() const
{
  return knownCount_;
}

const BBLPrinter* BBLPrinterDiscovery::knownPrinters() const
{
  return knownPrinters_;
}

bool BBLPrinterDiscovery::isBusy() const
{
  return state_ != State::IDLE;
}

void BBLPrinterDiscovery::forceRescan(unsigned long minDelayMs)
{
  if (WiFi.status() != WL_CONNECTED) {
    state_ = State::IDLE;
    return;
  }
  forceRescan_ = true;
  nextRunMs_ = millis() + minDelayMs;
}

void BBLPrinterDiscovery::update()
{
  if (!enabled_) return;

  // Do nothing if WiFi is not connected
  if (WiFi.status() != WL_CONNECTED)
  {
    state_ = State::IDLE;
    return;
  }

  const unsigned long now = millis();

  // LISTEN state: keep parsing packets without blocking
  if (state_ == State::LISTEN)
  {
    readPacketsNonBlocking(now);

    if (now >= listenUntilMs_)
    {
      state_ = State::IDLE;
      nextRunMs_ = now + intervalMs_;
    }
    return;
  }

  // SEND states also run via readPacketsNonBlocking()
  if (state_ == State::SEND_1 || state_ == State::SEND_2)
  {
    readPacketsNonBlocking(now);
    return;
  }

  // Start a scan session only if timer elapsed
  if (!forceRescan_ && now < nextRunMs_) return;

  forceRescan_ = false;

  // Ensure UDP multicast is ready
  ensureUdp();

  // Kick off a scan session: send 2 m-search packets (spaced by sendGapMs_)
  state_ = State::SEND_1;
  sendAtMs_ = now;
  sessionFound_ = 0;
  seenCount_ = 0;
}

void BBLPrinterDiscovery::ensureUdp()
{
  if (udpReady_) return;

  udp_.beginMulticast(BBL_SSDP_MCAST_IP, BBL_SSDP_PORT);
  udpReady_ = true;
}

void BBLPrinterDiscovery::sendMSearch()
{
  static const char msearch[] =
    "M-SEARCH * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:2021\r\n"
    "MAN: \"ssdp:discover\"\r\n"
    "MX: 5\r\n"
    "ST: urn:bambulab-com:device:3dprinter:1\r\n\r\n";

  udp_.beginPacket(BBL_SSDP_MCAST_IP, BBL_SSDP_PORT);
  udp_.write(reinterpret_cast<const uint8_t*>(msearch), sizeof(msearch) - 1);
  udp_.endPacket();
}

bool BBLPrinterDiscovery::isKnown(IPAddress ip, int* index)
{
  for (int i = 0; i < knownCount_; i++)
  {
    if (knownPrinters_[i].ip == ip)
    {
      if (index) *index = i;
      return true;
    }
  }
  return false;
}

bool BBLPrinterDiscovery::hasSeenThisSession(IPAddress ip)
{
  for (int i = 0; i < seenCount_; i++)
  {
    if (seenIPs_[i] == ip) return true;
  }
  return false;
}

void BBLPrinterDiscovery::markSeenThisSession(IPAddress ip)
{
  if (seenCount_ < BBL_MAX_PRINTERS) seenIPs_[seenCount_++] = ip;
}

void BBLPrinterDiscovery::drainPacket(int size)
{
  while (size-- > 0) udp_.read();
}

void BBLPrinterDiscovery::readPacketsNonBlocking(unsigned long now)
{
  if (WiFi.status() != WL_CONNECTED) {
    state_ = State::IDLE;
    return;
  }
  // State machine for sending without blocking
  if (state_ == State::SEND_1 && now >= sendAtMs_)
  {
    webSerial.println("[BBLScan] Searching for printers...");
    sendMSearch();
    state_ = State::SEND_2;
    sendAtMs_ = now + sendGapMs_;
    return;
  }

  if (state_ == State::SEND_2 && now >= sendAtMs_)
  {
    sendMSearch();
    state_ = State::LISTEN;
    listenUntilMs_ = now + listenWindowMs_;
    return;
  }

  // LISTEN: parse any available packets
  while (true)
  {
    const int size = udp_.parsePacket();
    if (!size) break;

    const IPAddress senderIP = udp_.remoteIP();

    if (hasSeenThisSession(senderIP))
    {
      drainPacket(size);
      continue;
    }
    markSeenThisSession(senderIP);

    char buffer[512];
    const int len = udp_.read(buffer, sizeof(buffer) - 1);
    if (len <= 0) continue;
    buffer[len] = 0;

    // Extract USN line
    String response(buffer);
    String usnStr;
    int usnPos = response.indexOf("USN:");
    if (usnPos >= 0)
    {
      int end = response.indexOf("\r\n", usnPos);
      if (end < 0) end = response.length();
      usnStr = response.substring(usnPos + 4, end);
      usnStr.trim();
    }
    if (!usnStr.length()) {
      // Ignore non-Bambu responses (no USN).
      continue;
    }

    // Update settings IP if stored USN matches
    const char* storedUSN = settings.get.printerUSN();
    if (storedUSN && storedUSN[0] && usnStr.length() > 0 && strcmp(storedUSN, usnStr.c_str()) == 0)
    {
      const String currentIP = senderIP.toString();
      const char* storedIP = settings.get.printerIP();

      if (!storedIP || String(storedIP) != currentIP)
      {
        webSerial.printf("[BBLScan] Detected matching USN with updated IP (%s -> %s). Saving...\n",
                         storedIP ? storedIP : "(empty)", currentIP.c_str());
        settings.set.printerIP(currentIP.c_str());
        settings.save();
        bambu.reloadFromSettings();
        if (WiFi.status() == WL_CONNECTED) bambu.connect();
      }
    }

    int existingIndex = -1;
    const bool isNewPrinter = !isKnown(senderIP, &existingIndex);

    if (isNewPrinter)
    {
      webSerial.printf("[BBLScan]  [%d] IP: %s", ++sessionFound_, senderIP.toString().c_str());
      if (usnStr.length()) webSerial.printf("  [USN: %s]", usnStr.c_str());
      webSerial.println();
    }

    if (isNewPrinter && knownCount_ < BBL_MAX_PRINTERS)
    {
      BBLPrinter& p = knownPrinters_[knownCount_++];
      p.ip = senderIP;

      if (usnStr.length())
      {
        strncpy(p.usn, usnStr.c_str(), sizeof(p.usn) - 1);
        p.usn[sizeof(p.usn) - 1] = 0;
      }
      else
      {
        p.usn[0] = 0;
      }
    }
  }
}
