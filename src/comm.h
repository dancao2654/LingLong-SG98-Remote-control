#include "definitions.h"

#ifndef comm_h

#define COMM_BAUD_RATE_JMC 9600
#define COMM_BAUD_RATE_RT 9600

#define comm_h
#define SLAVE_ID 0x01
#define FUNC_READ_HOLDING_REGISTERS 0x03
#define FUNC_WRITE_SINGLE_REGISTER 0x06
#define FUNC_WRITE_MULTIPLE_REGISTERS 0x10

enum CommProfile {
  COMM_PROFILE_RT,
  COMM_PROFILE_JMC,
};

enum JmcCommRate {
  JMC_COMM_RATE_9600,
  JMC_COMM_RATE_19200,
  JMC_COMM_RATE_38400,
  JMC_COMM_RATE_57600,
  JMC_COMM_RATE_115200,
};

struct CommDebugTransaction {
  uint8_t request[20];
  uint8_t response[32];
  uint8_t request_len;
  uint8_t response_len;
  uint8_t exception_code;
  bool response_crc_ok;
  bool timed_out;
  bool success;
  int16_t parsed_reg;
};

void comm_init(); //init RS485 communication
bool comm_set_profile(CommProfile profile, bool force = false);
CommProfile comm_get_profile();
const char* comm_get_profile_label();
bool comm_set_jmc_rate(JmcCommRate rate, bool reapply_active = false);
JmcCommRate comm_get_jmc_rate();
uint32_t comm_get_jmc_baud();
const char* comm_get_jmc_rate_label();
uint16_t comm_get_send_delay_ms();
uint16_t comm_get_receive_delay_ms();
uint16_t comm_get_idle_delay_ms();
uint16_t comm_get_active_interval_ms();

bool writeConfirm(uint16_t reg_addr, uint16_t value);
int16_t readReturn(uint16_t reg_addr, uint16_t reg_count);
bool writeMultipleConfirm(uint16_t reg_addr_start, uint16_t reg_count, uint16_t data1, uint16_t data2=0, uint16_t data3=0, uint16_t data4=0);

bool writeSingleRegister(uint16_t reg_addr, uint16_t value);
bool writeMultipleRegisters(uint16_t reg_addr_start, uint16_t reg_count, uint16_t data1, uint16_t data2=0xFF, uint16_t data3=0xFF, uint16_t data4=0xFF);
bool readRegister(uint16_t reg_addr, uint16_t reg_count);
bool comm_debug_raw_read(CommProfile profile, uint8_t slave, uint16_t reg_addr, uint16_t reg_count, CommDebugTransaction* transaction);
bool comm_debug_raw_write_single(CommProfile profile, uint8_t slave, uint16_t reg_addr, uint16_t value, CommDebugTransaction* transaction);
bool comm_debug_raw_read_uart(uint32_t baud_rate, uint32_t serial_config, uint8_t slave, uint16_t reg_addr, uint16_t reg_count, CommDebugTransaction* transaction);
bool comm_debug_raw_write_single_uart(uint32_t baud_rate, uint32_t serial_config, uint8_t slave, uint16_t reg_addr, uint16_t value, CommDebugTransaction* transaction);

void rs485_poll();

extern int16_t receive(); 
extern uint16_t lastRead[]; 
extern uint8_t lastRequestType; //0 = writeSingle, 101 = readSingleRegister, 102 = read two registers etc, 201 writeMultiperRg(1pc), 202 writeMultipleReg(2pcs)
extern uint16_t errorCode; 
extern unsigned long lastReceived; //used for connection states;
extern uint16_t lastRegAddr;
extern bool rs485Sending;


uint16_t modbus_crc16(uint8_t* buf, int length);
bool receiveError(uint8_t msgLength); 

void setTXmode();
void setRXmode();

#endif
