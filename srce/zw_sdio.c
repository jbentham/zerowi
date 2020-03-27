// ZeroWi bare-metal WiFi driver, see https://iosoft.blog/zerowi
// Raspberry Pi SDIO interface
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

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "zw_gpio.h"
#include "zw_sdio.h"
#include "zw_regs.h"

// Log buffer
SDIO_MSG msglog[LOG_SIZE];
int log_idx, log_start, logging;

// CRC tables
uint64_t qcrc16r_poly, qcrc16r_table[1 << SD_DATA_PINS];
uint8_t crc7_table[256], clkval;

void gdb_break(void);

// Read & check a value
int sdio_cmd52_reads_check(int func, int addr, uint32_t mask, uint32_t val, int nbytes)
{
    U32DATA data;

    return(sdio_cmd52_reads(func, addr, &data.uint32, nbytes) ? (data.uint32 & mask) == val: 0);
}

// Send SD command 7 to select chip using RCA, get response
int sdio_cmd7(int rca, SDIO_MSG *rsp)
{
    SDIO_MSG cmd={.cmd7 = {.start=0, .cmd=1, .num=7, 
        .rcax=SWAP16(rca), .x1=0, .crc=0, .stop=1}};

    return(sdio_cmd_rsp(&cmd, rsp));
}

// Write multiple 64-byte command 53 blocks (max 32K in total)
int sdio_write_blocks(int func, int addr, uint8_t *dp, int nblocks)
{
    int n=0;
    SDIO_MSG rspx, cmd={.cmd53 = {.start=0, .cmd=1, .num=53,
        .wr=1, .func=func, .blk=1, .inc=1, .addrh=(uint8_t)(addr>>15)&3,
        .addrm=(uint8_t)(addr>>7), .addrl=(uint8_t)(addr&0x7f),
        .lenh=(uint8_t)(nblocks>>8)&1, .lenl=(uint8_t)nblocks, .crc=0, .stop=1}};

    clk_0(1);
    add_crc7(cmd.data);
    log_msg(&cmd);
    sdio_cmd_write(cmd.data, MSG_BITS);
    if (sdio_rsp_read(rspx.data, MSG_BITS, SD_CMD_PIN))
    {
        log_msg(&rspx);
        gpio_write(SD_D0_PIN, 4, 0xf);
        gpio_mode(SD_D0_PIN, GPIO_OUT);
        gpio_mode(SD_D1_PIN, GPIO_OUT);
        gpio_mode(SD_D2_PIN, GPIO_OUT);
        gpio_mode(SD_D3_PIN, GPIO_OUT);
        while (n++ < nblocks)
        {
            sdio_block_out(dp, SD_BAK_BLK_BYTES);
            log_data(dp, SD_BAK_BLK_BYTES, 1);
            sdio_rsp_read(rspx.data, BLOCK_ACK_BITS, SD_D0_PIN);
            log_data_ack(rspx.data[0]);
            dp += SD_BAK_BLK_BYTES;
            clk_0(2);
        }
        gpio_mode(SD_D0_PIN, GPIO_IN);
        gpio_mode(SD_D1_PIN, GPIO_IN);
        gpio_mode(SD_D2_PIN, GPIO_IN);
        gpio_mode(SD_D3_PIN, GPIO_IN);
    }
    clk_0(1);
    return(n);
}

// Set backplane window, don't set if already OK
void sdio_bak_window(uint32_t addr)
{
    static uint32_t lastaddr=0;
    
    addr &= SB_WIN_MASK;
    if (addr != lastaddr)
        sdio_cmd52_writes(SD_FUNC_BAK, BAK_WIN_ADDR_REG, addr>>8, 3);
    lastaddr = addr;
}

// Set backplane window, and return offset within window
uint32_t sdio_bak_addr(uint32_t addr)
{
    sdio_bak_window(addr);
    return(addr & SB_ADDR_MASK);
}

// Write a 32-bit value via the backplane window
int sdio_bak_write32(uint32_t addr, uint32_t val)
{
    U32DATA u32d={.uint32=val};

    sdio_bak_window(addr);
    return(sdio_cmd53_write(SD_FUNC_BAK, addr | SB_32BIT_WIN, u32d.bytes, 4));
}

// Read a 32-bit value via the backplane window
int sdio_bak_read32(uint32_t addr, uint32_t *valp)
{
    U32DATA u32d;
    int n;

    sdio_bak_window(addr);
    n = sdio_cmd53_read(SD_FUNC_BAK, addr | SB_32BIT_WIN, u32d.bytes, 4);
    *valp = u32d.uint32;
    return(n);
}

// Do a command 53 block write
int sdio_cmd53_write(int func, int addr, uint8_t *dp, int nbytes)
{
    SDIO_MSG rspx, cmd={.cmd53 = {.start=0, .cmd=1, .num=53,
        .wr=1, .func=func, .blk=0, .inc=1, .addrh=(uint8_t)(addr>>15)&3,
        .addrm=(uint8_t)(addr>>7), .addrl=(uint8_t)(addr&0x7f),
        .lenh=(uint8_t)(nbytes>>8)&1, .lenl=(uint8_t)nbytes, .crc=0, .stop=1}};
    int n;

    clk_0(2);
    add_crc7(cmd.data);
    log_msg(&cmd);
    sdio_cmd_write(cmd.data, MSG_BITS);
    n = sdio_rsp_block_write(rspx.data, dp, nbytes);
    clk_0(16);
    log_msg(&rspx);
    log_data(dp, n, 1);
    return(n);
}

// Do a command 53 block read
int sdio_cmd53_read(int func, int addr, uint8_t *dp, int nbytes)
{
    SDIO_MSG rspx, cmd={.cmd53 = {.start=0, .cmd=1, .num=53,
        .wr=0, .func=func, .blk=0, .inc=1, .addrh=(uint8_t)(addr>>15)&3,
        .addrm=(uint8_t)(addr>>7), .addrl=(uint8_t)(addr&0x7f),
        .lenh=(uint8_t)(nbytes>>8)&1, .lenl=(uint8_t)nbytes, .crc=0, .stop=1}};
    int n;
    uint64_t crc;

    clk_0(2);
    add_crc7(cmd.data);
    log_msg(&cmd);
    sdio_cmd_write(cmd.data, MSG_BITS);
    n = sdio_rsp_block_read(rspx.data, dp, nbytes, &crc);
    clk_0(1);
    log_msg(&rspx);
    log_data(dp, n, crc==0);
    return(n);
}

// Do 1 - 4 CMD52 writes to successive addresses
int sdio_cmd52_writes(int func, int addr, uint32_t data, int nbytes)
{
    int n=0;

    while (nbytes--)
    {
        n += sdio_cmd52(func, addr++, (uint8_t)data, SD_WR, 0, 0);
        data >>= 8;
    }
    return(n);
}

// Do 1 - 4 CMD52 reads from successive addresses
int sdio_cmd52_reads(int func, int addr, uint32_t *dp, int nbytes)
{
    int i, n=0;
    uint32_t val=0;
    SDIO_MSG rspx; 

    for (i=0; i<nbytes; i++)
    {
        n += sdio_cmd52(func, addr++, 0, SD_RD, 0, &rspx);
        val |= (uint32_t)rspx.rsp52.data << i*8;
    }
    *dp = val;
    return(n);
}

// Send SDIO command 52, get response, return 0 if none
int sdio_cmd52(int func, int addr, uint8_t data, int wr, int raw, SDIO_MSG *rsp)
{
    SDIO_MSG cmd={.cmd52 = {.start=0, .cmd=1, .num=52,
        .wr=wr, .func=func, .raw=raw, .x1=0, .addrh=(uint8_t)(addr>>15 & 3),
        .addrm=(uint8_t)(addr>>7 & 0xff), .addrl=(uint8_t)(addr&0x7f), .x2=0,
        .data=data, .crc=0, .stop=1}};

    return(sdio_cmd_rsp(&cmd, rsp));
}

// Send SD command, get response, return 0 if none
int sdio_cmd(int num, uint32_t arg, SDIO_MSG *rsp)
{
    SDIO_MSG cmd={.msg = {.start=0, .cmd=1, .num=num, 
        .argx=SWAP32(arg), .crc=0, .stop=1}};

    return(sdio_cmd_rsp(&cmd, rsp));
}

// Send SD command, return response length in bits
int sdio_cmd_rsp(SDIO_MSG *cmdp, SDIO_MSG *rsp)
{
    SDIO_MSG rspx; 
    int n;

    clk_0(2);
    add_crc7(cmdp->data);
    rsp = rsp ? rsp : &rspx;
    log_msg(cmdp);
    sdio_cmd_write(cmdp->data, MSG_BITS);
    memset(rsp->data, 0, MSG_BYTES);
    n = sdio_rsp_read(rsp->data, MSG_BITS, SD_CMD_PIN);
    log_msg(rsp);
    return(n);
}

// Write command to SD interface
void sdio_cmd_write(uint8_t *data, int nbits)
{
   uint8_t b, n;

    gpio_mode(SD_CMD_PIN, GPIO_OUT);
    for (n=0; n<nbits; n++)
    {
        if (n%8 == 0)
            b = *data++;
        gpio_out(SD_CMD_PIN, b & 0x80);
        b <<= 1;
        usdelay(SD_CLK_DELAY);
        gpio_out(SD_CLK_PIN, 1);
        usdelay(SD_CLK_DELAY);
        gpio_out(SD_CLK_PIN, 0);
    }
    gpio_mode(SD_CMD_PIN, GPIO_IN);
}

// Return response from chip
int sdio_rsp_read(uint8_t *rsp, int nbits, int pin)
{
    uint8_t wt=RSP_WAIT, n=0, r=1;

    *rsp = 0;
    while (wt-- && r)
    {
        usdelay(SD_CLK_DELAY);
        gpio_out(SD_CLK_PIN, 1);
        r = gpio_in(pin);
        usdelay(SD_CLK_DELAY);
        gpio_out(SD_CLK_PIN, 0);
    }
    if (r == 0)
    {
        for (n=1; n<nbits; n++)
        {
            if (n%8 == 0)
                *++rsp = 0;
            usdelay(SD_CLK_DELAY);
            gpio_out(SD_CLK_PIN, 1);
            *rsp = (*rsp << 1) | gpio_in(pin);
            usdelay(SD_CLK_DELAY);
            gpio_out(SD_CLK_PIN, 0);
        }
    }
    return(n);
}

// Return response from a command 53 block write
int sdio_rsp_block_write(uint8_t *rsp, uint8_t *dp, int nbytes)
{
    if (sdio_rsp_read(rsp, MSG_BITS, SD_CMD_PIN))
    {
        clk_0(1);
        gpio_write(SD_D0_PIN, 4, 0xff);
        gpio_mode(SD_D0_PIN, GPIO_OUT);
        gpio_mode(SD_D1_PIN, GPIO_OUT);
        gpio_mode(SD_D2_PIN, GPIO_OUT);
        gpio_mode(SD_D3_PIN, GPIO_OUT);
        sdio_block_out(dp, nbytes);
        gpio_mode(SD_D0_PIN, GPIO_IN);
        gpio_mode(SD_D1_PIN, GPIO_IN);
        gpio_mode(SD_D2_PIN, GPIO_IN);
        gpio_mode(SD_D3_PIN, GPIO_IN);
    }
    else
        nbytes = 0;
    clk_0(1);
    return(nbytes);
}

// Send block of data with CRC
// (assumes command 53 sent, response received, and O/P set)
void sdio_block_out(uint8_t *dp, int nbytes)
{
    int dbits=0, n;
    uint64_t qcrc=0;
    uint8_t d;

    clk_0(1);
    gpio_write(SD_D0_PIN, 4, 0);
    clk_0(1);
    while (dbits < nbytes*8)
    {
        d = dbits%8 ? dp[dbits/8] & 0xf : dp[dbits/8] >> 4;
        gpio_write(SD_D0_PIN, 4, d);
        gpio_out(SD_CLK_PIN, 1);
        //clk_0(1);
        qcrc = qcrc >> SD_DATA_PINS ^ qcrc16r_table[(d ^ (uint8_t)qcrc) & 0xf];
        dbits += 4;
        gpio_out(SD_CLK_PIN, 0);
    }
    for (n=0; n<16; n++)
    {
        gpio_write(SD_D0_PIN, 4, (uint8_t)qcrc & 0xf);
        gpio_out(SD_CLK_PIN, 1);
        qcrc >>= 4;
        //clk_0(1);
        gpio_out(SD_CLK_PIN, 0);
    }
    gpio_write(SD_D0_PIN, 4, 0xf);
    clk_0(1);
}

// Return response & data block from a command 53 read
int sdio_rsp_block_read(uint8_t *rsp, uint8_t *dp, int nbytes, uint64_t *crcp)
{
    int wt=RSP_WAIT, rbits=1, dbits=0, din=0;
    uint8_t r=1, d=0xf;
    uint64_t qcrc=0;

    *rsp = 0;
    if (dp)
        *dp = 0;
    while (wt-- && r)
    {
        usdelay(SD_CLK_DELAY);
        gpio_out(SD_CLK_PIN, 1);
        r = gpio_in(SD_CMD_PIN);
        usdelay(SD_CLK_DELAY);
        gpio_out(SD_CLK_PIN, 0);
    }
    if (r == 0)
    {
        while (rbits<MSG_BITS || din)
        {
            usdelay(SD_CLK_DELAY);
            gpio_out(SD_CLK_PIN, 1);
            if (rbits < MSG_BITS)
            {
                if (rbits % 8 == 0)
                    *++rsp = 0;
                *rsp = (*rsp<<1) | gpio_in(SD_CMD_PIN);
                rbits++;
            }
            if (din==0 && gpio_read(SD_D0_PIN, SD_DATA_PINS)==0)
                din = 1;
            else if (din)
            {
                d = gpio_read(SD_D0_PIN, SD_DATA_PINS);
                if (dp && dbits/8 < nbytes)
                    *dp = (*dp << SD_DATA_PINS) | d;
                qcrc = qcrc >> SD_DATA_PINS ^ qcrc16r_table[(d ^ (uint8_t)qcrc) & 0xf];
                dbits += SD_DATA_PINS;
                if (dbits/8 >= nbytes + SD_DATA_PINS*2)
                    din = 0;
                else if (dp && dbits/8 < nbytes && dbits%8 == 0)
                    *++dp = 0;
            }
            usdelay(SD_CLK_DELAY);
            gpio_out(SD_CLK_PIN, 0);
        }
    }
    *crcp = qcrc;
    dbits -= SD_DATA_PINS*2*8;
    return(dbits>0 ? dbits/8 : 0);
}

// Toggle clock, leave it at 0
void clk_0(int cycles)
{
    while (cycles--)
    {
        usdelay(SD_CLK_DELAY);
        gpio_out(SD_CLK_PIN, clkval=!clkval);
        usdelay(SD_CLK_DELAY);
        gpio_out(SD_CLK_PIN, clkval=!clkval);
    }
    if (clkval)
    {
        usdelay(SD_CLK_DELAY);
        gpio_out(SD_CLK_PIN, clkval=!clkval);
    }

}

// Initialise CRC7 calculator
void crc7_init(void)
{
    int i;

    for (i=0; i<256; i++)
        crc7_table[i] = crc7_byte(i);
}

// Add CRC and stop bit to message
void add_crc7(uint8_t *data)
{
    data[MSG_BYTES-1] = crc7_data(data, MSG_BYTES-1);
}

// Calculate 7-bit CRC of byte, return as bits 1-7
uint8_t crc7_byte(uint8_t b)
{
    uint16_t n, w=b;

    for (n=0; n<8; n++)
    {
        w <<= 1;
        if (w & 0x100)
            w ^= CRC7_POLY;
    }
    return((uint8_t)w);
}

// Calculate 7-bit CRC of data bytes, with l.s.bit as stop bit
uint8_t crc7_data(uint8_t *data, int n)
{
    uint8_t crc=0;

    while (n--)
        crc = crc7_table[crc ^ *data++];
    return(crc | 1);
}

// Dump data as byte values
void disp_bytes(uint8_t *data, int len)
{
    while (len--)
        printf("%02x ", *data++);
}

// Dump message
void dump_msg(uint8_t *data)
{
    uint8_t i, crc=crc7_data(data, MSG_BYTES-1);

    for (i=0; i<MSG_BYTES; i++)
        printf("%02x ", data[i]);
    printf(crc == data[MSG_BYTES-1] ? "*" : "?");
    printf("\n");
}

// Initialise bit-reversed CRC16 lookup table for 4-bit values
void qcrc16r_init(void)
{
    qcrc16r_poly = quadval(CRC16R_POLY);
    int i;

    for (i=0; i<(1<<SD_DATA_PINS); i++)
        qcrc16r_table[i]  = (i & 8 ? qcrc16r_poly<<3 : 0) |
                            (i & 4 ? qcrc16r_poly<<2 : 0) |
                            (i & 2 ? qcrc16r_poly<<1 : 0) |
                            (i & 1 ? qcrc16r_poly<<0 : 0);
}

// Spread a 16-bit value to occupy 64 bits
uint64_t quadval(uint16_t val)
{
    uint64_t ret=0;
    int i;

    for (i=0; i<16; i++)
        ret |= val & (1<<i) ? 1LL<<(i*4) : 0;
    return(ret);
}

// Display command or response
void disp_msg(SDIO_MSG *smf)
{
    uint8_t crc = crc7_data(smf->data, MSG_BYTES-1);

    disp_bytes(smf->data, MSG_BYTES);
    printf("%s ", smf->data[MSG_BYTES-1] == crc ? "*" : "?");
    printf("%s %2u %08lX", smf->msg.cmd ? "Cmd" : "Rsp", 
           smf->msg.num, SWAP32(smf->msg.argx));
    if (smf->msg.num==52)
    {
        if (smf->msg.cmd)
            disp_cmd52(smf);
        else
            disp_rsp52(smf);
    }
    if (smf->msg.num==53)
    {
        if (smf->msg.cmd)
            disp_cmd53(smf);
        else
            disp_rsp53(smf);
    }
    printf("\n");
}

// Display command 52
void disp_cmd52(SDIO_MSG *smf)
{
    printf(" %s", smf->cmd52.wr ? "Wr" : "Rd");
    printf(" %s", smf->cmd52.func==0 ? "BUS " : smf->cmd52.func==1 ?"BAK " : "WLAN");
    printf(" %05X", REG_ADDR(smf->cmd52));
    if (smf->cmd52.wr)
        printf(" %02X", smf->cmd52.data);
}

// Display response to command 52
void disp_rsp52(SDIO_MSG *smf)
{
    printf(" Flags %02X data %02X", smf->rsp52.flags, smf->rsp52.data);
}

// Display command 53
void disp_cmd53(SDIO_MSG *smf)
{
    int n = smf->cmd53.lenl + smf->cmd53.lenh*256;

    printf(" %s", smf->cmd53.wr ? "Wr" : "Rd");
    printf(" %s", smf->cmd53.func==0 ? "BUS " : smf->cmd53.func==1 ?"BAK " : "WLAN");
    printf(" %05X %s %u", REG_ADDR(smf->cmd53), smf->cmd53.blk ? "blks" : "len", n?n:512);
}

// Display response to command 53
void disp_rsp53(SDIO_MSG *smf)
{
    printf(" Flags %02X", smf->rsp52.flags);
}

// Enable / disable logging
void log_enable(int on)
{
    logging = on;
}

// Increment log to next record, keeping the last LOG_SIZE records
void log_incr(void)
{
    log_idx = (log_idx + 1) % LOG_SIZE;
    if (log_start == log_idx)
        log_start = (log_start + 1) % LOG_SIZE;
}

// Log a command or response
void log_msg(SDIO_MSG *msgp)
{
    if (logging)
    {
        memcpy(&msglog[log_idx], msgp, MSG_BYTES);
        log_incr();
    }
}

// Log data, retain max 6 bytes
void log_data(uint8_t *data, int len, int ok)
{
    int nbytes = len, n = MIN(nbytes, LOG_DATA_LEN);

    if (logging > LOG_CMDS)
    {
        msglog[log_idx].data[0] = (uint8_t)(nbytes / 256) | 0x80 | (ok?0x40:0);
        msglog[log_idx].data[1] = (uint8_t)nbytes;
        memcpy(&msglog[log_idx].data[2], data, n);
        log_incr();
    }
}

// Log data write acknowledgement
void log_data_ack(uint8_t val)
{
    if (logging > LOG_CMDS)
    {
        msglog[log_idx].data[0] = LOG_DATA_ACK;
        msglog[log_idx].data[1] = val;
        log_incr();
    }
}

// Display the message log, and trigger breakpoint
void disp_log_break(void)
{
    disp_log();
    gdb_break();
}

// Dump the message log
void disp_log(void)
{
    int n;
    uint8_t b;

    while (log_start != log_idx)
    {
        if ((b=msglog[log_start].data[0]) == 0)
            printf("00\n");
        else if (b == LOG_DATA_ACK)
            printf("Ack %02X\n", msglog[log_start].data[1]);
        else if (b & 0x80)
        {
            n = (b & 0x3f) * 256 + msglog[log_start].data[1];
            printf("Data %2u bytes: ", n);
            disp_bytes(&msglog[log_start].data[2], MIN(n, LOG_DATA_LEN));
            printf("%s\n", (b & 0x40) ? "*" : "?");
        }
        else 
            disp_msg(&msglog[log_start]);

        log_start = (log_start + 1) % LOG_SIZE;
    }
    printf("\n");
    fflush(stdout);
}

// Initialise 32 kHz oscillator
void osc_init(void)
{
    *GP2DIV = GP2DIV_VAL;
    *GP2CTL = GP2CTL_VAL;
    gpio_set(SD_32KHZ_PIN, GPIO_ALT0, GPIO_NOPULL);
}

// EOF

