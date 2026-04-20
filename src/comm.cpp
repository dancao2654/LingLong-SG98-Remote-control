#include "definitions.h"
#include "HardwareSerial.h"
#include "comm.h"  
#include "main.h"
#include "motor_hal.h"

unsigned long lastSend;
unsigned long lastReceived = 0;
uint16_t errorCode = 0;
uint8_t request[20] = {0};
uint8_t response[32] = {0};
uint16_t lastRead[20] = {0};
bool responseReceived = 0;
uint8_t lastRequestType = 0;
uint16_t crc = 0;
uint16_t lastRegAddr = 0;
CommProfile activeCommProfile = COMM_PROFILE_JMC;
JmcCommRate activeJmcRate = JMC_COMM_RATE_9600;

namespace {
constexpr uint32_t RS485_TURNAROUND_US = 50;
uint16_t sendDelayForProfile(CommProfile profile);
uint16_t receiveDelayForProfile(CommProfile profile);
uint16_t idleDelayForProfile(CommProfile profile);
uint16_t activeIntervalForProfile(CommProfile profile);

uint32_t baudRateForJmcRate(JmcCommRate rate) {
  switch (rate) {
    case JMC_COMM_RATE_57600:
      return 57600;
    case JMC_COMM_RATE_38400:
      return 38400;
    case JMC_COMM_RATE_19200:
      return 19200;
    case JMC_COMM_RATE_115200:
      return 115200;
    case JMC_COMM_RATE_9600:
    default:
      return COMM_BAUD_RATE_JMC;
  }
}

uint32_t baudRateForProfile(CommProfile profile) {
  switch (profile) {
    case COMM_PROFILE_JMC:
      return baudRateForJmcRate(activeJmcRate);
    case COMM_PROFILE_RT:
    default:
      return COMM_BAUD_RATE_RT;
  }
}

uint32_t serialConfigForProfile(CommProfile profile) {
  switch (profile) {
    case COMM_PROFILE_JMC:
      return SERIAL_8N1;
    case COMM_PROFILE_RT:
    default:
      return SERIAL_8N1;
  }
}

uint8_t expectedResponseLength() {
  if(lastRequestType == 0 || lastRequestType > 200) {
    return 8;
  }

  if(lastRequestType > 100 && lastRequestType < 200) {
    return 5 + ((lastRequestType - 100) * 2);
  }

  return 0;
}

bool frameReadyToRead(int availableBytes) {
  if(availableBytes <= 0) {
    return false;
  }

  const uint8_t expectedLen = expectedResponseLength();
  if(expectedLen > 0 && availableBytes >= expectedLen) {
    return true;
  }

  // Modbus exception replies are always 5 bytes. Give normal frames a brief
  // window to finish arriving before treating 5 bytes as an exception.
  if(availableBytes == 5 && millis() >= lastSend + receiveDelayForProfile(activeCommProfile) + 20) {
    return true;
  }

  return false;
}

int16_t waitForResponse(unsigned long timeoutMs) {
  const unsigned long deadline = millis() + timeoutMs;
  while(millis() < deadline) {
    const int16_t received = receive();
    if(received != 0) {
      return received;
    }
    delay(1);
  }
  return 0;
}

bool isJmcProfile() {
  return activeCommProfile == COMM_PROFILE_JMC;
}

uint16_t sendDelayForProfile(CommProfile profile) {
  if (profile != COMM_PROFILE_JMC) return COMM_DELAY_SEND;

  switch (activeJmcRate) {
    case JMC_COMM_RATE_115200: return 8;
    case JMC_COMM_RATE_57600: return 10;
    case JMC_COMM_RATE_38400: return 12;
    case JMC_COMM_RATE_19200: return 20;
    case JMC_COMM_RATE_9600:
    default:
      return 35;
  }
}

uint16_t receiveDelayForProfile(CommProfile profile) {
  if (profile != COMM_PROFILE_JMC) return COMM_DELAY_RECEIVE;

  switch (activeJmcRate) {
    case JMC_COMM_RATE_115200: return 6;
    case JMC_COMM_RATE_57600: return 7;
    case JMC_COMM_RATE_38400: return 8;
    case JMC_COMM_RATE_19200: return 12;
    case JMC_COMM_RATE_9600:
    default:
      return 25;
  }
}

uint16_t idleDelayForProfile(CommProfile profile) {
  if (profile != COMM_PROFILE_JMC) return COMM_DELAY_IDLE;

  switch (activeJmcRate) {
    case JMC_COMM_RATE_115200: return 140;
    case JMC_COMM_RATE_57600: return 160;
    case JMC_COMM_RATE_38400: return 180;
    case JMC_COMM_RATE_19200: return 250;
    case JMC_COMM_RATE_9600:
    default:
      return 400;
  }
}

uint16_t activeIntervalForProfile(CommProfile profile) {
  if (profile != COMM_PROFILE_JMC) return COMMINTERVAL;

  switch (activeJmcRate) {
    case JMC_COMM_RATE_115200: return 12;
    case JMC_COMM_RATE_57600: return 14;
    case JMC_COMM_RATE_38400: return 16;
    case JMC_COMM_RATE_19200: return 24;
    case JMC_COMM_RATE_9600:
    default:
      return 40;
  }
}

bool writeResponseMatches(int16_t received, uint16_t expectedReg) {
  if(received == 0) {
    return false;
  }

  if(received == 0x603F) {
    return false;
  }

  // The original JMC-only transport treated any parsed response as a
  // successful confirm step. Preserve that behavior for JMC so the shared
  // transport does not become stricter than the legacy firmware.
  if(isJmcProfile()) {
    return true;
  }

  return received == expectedReg;
}

bool readResponseMatches(int16_t received, uint16_t expectedReg) {
  if(received == 0 || received == 0x603F) {
    return false;
  }

  return received == expectedReg;
}

void debugSendFrame(const uint8_t* data, size_t length) {
  setTXmode();
  delayMicroseconds(RS485_TURNAROUND_US);
  Serial2.write(data, length);
  Serial2.flush();
  delayMicroseconds(RS485_TURNAROUND_US);
  setRXmode();
}

uint8_t debugExpectedReadLength(uint16_t regCount) {
  return static_cast<uint8_t>(5 + (regCount * 2));
}

bool debugCollectResponse(uint8_t* buffer,
                          uint8_t maxLen,
                          uint8_t expectedLen,
                          uint8_t* actualLen) {
  const unsigned long deadline = millis() + comm_get_receive_delay_ms() + 250;
  bool sawAnyBytes = false;
  unsigned long lastByteAt = 0;
  uint8_t len = 0;

  while(millis() < deadline) {
    while(Serial2.available() > 0 && len < maxLen) {
      buffer[len++] = static_cast<uint8_t>(Serial2.read());
      sawAnyBytes = true;
      lastByteAt = millis();
    }

    if(sawAnyBytes) {
      if(expectedLen > 0 && len >= expectedLen) {
        break;
      }
      if(lastByteAt > 0 && millis() - lastByteAt > 20) {
        break;
      }
    }
    delay(1);
  }

  *actualLen = len;
  return sawAnyBytes;
}

void debugInitTransaction(CommDebugTransaction* transaction) {
  if(transaction == nullptr) {
    return;
  }

  memset(transaction, 0, sizeof(*transaction));
  transaction->parsed_reg = -1;
  transaction->timed_out = true;
}

bool debugFinalizeRead(uint8_t slave,
                       uint16_t regAddr,
                       uint16_t regCount,
                       CommDebugTransaction* transaction) {
  if(transaction == nullptr) {
    return false;
  }

  if(transaction->response_len == 0) {
    transaction->timed_out = true;
    return false;
  }

  transaction->timed_out = false;

  if(transaction->response_len == 5 && (transaction->response[1] & 0x80U) != 0) {
    transaction->exception_code = transaction->response[2];
    transaction->response_crc_ok =
        modbus_crc16(transaction->response, 3) ==
        static_cast<uint16_t>(transaction->response[3] | (transaction->response[4] << 8));
    return false;
  }

  if(transaction->response_len < 5) {
    return false;
  }

  const uint16_t crcResp =
      static_cast<uint16_t>(transaction->response[transaction->response_len - 2] |
                            (transaction->response[transaction->response_len - 1] << 8));
  transaction->response_crc_ok =
      modbus_crc16(transaction->response, transaction->response_len - 2) == crcResp;
  if(!transaction->response_crc_ok) {
    return false;
  }

  if(transaction->response[0] != slave || transaction->response[1] != FUNC_READ_HOLDING_REGISTERS) {
    return false;
  }

  const uint8_t byteCount = transaction->response[2];
  if(byteCount != regCount * 2 || transaction->response_len < static_cast<uint8_t>(byteCount + 5)) {
    return false;
  }

  transaction->parsed_reg = regAddr;
  transaction->success = true;
  return true;
}

bool debugFinalizeWriteSingle(uint8_t slave,
                              CommDebugTransaction* transaction) {
  if(transaction == nullptr) {
    return false;
  }

  if(transaction->response_len == 0) {
    transaction->timed_out = true;
    return false;
  }

  transaction->timed_out = false;

  if(transaction->response_len == 5 && (transaction->response[1] & 0x80U) != 0) {
    transaction->exception_code = transaction->response[2];
    transaction->response_crc_ok =
        modbus_crc16(transaction->response, 3) ==
        static_cast<uint16_t>(transaction->response[3] | (transaction->response[4] << 8));
    return false;
  }

  if(transaction->response_len < 8) {
    return false;
  }

  const uint16_t crcResp =
      static_cast<uint16_t>(transaction->response[transaction->response_len - 2] |
                            (transaction->response[transaction->response_len - 1] << 8));
  transaction->response_crc_ok =
      modbus_crc16(transaction->response, transaction->response_len - 2) == crcResp;
  if(!transaction->response_crc_ok) {
    return false;
  }

  if(transaction->response[0] != slave || transaction->response[1] != FUNC_WRITE_SINGLE_REGISTER) {
    return false;
  }

  transaction->parsed_reg = static_cast<int16_t>((transaction->response[2] << 8) | transaction->response[3]);
  transaction->success = true;
  return true;
}

void debugApplyUart(uint32_t baudRate, uint32_t serialConfig) {
  Serial2.flush();
  Serial2.end();
  delay(2);
  Serial2.begin(baudRate, serialConfig, RS485_RXD, RS485_TXD);
  setRXmode();
  delay(2);
}
}

void comm_init() { 
  pinMode(RS485DE, OUTPUT);
  pinMode(RS485RE, OUTPUT);
  setRXmode();
  comm_set_profile(COMM_PROFILE_JMC, true);
  #ifdef DEBUG_COMM
    Serial.println("comm init");
  #endif
}

bool comm_set_profile(CommProfile profile, bool force) {
  if (!force && profile == activeCommProfile) return false;

  Serial2.flush();
  Serial2.end();
  delay(2);
  Serial2.begin(baudRateForProfile(profile), serialConfigForProfile(profile), RS485_RXD, RS485_TXD);
  setRXmode();
  delay(2);

  while (Serial2.available()) Serial2.read();
  memset(response, 0, sizeof(response));
  memset(lastRead, 0, sizeof(lastRead));
  errorCode = 0;
  lastRequestType = 0;
  lastRegAddr = 0;
  lastSend = 0;
  lastReceived = 0;
  activeCommProfile = profile;
  return true;
}

bool comm_set_jmc_rate(JmcCommRate rate, bool reapply_active) {
  if(!reapply_active && rate == activeJmcRate) {
    return false;
  }

  activeJmcRate = rate;
  if(reapply_active && activeCommProfile == COMM_PROFILE_JMC) {
    comm_set_profile(COMM_PROFILE_JMC, true);
  }
  return true;
}

JmcCommRate comm_get_jmc_rate() {
  return activeJmcRate;
}

uint32_t comm_get_jmc_baud() {
  return baudRateForJmcRate(activeJmcRate);
}

const char* comm_get_jmc_rate_label() {
  switch (activeJmcRate) {
    case JMC_COMM_RATE_57600:
      return "57600";
    case JMC_COMM_RATE_38400:
      return "38400";
    case JMC_COMM_RATE_19200:
      return "19200";
    case JMC_COMM_RATE_115200:
      return "115200";
    case JMC_COMM_RATE_9600:
    default:
      return "9600";
  }
}

uint16_t comm_get_send_delay_ms() {
  return sendDelayForProfile(activeCommProfile);
}

uint16_t comm_get_receive_delay_ms() {
  return receiveDelayForProfile(activeCommProfile);
}

uint16_t comm_get_idle_delay_ms() {
  return idleDelayForProfile(activeCommProfile);
}

uint16_t comm_get_active_interval_ms() {
  return activeIntervalForProfile(activeCommProfile);
}

CommProfile comm_get_profile() {
  return activeCommProfile;
}

const char* comm_get_profile_label() {
  switch (activeCommProfile) {
    case COMM_PROFILE_JMC:
      switch (activeJmcRate) {
        case JMC_COMM_RATE_57600:
          return "57600-8N1";
        case JMC_COMM_RATE_38400:
          return "38400-8N1";
        case JMC_COMM_RATE_19200:
          return "19200-8N1";
        case JMC_COMM_RATE_115200:
          return "115200-8N1";
        case JMC_COMM_RATE_9600:
        default:
          return "9600-8N1";
      }
    case COMM_PROFILE_RT:
    default:
      return "9600-8N1";
  }
}

bool writeConfirm(uint16_t reg_addr, uint16_t value) { 
  while(millis() - lastSend < comm_get_send_delay_ms()) {
    delay(1);
  }
  if(!writeSingleRegister(reg_addr, value)) {
    return false;
  }
  return writeResponseMatches(waitForResponse(comm_get_receive_delay_ms() + 150), reg_addr);
}

bool writeMultipleConfirm(uint16_t reg_addr_start, uint16_t reg_count, uint16_t data1, uint16_t data2, uint16_t data3, uint16_t data4) { 
  while(millis() - lastSend < comm_get_send_delay_ms()) {
    delay(1);
  }
  if(!writeMultipleRegisters(reg_addr_start, reg_count, data1, data2, data3, data4)) {
    return false;
  }
  return writeResponseMatches(waitForResponse(comm_get_receive_delay_ms() + 150), reg_addr_start);
}

int16_t readReturn(uint16_t reg_addr, uint16_t reg_count) { //returns 0xFFFF if unsuccesfull, otherwise returns read value of first reg_addr
  while(millis() - lastSend < comm_get_send_delay_ms()) {
    delay(1);
  }
  if(!readRegister(reg_addr, reg_count)) { //Send read request
    return 0xFFFF;
  }
  #ifdef DEBUG_COMM
    Serial.print("requesting: "), Serial.println(reg_addr);
  #endif
  const int16_t received = waitForResponse(comm_get_receive_delay_ms() + 150);
  if(readResponseMatches(received, reg_addr)) { 
    #ifdef DEBUG_COMM
    Serial.print("Readreturn, lastRead0: "), Serial.println(lastRead[0]);
    #endif
    return lastRead[0];
  } else return 0xFFFF;
}

void rs485_send(uint8_t* data, size_t length) {
  #ifdef DEBUG_COMM
  Serial.print("sending: ");
    for(int i = 0; i < length; i++) { 
      Serial.print(data[i], HEX); 
      Serial.print(" ");
    }
  Serial.println("<end>");
  #endif
  setTXmode();
  delayMicroseconds(RS485_TURNAROUND_US);
  Serial2.write(data, length);
  Serial2.flush();
  delayMicroseconds(RS485_TURNAROUND_US);
  setRXmode();
  lastSend = millis();
  commCounter++;
}

bool writeSingleRegister(uint16_t reg_addr, uint16_t value) { //returns true when succesfully SEND
  if(lastSend + comm_get_send_delay_ms() > millis()) { return false; }
  request[0] = SLAVE_ID;
  request[1] = FUNC_WRITE_SINGLE_REGISTER;
  request[2] = reg_addr >> 8;
  request[3] = reg_addr & 0xFF;
  request[4] = value >> 8;
  request[5] = value & 0xFF;
  crc = modbus_crc16(request, 6);
  request[6] = crc & 0xFF;
  request[7] = crc >> 8;

  while (Serial2.available()) Serial2.read();
  rs485_send(request, 8);

  lastSend = millis();
  lastRequestType = 0;
  return true;
}

bool writeMultipleRegisters(uint16_t reg_addr_start, uint16_t reg_count,
                           uint16_t data1, uint16_t data2,
                           uint16_t data3, uint16_t data4) {
    if (lastSend + comm_get_send_delay_ms() > millis()) return false;
    if (reg_count < 1 || reg_count > 4) return false;

    request[0] = SLAVE_ID;
    request[1] = FUNC_WRITE_MULTIPLE_REGISTERS;
    request[2] = reg_addr_start >> 8;
    request[3] = reg_addr_start & 0xFF;
    request[4] = reg_count >> 8;
    request[5] = reg_count & 0xFF;
    request[6] = reg_count * 2;

    uint16_t data[4] = { data1, data2, data3, data4 };
    for (uint8_t i = 0; i < reg_count; ++i) {
        request[7 + i * 2] = data[i] >> 8;
        request[8 + i * 2] = data[i] & 0xFF;
    }

    uint8_t msgLen = 7 + reg_count * 2;
    crc = modbus_crc16(request, msgLen);
    request[msgLen]     = crc & 0xFF;
    request[msgLen + 1] = crc >> 8;

    while (Serial2.available()) Serial2.read();
    rs485_send(request, msgLen + 2);

    lastSend = millis();
    lastRequestType = 200 + reg_count;
    return true;
}

bool receiveError(uint8_t msgLength) {
  // Serial2.readBytes(response, 5);

  //DEBUG COMM
  #ifdef DEBUG_COMM
  Serial.println("Fault code received!");
    Serial.print("Last req type: "), Serial.println(lastRequestType);
    Serial.print("Message: ");
    for(int i = 0; i < 5; i++) { 
      Serial.print(response[i], HEX);
      Serial.print(" ");
    }
    Serial.print(" < end > "), Serial.print("bytes: "), Serial.println(5);
  #endif

  if(msgLength == 5 && response[1] >= 0x80 && response[1] <= 0x90) { //error frame
    uint16_t crc_resp = response[3] | (response[4] << 8);
    if(crc_resp != modbus_crc16(response, 3)) return 0;

    errorCode = response[2];
    error = 3; 
    if(commCounter > 0) {
      commCounter--;
    }
    Serial.print("error received: ");
    Serial.println(errorCode);

    return 0x603F; //Error code cia402
  }
  return 0;
}

int16_t receive() {  // Returns 0 when nothing is read. returns the read register when succesfull receive(). 
  if(lastSend + comm_get_receive_delay_ms() > millis()) return 0; //exit if not expecting a complete message yet, faster and safer than checking serial.available();

  int len = Serial2.available();
  if(!frameReadyToRead(len)) {
    return 0;
  }

  if(len > static_cast<int>(sizeof(response))) {
    len = sizeof(response);
  }

  Serial2.readBytes(response, len);
  memset(lastRead, 0, sizeof(lastRead));

  //DEBUG COMM
  #ifdef DEBUG_COMM
    if(len > 2) {
      Serial.print("Last req type: "), Serial.println(lastRequestType);
      Serial.print("Message: ");
      for(int i = 0; i < len; i++) { 
        Serial.print(response[i], HEX);
        Serial.print(" ");
      }
      Serial.print(" < end > "), Serial.print("bytes: "), Serial.println(len);
    }
  #endif

  if(len == 5) return(receiveError(5)); //If we're between received and new send (delay)
  if(lastRequestType == 0 && len < 8) return 0;
  if(lastRequestType > 100 && lastRequestType < 200 && len < 7) return 0;
  if(lastRequestType > 200 && len < 8) return 0;
  // READ
  

  // >>>>>>>>>>>> Last comm request was a writeSingleReg
  if(lastRequestType == 0) { 
    uint16_t crc_resp = response[6] | (response[7] << 8);
    if (modbus_crc16(response, 6) != crc_resp) {
      #ifdef DEBUG_COMM
        Serial.println("CRC check failed0");
      #endif
      return false;
    } else { 
      #ifdef DEBUG_COMM
        Serial.println("CRC OK"); 
      #endif
      lastReceived = millis();
    }

    if(crc == crc_resp) { //SEND CRC matches received -> messages match -> Succes!
      commCounter--;
      lastRead[0] = (response[4] << 8) | response[5];
      return (response[2] << 8) | response[3];
    };

    #ifdef DEBUG_COMM
      Serial.println("Received data unexpected"); //Returned value is different that the set value
    #endif
    return false;
  }

  // >>>>>>>>>>>> Last send type was a data request (101 = 1 register)
  if (lastRequestType > 100 && lastRequestType < 200) {  

    // Validate CRC
    uint16_t crc_resp = response[len - 2] | (response[len - 1] << 8);
    if (modbus_crc16(response, len - 2) != crc_resp) {
      #ifdef DEBUG_COMM
        Serial.println("CRC failed 100+");
      #endif
      return 0;
    }

    // VALIDATE MSG 
    if (response[0] != SLAVE_ID || response[1] != FUNC_READ_HOLDING_REGISTERS) {
      #ifdef DEBUG_COMM
        Serial.print("Error: SLAVE ADDR:");
        Serial.println(response[0], HEX);
        Serial.print("Error: FUNCTION CODE");
        Serial.println(response[1], HEX);
      #endif
      return 0;
    }

    // SAVE DATA FROM RESPONSE
    memset(lastRead, 0, sizeof(lastRead));
    uint8_t byteCount = response[2];
    #ifdef DEBUG_COMM
      Serial.print("Registers read: ");
      Serial.println(byteCount / 2);
    #endif
    for (int i = 0; i < byteCount; i += 2) {
      lastRead[i/2] = (response[3 + i] << 8) | response[4 + i];
      #ifdef DEBUG_COMM
        Serial.print("Lastread #" + i/2), Serial.print(" : "), Serial.println(lastRead[i/2]);
      #endif
    if((lastRequestType-100)*2 != byteCount) lastRequestType = byteCount/2 + 100;
    }
    commCounter--;
    return lastRegAddr;
  }

  //  >>>>>>>>>>>> receive multiplewrites
  if(lastRequestType > 200) { 
    // Validate CRC
    uint16_t crc_resp = response[len - 2] | (response[len - 1] << 8);
    if (modbus_crc16(response, len - 2) != crc_resp) {
      #ifdef DEBUG_COMM
        Serial.println("Error: CRC failed>200");
      #endif
      return false;
    } else {
      #ifdef DEBUG_COMM
      Serial.println("CRC succes!");
      #endif
      lastReceived = millis();
    }

    // VALIDATE MSG 
    if (response[0] != SLAVE_ID || response[1] != FUNC_WRITE_MULTIPLE_REGISTERS) {
      #ifdef DEBUG_COMM
        Serial.print("Error: SLAVE ADDR:");
        Serial.println(response[0], HEX);
        Serial.print("Error: FUNCTION CODE");
        Serial.println(response[1], HEX);
      #endif
      return false;
    }

    // VALIDATE REG ADDR
    if(response[2] == request[2] && response[3] == request[3] && response[4] == request[4] && response[5] == request[5]) { 
      commCounter--;
      return (response[2] << 8) | response[3];
    } else {
        #ifdef DEBUG_COMM
          Serial.println("Response != request");
          Serial.print("Reg Requested: "), Serial.print(lastRegAddr), Serial.print("Reg Received: "), Serial.println((response[2] << 8) | response[3]);
      #endif 
    }
    return 0;
  }
  return 0;
}

bool readRegister(uint16_t reg_addr, uint16_t reg_count) { //returns true when succesfully SEND
  if(lastSend + comm_get_send_delay_ms() > millis()) { return false; }
  request[0] = SLAVE_ID;
  request[1] = FUNC_READ_HOLDING_REGISTERS;
  request[2] = reg_addr >> 8;
  request[3] = reg_addr & 0xFF;
  request[4] = reg_count >> 8;
  request[5] = reg_count & 0xFF;

  uint16_t crc = modbus_crc16(request, 6);
  request[6] = crc & 0xFF;
  request[7] = crc >> 8;

  // Clear previous data
  while (Serial2.available()) Serial2.read();

  rs485_send(request, 8);
  lastRequestType = 100 + reg_count;
  lastRegAddr = reg_addr;
  return true;
}

bool comm_debug_raw_read(CommProfile profile,
                         uint8_t slave,
                         uint16_t reg_addr,
                         uint16_t reg_count,
                         CommDebugTransaction* transaction) {
  return comm_debug_raw_read_uart(baudRateForProfile(profile),
                                  serialConfigForProfile(profile),
                                  slave,
                                  reg_addr,
                                  reg_count,
                                  transaction);
}

bool comm_debug_raw_write_single(CommProfile profile,
                                 uint8_t slave,
                                 uint16_t reg_addr,
                                 uint16_t value,
                                 CommDebugTransaction* transaction) {
  return comm_debug_raw_write_single_uart(baudRateForProfile(profile),
                                          serialConfigForProfile(profile),
                                          slave,
                                          reg_addr,
                                          value,
                                          transaction);
}

bool comm_debug_raw_read_uart(uint32_t baud_rate,
                              uint32_t serial_config,
                              uint8_t slave,
                              uint16_t reg_addr,
                              uint16_t reg_count,
                              CommDebugTransaction* transaction) {
  debugInitTransaction(transaction);
  if(transaction == nullptr || reg_count == 0 || reg_count > 10) {
    return false;
  }

  const CommProfile savedProfile = activeCommProfile;
  debugApplyUart(baud_rate, serial_config);
  delay(20);
  while(Serial2.available()) Serial2.read();

  transaction->request[0] = slave;
  transaction->request[1] = FUNC_READ_HOLDING_REGISTERS;
  transaction->request[2] = reg_addr >> 8;
  transaction->request[3] = reg_addr & 0xFF;
  transaction->request[4] = reg_count >> 8;
  transaction->request[5] = reg_count & 0xFF;
  const uint16_t crcReq = modbus_crc16(transaction->request, 6);
  transaction->request[6] = crcReq & 0xFF;
  transaction->request[7] = crcReq >> 8;
  transaction->request_len = 8;

  debugSendFrame(transaction->request, transaction->request_len);
  debugCollectResponse(transaction->response,
                       sizeof(transaction->response),
                       debugExpectedReadLength(reg_count),
                       &transaction->response_len);
  const bool success = debugFinalizeRead(slave, reg_addr, reg_count, transaction);

  while(Serial2.available()) Serial2.read();
  comm_set_profile(savedProfile, true);
  delay(20);
  return success;
}

bool comm_debug_raw_write_single_uart(uint32_t baud_rate,
                                      uint32_t serial_config,
                                      uint8_t slave,
                                      uint16_t reg_addr,
                                      uint16_t value,
                                      CommDebugTransaction* transaction) {
  debugInitTransaction(transaction);
  if(transaction == nullptr) {
    return false;
  }

  const CommProfile savedProfile = activeCommProfile;
  debugApplyUart(baud_rate, serial_config);
  delay(20);
  while(Serial2.available()) Serial2.read();

  transaction->request[0] = slave;
  transaction->request[1] = FUNC_WRITE_SINGLE_REGISTER;
  transaction->request[2] = reg_addr >> 8;
  transaction->request[3] = reg_addr & 0xFF;
  transaction->request[4] = value >> 8;
  transaction->request[5] = value & 0xFF;
  const uint16_t crcReq = modbus_crc16(transaction->request, 6);
  transaction->request[6] = crcReq & 0xFF;
  transaction->request[7] = crcReq >> 8;
  transaction->request_len = 8;

  debugSendFrame(transaction->request, transaction->request_len);
  debugCollectResponse(transaction->response,
                       sizeof(transaction->response),
                       8,
                       &transaction->response_len);
  const bool success = debugFinalizeWriteSingle(slave, transaction);

  while(Serial2.available()) Serial2.read();
  comm_set_profile(savedProfile, true);
  delay(20);
  return success;
}

// ---- MODBUS CRC16 ----
uint16_t modbus_crc16(uint8_t* buf, int len) {
  uint16_t crc = 0xFFFF;
  for (int pos = 0; pos < len; pos++) {
    crc ^= (uint16_t)buf[pos];
    for (int i = 0; i < 8; i++) {
      if (crc & 0x0001) {
        crc >>= 1;
        crc ^= 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}
