#pragma once

#include <Arduino.h>

namespace sg98_protocol {

constexpr char kFrameSeparator = '|';
constexpr char kListSeparator = ',';

constexpr const char* kFrameAck = "ACK";
constexpr const char* kFrameStatus = "STAT";
constexpr const char* kFrameMenu = "MENU";
constexpr const char* kFramePeer = "PEER";
constexpr const char* kFrameEnvironment = "ENV";
constexpr const char* kFrameWifiConfig = "WIFI";

constexpr const char* kCmdStart = "START";
constexpr const char* kCmdStop = "STOP";
constexpr const char* kCmdIdle = "IDLE";
constexpr const char* kCmdIdleGbw = "IDLE_GBW";
constexpr const char* kCmdGetStatus = "GET_STATUS";
constexpr const char* kCmdGetEnvironment = "GET_ENV";
constexpr const char* kCmdDiscover = "DISCOVER";
constexpr const char* kCmdHeartbeat = "HEARTBEAT";

constexpr size_t kMaxMenuValues = 24;

struct AckFrame {
  bool ok = false;
  String message;
};

struct StatusFrame {
  String state;
  String drive;
  String comm;
  String motor;
  String scale;
  int32_t error = 0;
  int32_t setRpm = 0;
  int32_t motorSetRpm = 0;
  int32_t currentRpm = 0;
  int32_t torquePercent = 0;
  int32_t setWeightMg = 0;
  int32_t currentWeightMg = 0;
  int32_t currentMenu = 0;
  bool enterMenu = false;
  int32_t jmcBaud = 0;
  int32_t preferredJmcBaud = 0;
  bool wifiSetupComplete = false;
  bool wifiSaved = false;
  bool wifiStaConnected = false;
  int32_t wifiApStations = 0;
  bool envSyncFieldsPresent = false;
  bool envTimeValid = false;
  bool envWeatherValid = false;
  bool burrGapFieldsPresent = false;
  bool burrGapEnabled = false;
  bool burrGapKnown = false;
  bool burrGapLive = false;
  int32_t burrGapUm = 0;
  bool controllerOtaReadyFieldsPresent = false;
  bool controllerOtaReady = false;
};

struct MenuFrame {
  uint8_t menuNum = 0;
  int32_t values[kMaxMenuValues] = {};
  uint8_t count = 0;
};

struct PeerFrame {
  String name;
  String mac;
  String target;
  int32_t channel = 0;
};

struct EnvironmentFrame {
  bool timeValid = false;
  uint32_t unixTime = 0;
  bool weatherValid = false;
  int16_t temperatureF = 0;
  uint16_t weatherCode = 0;
  int8_t precipitationPercent = -1;
  String location;
};

struct WifiConfigFrame {
  String ssid;
  String password;
};

inline String setRpm(int32_t rpm) {
  return String("SET_RPM=") + String(rpm);
}

inline String setWeightMg(int32_t weightMg) {
  return String("SET_WEIGHT=") + String(weightMg);
}

inline String getMenu(uint8_t menuNum) {
  return String("GET_MENU=") + String(menuNum);
}

inline String setMenuValue(uint8_t menuNum, uint8_t itemIdx, int32_t value) {
  String out = "SET_MENU=";
  out += String(menuNum);
  out += ',';
  out += String(itemIdx);
  out += ',';
  out += String(value);
  return out;
}

inline String action(const char* actionName, const char* arg0 = nullptr) {
  String out = "ACTION=";
  out += actionName != nullptr ? actionName : "";
  if(arg0 != nullptr && *arg0 != '\0') {
    out += ',';
    out += arg0;
  }
  return out;
}

inline String discoverRequest() {
  return kCmdDiscover;
}

inline String ack(bool ok, const String& message) {
  String out;
  out.reserve(message.length() + 8);
  out += kFrameAck;
  out += kFrameSeparator;
  out += ok ? '1' : '0';
  out += kFrameSeparator;
  out += message;
  return out;
}

inline String peer(const String& name, const String& mac, const String& target, int32_t channel) {
  String out;
  out.reserve(name.length() + mac.length() + target.length() + 24);
  out += kFramePeer;
  out += kFrameSeparator;
  out += name;
  out += kFrameSeparator;
  out += mac;
  out += kFrameSeparator;
  out += target;
  out += kFrameSeparator;
  out += String(channel);
  return out;
}

inline String environment(const EnvironmentFrame& frame) {
  String out;
  out.reserve(frame.location.length() + 48);
  out += kFrameEnvironment;
  out += kFrameSeparator;
  out += frame.timeValid ? '1' : '0';
  out += kFrameSeparator;
  out += String(static_cast<unsigned long>(frame.unixTime));
  out += kFrameSeparator;
  out += frame.weatherValid ? '1' : '0';
  out += kFrameSeparator;
  out += String(static_cast<int>(frame.temperatureF));
  out += kFrameSeparator;
  out += String(static_cast<unsigned>(frame.weatherCode));
  out += kFrameSeparator;
  out += String(static_cast<int>(frame.precipitationPercent));
  out += kFrameSeparator;
  String location = frame.location;
  location.replace(String(kFrameSeparator), String(' '));
  location.replace(String(kListSeparator), String(' '));
  out += location;
  return out;
}

namespace detail {

inline char hexNibble(uint8_t value) {
  value &= 0x0F;
  return value < 10 ? static_cast<char>('0' + value)
                    : static_cast<char>('A' + value - 10);
}

inline int hexValue(char ch) {
  if(ch >= '0' && ch <= '9') return ch - '0';
  if(ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if(ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

inline String hexEncode(const String& value) {
  String out;
  out.reserve(value.length() * 2);
  for(size_t i = 0; i < value.length(); ++i) {
    const uint8_t ch = static_cast<uint8_t>(value.charAt(i));
    out += hexNibble(ch >> 4);
    out += hexNibble(ch);
  }
  return out;
}

inline bool hexDecode(const String& value, String* out) {
  if(out == nullptr || (value.length() % 2) != 0) {
    return false;
  }
  String decoded;
  decoded.reserve(value.length() / 2);
  for(size_t i = 0; i < value.length(); i += 2) {
    const int high = hexValue(value.charAt(i));
    const int low = hexValue(value.charAt(i + 1));
    if(high < 0 || low < 0) {
      return false;
    }
    decoded += static_cast<char>((high << 4) | low);
  }
  *out = decoded;
  return true;
}

}  // namespace detail

inline String wifiConfig(const String& ssid, const String& password) {
  String out;
  out.reserve(ssid.length() * 2 + password.length() * 2 + 8);
  out += kFrameWifiConfig;
  out += kFrameSeparator;
  out += detail::hexEncode(ssid);
  out += kFrameSeparator;
  out += detail::hexEncode(password);
  return out;
}

namespace detail {

inline bool nextToken(const String& input, int& start, char separator, String* token) {
  if(token == nullptr || start > input.length()) {
    return false;
  }

  const int end = input.indexOf(separator, start);
  if(end < 0) {
    *token = input.substring(start);
    start = input.length() + 1;
    return true;
  }

  *token = input.substring(start, end);
  start = end + 1;
  return true;
}

inline bool parseIntToken(const String& token, int32_t* value) {
  if(value == nullptr || token.length() == 0) {
    return false;
  }

  char* endPtr = nullptr;
  const long parsed = strtol(token.c_str(), &endPtr, 10);
  if(endPtr == nullptr || *endPtr != '\0') {
    return false;
  }

  *value = static_cast<int32_t>(parsed);
  return true;
}

}  // namespace detail

inline bool parseAckFrame(const String& frame, AckFrame* out) {
  if(out == nullptr || !frame.startsWith(String(kFrameAck) + kFrameSeparator)) {
    return false;
  }

  int start = String(kFrameAck).length() + 1;
  String okToken;
  String messageToken;
  if(!detail::nextToken(frame, start, kFrameSeparator, &okToken)) {
    return false;
  }
  detail::nextToken(frame, start, kFrameSeparator, &messageToken);

  out->ok = (okToken == "1");
  out->message = messageToken;
  return true;
}

inline bool parseStatusFrame(const String& frame, StatusFrame* out) {
  if(out == nullptr || !frame.startsWith(String(kFrameStatus) + kFrameSeparator)) {
    return false;
  }

  int start = String(kFrameStatus).length() + 1;
  String token;
  int32_t parsed = 0;

  if(!detail::nextToken(frame, start, kFrameSeparator, &out->state)) return false;
  if(!detail::nextToken(frame, start, kFrameSeparator, &out->drive)) return false;
  if(!detail::nextToken(frame, start, kFrameSeparator, &out->comm)) return false;
  if(!detail::nextToken(frame, start, kFrameSeparator, &out->motor)) return false;
  if(!detail::nextToken(frame, start, kFrameSeparator, &out->scale)) return false;

  if(!detail::nextToken(frame, start, kFrameSeparator, &token) || !detail::parseIntToken(token, &parsed)) return false;
  out->error = parsed;
  if(!detail::nextToken(frame, start, kFrameSeparator, &token) || !detail::parseIntToken(token, &parsed)) return false;
  out->setRpm = parsed;
  if(!detail::nextToken(frame, start, kFrameSeparator, &token) || !detail::parseIntToken(token, &parsed)) return false;
  out->motorSetRpm = parsed;
  if(!detail::nextToken(frame, start, kFrameSeparator, &token) || !detail::parseIntToken(token, &parsed)) return false;
  out->currentRpm = parsed;
  if(!detail::nextToken(frame, start, kFrameSeparator, &token) || !detail::parseIntToken(token, &parsed)) return false;
  out->torquePercent = parsed;
  if(!detail::nextToken(frame, start, kFrameSeparator, &token) || !detail::parseIntToken(token, &parsed)) return false;
  out->setWeightMg = parsed;
  if(!detail::nextToken(frame, start, kFrameSeparator, &token) || !detail::parseIntToken(token, &parsed)) return false;
  out->currentWeightMg = parsed;
  if(!detail::nextToken(frame, start, kFrameSeparator, &token) || !detail::parseIntToken(token, &parsed)) return false;
  out->currentMenu = parsed;
  if(!detail::nextToken(frame, start, kFrameSeparator, &token) || !detail::parseIntToken(token, &parsed)) return false;
  out->enterMenu = (parsed != 0);
  if(!detail::nextToken(frame, start, kFrameSeparator, &token) || !detail::parseIntToken(token, &parsed)) return false;
  out->jmcBaud = parsed;
  if(!detail::nextToken(frame, start, kFrameSeparator, &token) || !detail::parseIntToken(token, &parsed)) return false;
  out->preferredJmcBaud = parsed;
  if(detail::nextToken(frame, start, kFrameSeparator, &token) && detail::parseIntToken(token, &parsed)) {
    out->wifiSetupComplete = parsed != 0;
  }
  if(detail::nextToken(frame, start, kFrameSeparator, &token) && detail::parseIntToken(token, &parsed)) {
    out->wifiSaved = parsed != 0;
  }
  if(detail::nextToken(frame, start, kFrameSeparator, &token) && detail::parseIntToken(token, &parsed)) {
    out->wifiStaConnected = parsed != 0;
  }
  if(detail::nextToken(frame, start, kFrameSeparator, &token) && detail::parseIntToken(token, &parsed)) {
    out->wifiApStations = parsed;
  }
  bool parsedEnvTime = false;
  if(detail::nextToken(frame, start, kFrameSeparator, &token) && detail::parseIntToken(token, &parsed)) {
    out->envTimeValid = parsed != 0;
    parsedEnvTime = true;
  }
  if(parsedEnvTime &&
     detail::nextToken(frame, start, kFrameSeparator, &token) &&
     detail::parseIntToken(token, &parsed)) {
    out->envWeatherValid = parsed != 0;
    out->envSyncFieldsPresent = true;
  }
  if(detail::nextToken(frame, start, kFrameSeparator, &token) && detail::parseIntToken(token, &parsed)) {
    out->burrGapEnabled = parsed != 0;
    out->burrGapFieldsPresent = true;
  }
  if(out->burrGapFieldsPresent &&
     detail::nextToken(frame, start, kFrameSeparator, &token) &&
     detail::parseIntToken(token, &parsed)) {
    out->burrGapKnown = parsed != 0;
  }
  if(out->burrGapFieldsPresent &&
     detail::nextToken(frame, start, kFrameSeparator, &token) &&
     detail::parseIntToken(token, &parsed)) {
    out->burrGapLive = parsed != 0;
  }
  if(out->burrGapFieldsPresent &&
     detail::nextToken(frame, start, kFrameSeparator, &token) &&
     detail::parseIntToken(token, &parsed)) {
    out->burrGapUm = parsed;
  }
  if(detail::nextToken(frame, start, kFrameSeparator, &token) && detail::parseIntToken(token, &parsed)) {
    out->controllerOtaReadyFieldsPresent = true;
    out->controllerOtaReady = parsed != 0;
  }
  return true;
}

inline bool parseMenuFrame(const String& frame, MenuFrame* out) {
  if(out == nullptr || !frame.startsWith(String(kFrameMenu) + kFrameSeparator)) {
    return false;
  }

  int start = String(kFrameMenu).length() + 1;
  String token;
  int32_t parsed = 0;
  if(!detail::nextToken(frame, start, kFrameSeparator, &token) || !detail::parseIntToken(token, &parsed)) {
    return false;
  }
  out->menuNum = static_cast<uint8_t>(parsed);
  out->count = 0;

  String valuesToken;
  if(!detail::nextToken(frame, start, kFrameSeparator, &valuesToken)) {
    valuesToken = "";
  }

  int valueCursor = 0;
  String valueToken;
  while(out->count < kMaxMenuValues && detail::nextToken(valuesToken, valueCursor, kListSeparator, &valueToken)) {
    if(valueToken.length() == 0) {
      continue;
    }
    if(!detail::parseIntToken(valueToken, &parsed)) {
      return false;
    }
    out->values[out->count++] = parsed;
  }
  return true;
}

inline bool parsePeerFrame(const String& frame, PeerFrame* out) {
  if(out == nullptr || !frame.startsWith(String(kFramePeer) + kFrameSeparator)) {
    return false;
  }

  int start = String(kFramePeer).length() + 1;
  String token;
  int32_t parsed = 0;

  if(!detail::nextToken(frame, start, kFrameSeparator, &out->name)) return false;
  if(!detail::nextToken(frame, start, kFrameSeparator, &out->mac)) return false;
  if(!detail::nextToken(frame, start, kFrameSeparator, &out->target)) return false;
  if(!detail::nextToken(frame, start, kFrameSeparator, &token) || !detail::parseIntToken(token, &parsed)) {
    return false;
  }
  out->channel = parsed;
  return true;
}

inline bool parseEnvironmentFrame(const String& frame, EnvironmentFrame* out) {
  if(out == nullptr || !frame.startsWith(String(kFrameEnvironment) + kFrameSeparator)) {
    return false;
  }

  int start = String(kFrameEnvironment).length() + 1;
  String token;
  int32_t parsed = 0;

  if(!detail::nextToken(frame, start, kFrameSeparator, &token) || !detail::parseIntToken(token, &parsed)) return false;
  out->timeValid = parsed != 0;
  if(!detail::nextToken(frame, start, kFrameSeparator, &token) || !detail::parseIntToken(token, &parsed)) return false;
  out->unixTime = static_cast<uint32_t>(parsed);
  if(!detail::nextToken(frame, start, kFrameSeparator, &token) || !detail::parseIntToken(token, &parsed)) return false;
  out->weatherValid = parsed != 0;
  if(!detail::nextToken(frame, start, kFrameSeparator, &token) || !detail::parseIntToken(token, &parsed)) return false;
  out->temperatureF = static_cast<int16_t>(parsed);
  if(!detail::nextToken(frame, start, kFrameSeparator, &token) || !detail::parseIntToken(token, &parsed)) return false;
  out->weatherCode = static_cast<uint16_t>(parsed);
  if(!detail::nextToken(frame, start, kFrameSeparator, &token) || !detail::parseIntToken(token, &parsed)) return false;
  out->precipitationPercent = static_cast<int8_t>(parsed);
  out->location = "";
  detail::nextToken(frame, start, kFrameSeparator, &out->location);
  return true;
}

inline bool parseWifiConfigFrame(const String& frame, WifiConfigFrame* out) {
  if(out == nullptr || !frame.startsWith(String(kFrameWifiConfig) + kFrameSeparator)) {
    return false;
  }

  int start = String(kFrameWifiConfig).length() + 1;
  String ssidToken;
  String passwordToken;
  if(!detail::nextToken(frame, start, kFrameSeparator, &ssidToken)) return false;
  if(!detail::nextToken(frame, start, kFrameSeparator, &passwordToken)) passwordToken = "";
  if(!detail::hexDecode(ssidToken, &out->ssid)) return false;
  if(!detail::hexDecode(passwordToken, &out->password)) return false;
  return out->ssid.length() > 0;
}

}  // namespace sg98_protocol
