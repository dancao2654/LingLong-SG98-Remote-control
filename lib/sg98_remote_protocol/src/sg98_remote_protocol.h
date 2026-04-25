#pragma once

#include <Arduino.h>

namespace sg98_protocol {

constexpr char kFrameSeparator = '|';
constexpr char kListSeparator = ',';

constexpr const char* kFrameAck = "ACK";
constexpr const char* kFrameStatus = "STAT";
constexpr const char* kFrameMenu = "MENU";
constexpr const char* kFramePeer = "PEER";

constexpr const char* kCmdStart = "START";
constexpr const char* kCmdStop = "STOP";
constexpr const char* kCmdIdle = "IDLE";
constexpr const char* kCmdIdleGbw = "IDLE_GBW";
constexpr const char* kCmdGetStatus = "GET_STATUS";
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

}  // namespace sg98_protocol
