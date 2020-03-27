// ZeroWi bare-metal WiFi driver, see https://iosoft.blog/zerowi
// Raspberry Pi SDIO interface definitions
//
// Copyright (c) 2020 Jeremy P Bentham
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#define LOG_CMDS     1
#define LOG_ALL      2

// I/O pin numbers
#define LED_PIN      47
#define SD_CLK_PIN   34
#define SD_CMD_PIN   35
#define WLAN_ON_PIN  41
#define SD_32KHZ_PIN 43

#define SD_D0_PIN    36
#define SD_D1_PIN    37
#define SD_D2_PIN    38
#define SD_D3_PIN    39
#define SD_DATA_PINS 4

// 32.768 kHz oscillator
#define GP2CTL          ((uint32_t *)0x20101080)
#define GP2DIV          (GP2CTL + 0x4/4)
#define GP2CTL_VAL      (0x5a000000 + 0x291)
#define GP2DIV_VAL      (0x5a000000 + 0x00249F00)

// CRC polynomials
#define CRC7_POLY    (uint8_t)(0b10001001 << 1)
#define CRC16R_POLY  (1<<(15-0) | 1<<(15-5) | 1<<(15-12))

// Bit counts
#define BLOCK_ACK_BITS  8
#define MSG_BITS        48
#define BYTE_BITS       8
#define MSG_BYTES       (MSG_BITS / BYTE_BITS)

// Read/write block sizes
#define SD_BAK_BLK_BYTES    64
#define SD_RAD_BLK_BYTES    512

// SD function numbers
#define SD_FUNC_BUS     0
#define SD_FUNC_BAK     1
#define SD_FUNC_RAD     2

// Read/write flags
#define SD_RD           0
#define SD_WR           1

// Delays
#define SD_CLK_DELAY    1   // Clock on/off time in usec
#define RSP_WAIT        20  // Number of clock cycles to wait for resp

// Macros to reorder items in structure
#define BITF1(typ, a)             typ a
#define BITF2(typ, a, b)          typ b, a
#define BITF3(typ, a, b, c)       typ c, b, a
#define BITF4(typ, a, b, c, d)    typ d, c, b, a
#define BITF5(typ, a, b, c, d, e) typ e, d, c, b, a

// Structures for SDIO communication
#pragma pack(1)
typedef struct
{
    BITF3(uint8_t,  start:1, cmd:1, num:6);
    BITF1(uint32_t, argx);
    BITF2(uint8_t,  crc:7,   stop:1);
} SDIO_MSG_STRUCT;

typedef struct
{
    BITF3(uint8_t,  start:1, cmd:1,  num:6);
    BITF1(uint16_t, rcax);
    BITF1(uint16_t, statusx);
    BITF2(uint8_t,  crc:7,   stop:1);
} SDIO_RSP3_STRUCT;

typedef struct
{
    BITF3(uint8_t,  start:1, cmd:1, x1:6);
    BITF5(uint8_t,  rdy:1, nio:3, mem:1, x2:2, s18a:1);
    BITF1(uint8_t,  ocrl);
    BITF1(uint8_t,  ocrm);
    BITF1(uint8_t,  ocrh);
    BITF2(uint8_t,  x3:7,   stop:1);
} SDIO_RSP5_STRUCT;

typedef struct
{
    BITF3(uint8_t,  start:1, cmd:1,  num:6);
    BITF1(uint16_t, rcax);
    BITF1(uint16_t, x1);
    BITF2(uint8_t,  crc:7,   stop:1);
} SDIO_CMD7_STRUCT;

typedef struct
{
    BITF3(uint8_t,  start:1, cmd:1,   num:6);
    BITF5(uint8_t,  wr:1,    func:3,  raw:1, x1:1, addrh:2);
    BITF1(uint8_t,  addrm);
    BITF2(uint8_t,  addrl:7, x2:1);
    BITF1(uint8_t,  data);
    BITF2(uint8_t,  crc:7,   stop:1);
} SDIO_CMD52_STRUCT;

typedef struct
{
    BITF3(uint8_t,  start:1, cmd:1,  num:6);
    BITF1(uint16_t, x1);
    BITF1(uint8_t,  flags);
    BITF1(uint8_t,  data);
    BITF2(uint8_t,  crc:7,   stop:1);
} SDIO_RSP52_STRUCT;

typedef struct
{
    BITF3(uint8_t,  start:1, cmd:1,   num:6);
    BITF5(uint8_t,  wr:1,    func:3,  blk:1, inc:1, addrh:2);
    BITF1(uint8_t,  addrm);
    BITF2(uint8_t,  addrl:7, lenh:1);
    BITF1(uint8_t,  lenl);
    BITF2(uint8_t,  crc:7,   stop:1);
} SDIO_CMD53_STRUCT;

typedef union 
{
    SDIO_MSG_STRUCT     msg;
    SDIO_RSP3_STRUCT    rsp3;
    SDIO_CMD7_STRUCT    cmd7;
    SDIO_CMD52_STRUCT   cmd52;
    SDIO_RSP52_STRUCT   rsp52;
    SDIO_CMD53_STRUCT   cmd53;
    uint8_t             data[MSG_BYTES+2];
} SDIO_MSG;

// Union to handle 8/16/32 bit conversions
typedef union
{
    uint32_t uint32;
    uint32_t uint24:24;
    uint16_t uint16;
    uint8_t  uint8;
    uint8_t  bytes[4];
} U32DATA;

// Tags for logged messages
#define LOG_ERROR       0xff
#define LOG_DATA_ACK    0xfe
#define LOG_SIZE        50
#define LOG_DATA_LEN    6

// Miscellaneous macros
#define BYTES(d) (uint8_t *)(d)
#define REG_ADDR(m) (m.addrl | m.addrm<<7 | m.addrh<<15)
#define SWAP16(x) ((x&0xff)<<8 | (x&0xff00)>>8)
#define SWAP32(x) ((x&0xff)<<24 | (x&0xff00)<<8 | (x&0xff0000)>>8 | (x&0xff000000)>>24)
#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

void sdio_bak_window(uint32_t addr);
uint32_t sdio_bak_addr(uint32_t addr);
int sdio_cmd7(int rca, SDIO_MSG *rsp);
int sdio_write_blocks(int func, int addr, uint8_t *dp, int nblocks);
int sdio_bak_write32(uint32_t addr, uint32_t val);
int sdio_bak_read32(uint32_t addr, uint32_t *valp);
int sdio_cmd53_write(int func, int addr, uint8_t *dp, int nbytes);
int sdio_cmd53_read(int func, int addr, uint8_t *dp, int nbytes);
int sdio_cmd52_reads_check(int func, int addr, uint32_t mask, uint32_t val, int nbytes);
int sdio_cmd52_writes(int func, int addr, uint32_t data, int nbytes);
int sdio_cmd52_reads(int func, int addr, uint32_t *dp, int nbytes);
int sdio_cmd52(int func, int addr, uint8_t data, int wr, int raw, SDIO_MSG *rsp);
int sdio_cmd(int num, uint32_t arg, SDIO_MSG *rsp);
int sdio_cmd_rsp(SDIO_MSG *cmdp, SDIO_MSG *rsp);
void sdio_cmd_write(uint8_t *data, int nbits);
int sdio_rsp_block_write(uint8_t *rsp, uint8_t *dp, int nbytes);
void sdio_block_out(uint8_t *dp, int nbytes);
int sdio_rsp_block_read(uint8_t *rspd, uint8_t *data, int nbytes, uint64_t *crcp);
int sdio_rsp_read(uint8_t *rsp, int nbits, int pin);
void clk_0(int cycles);
void crc7_init(void);
void add_crc7(uint8_t *data);
uint8_t crc7_byte(uint8_t b);
uint8_t crc7_data(uint8_t *data, int n);
void usdelay(int usec);
int ustimeout(int *tickp, int usec);
void qcrc16r_init(void);
uint64_t quadval(uint16_t val);
void disp_msg(SDIO_MSG *smf);
void disp_cmd52(SDIO_MSG *smf);
void disp_rsp52(SDIO_MSG *smf);
void disp_cmd53(SDIO_MSG *smf);
void disp_rsp53(SDIO_MSG *smf);
void log_enable(int on);
void log_incr(void);
void log_msg(SDIO_MSG *msgp);
void log_data(uint8_t *data, int len, int ok);
void log_data_ack(uint8_t val);
void log_error(uint8_t *data, int len);
void disp_log_break(void);
void disp_log(void);
void dump_msg(uint8_t *data);
void osc_init(void);

// EOF

