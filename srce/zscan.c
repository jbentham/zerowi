// ZeroWi bare-metal WiFi driver, see https://iosoft.blog/zerowi
// Raspberry Pi WiFi network scan
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

#define VERSION "0.76"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "whd_types.h"
#include "whd_events.h"
#include "whd_wlioctl.h"

#include "zw_gpio.h"
#include "zw_sdio.h"
#include "zw_regs.h"
#include "zw_ioctl.h"

// WiFi channel number to scan (0 for all channels)
#define SCAN_CHAN       1

// Set non-zero to include WiFi firmware in image
#define INCLUDE_FIRMWARE 1
#define FIRMWARE_FNAME   "../firmware/brcmfmac43430-sdio.c"

// Length of firmware file (rounded up to 4-byte value)
#define FIRMWARE_LEN    0x5ee84
#if INCLUDE_FIRMWARE
extern const unsigned char firmware_bin[FIRMWARE_LEN];
uint32_t firmware_pos;
#endif

// Configuration for brcmfmac43430-sdio
uint8_t config_data[] = 
"manfid=0x2d0\0""prodid=0x0726\0""vendid=0x14e4\0""devid=0x43e2\0"
"boardtype=0x0726\0""boardrev=0x1202\0""boardnum=22\0""macaddr=00:90:4c:c5:12:38\0"
"sromrev=11\0""boardflags=0x00404201\0""boardflags3=0x08000000\0""xtalfreq=37400\0"
"nocrc=1\0""ag0=255\0""aa2g=1\0""ccode=ALL\0""pa0itssit=0x20\0""extpagain2g=0\0"
"pa2ga0=-168,7161,-820\0""AvVmid_c0=0x0,0xc8\0""cckpwroffset0=5\0""maxp2ga0=84\0"
"txpwrbckof=6\0""cckbw202gpo=0\0""legofdmbw202gpo=0x66111111\0"
"mcsbw202gpo=0x77711111\0""propbw202gpo=0xdd\0""ofdmdigfilttype=18\0"
"ofdmdigfilttypebe=18\0""papdmode=1\0""papdvalidtest=1\0""pacalidx2g=32\0"
"papdepsoffset=-36\0""papdendidx=61\0""il0macaddr=00:90:4c:c5:12:38\0"
"wl0id=0x431b\0""deadman_to=0xffffffff\0""muxenab=0x1\0""spurconfig=0x3 \0"
"btc_mode=1\0""btc_params8=0x4e20\0""btc_params1=0x7530\0""\0\0\0\0\xaa\x00\x55\xff";
int config_len = sizeof(config_data) - 1;

// SDIO Tx buffer (must be multiple of 256, and less than 32K)
uint8_t txbuffer[0x4000];

// Network scan parameters
#define SCAN_CHAN_TIME      40
#define SSID_MAXLEN         32
#define SCANTYPE_ACTIVE     0
#define SCANTYPE_PASSIVE    1
typedef struct {
    uint32_t version;
    uint16_t action,
             sync_id;
    uint32_t ssidlen;
    uint8_t  ssid[SSID_MAXLEN],
             bssid[6],
             bss_type,
             scan_type;
    uint32_t nprobes,
             active_time,
             passive_time,
             home_time;
    uint16_t nchans,
             nssids;
    uint8_t  chans[14][2],
             ssids[1][SSID_MAXLEN];
} SCAN_PARAMS;
SCAN_PARAMS scan_params = {
    .version=1, .action=1, .sync_id=0x1234, .ssidlen=0, .ssid={0}, 
    .bssid={0xff,0xff,0xff,0xff,0xff,0xff}, .bss_type=2,
    .scan_type=SCANTYPE_PASSIVE, .nprobes=~0, .active_time=~0,
    .passive_time=~0, .home_time=~0, 
#if SCAN_CHAN == 0
    .nchans=14, .nssids=0, 
    .chans={{1,0x2b},{2,0x2b},{3,0x2b},{4,0x2b},{5,0x2b},{6,0x2b},{7,0x2b},
      {8,0x2b},{9,0x2b},{10,0x2b},{11,0x2b},{12,0x2b},{13,0x2b},{14,0x2b}},
#else
    .nchans=1, .nssids=0, .chans={{SCAN_CHAN,0x2b}}, .ssids={{0}}
#endif
};

// Escan result event (excluding 12-byte IOCTL header)
typedef struct {
    uint8_t pad[10];
    whd_event_t event;
    wl_escan_result_t escan;
} escan_result;

// IOCTL commands
#define IOCTL_UP                    2
#define IOCTL_SET_SCAN_CHANNEL_TIME 0xb9

// Event handling
uint8_t eventbuff[1600];
EVT_STR escan_evts[]=ESCAN_EVTS;

// State of SDIO clock line
extern uint8_t clkval;

void disp_ssid(uint8_t *data);
void disp_mac_addr(uint8_t *data);
void disp_block(uint8_t *data, int len);
void gdb_break(void);
int sdio_init(void);
int write_firmware(void);
int write_nvram(void);
void disp_bytes(uint8_t *addr, int len);
void sd_setup(void);

int main(void)
{
    int ticks=0, ledon=0, n;
    uint32_t val=0;
    uint8_t resp[256] = {0}, eth[7]={0};
    IOCTL_EVENT_HDR ieh;
    escan_result *erp = (escan_result *)eventbuff;

    crc7_init();
    qcrc16r_init();
    ustimeout(&ticks, 0);
    printf("\nZerowi scan test v" VERSION "\n");
    fflush(stdout);
    osc_init();
    gpio_set(LED_PIN, GPIO_OUT, GPIO_NOPULL);
    sd_setup();
    gpio_out(WLAN_ON_PIN, 1);
    flash_init(10000);
    usdelay(10000);
    log_enable(2);
    sdio_init();
    sdio_cmd53_read(SD_FUNC_RAD, SB_32BIT_WIN, resp, 64);
    n = ioctl_get_data("cur_etheraddr", 0, eth, 6);
    printf("MAC address ");
    if (n)
        disp_mac_addr(eth);
    else
        printf("unavailable");
    n = ioctl_get_data("ver", 0, resp, sizeof(resp));
    printf("\nFirmware %s\n", (n ? (char *)resp : "not responding"));
    ioctl_wr_int32(IOCTL_SET_SCAN_CHANNEL_TIME, 0, SCAN_CHAN_TIME);
    if (!ioctl_wr_int32(WLC_UP, 200, 0))
    {
        printf("WiFi CPU not running\n");
        fflush(stdout);
        gdb_break();
    }
    sdio_bak_write32(SB_INT_STATUS_REG, val);
    sdio_cmd53_read(SD_FUNC_RAD, SB_32BIT_WIN, (void *)resp, 64);
    ioctl_enable_evts(escan_evts);
    ioctl_set_data("escan", 0, &scan_params, sizeof(scan_params));
    while (1)
    {
        usdelay(SD_CLK_DELAY);
        gpio_out(SD_CLK_PIN, clkval=!clkval);
        if (ustimeout(&ticks, 100000))
        {
            gpio_out(LED_PIN, ledon = !ledon);
            if (!ledon)
            {
                n = ioctl_get_event(&ieh, eventbuff, sizeof(eventbuff));
                if (n > sizeof(escan_result))
                {
                    printf("%u bytes\n", n);
                    disp_mac_addr((uint8_t *)&erp->event.whd_event.addr);
                    printf(" %2u ", SWAP16(erp->escan.bss_info->chanspec));
                    disp_ssid(&erp->escan.bss_info->SSID_len);
                    printf("\n");
                    fflush(stdout);
                }
                else
                {
                    printf(".");
                    fflush(stdout);
                }
            }
        }
    }
}

// Display SSID, prefixed with length byte
void disp_ssid(uint8_t *data)
{
    int i=*data++;

    if (i == 0 || *data == 0)
        printf("[hidden]");
    else if (i <= SSID_MAXLEN)
    {
        while (i-- > 0)
            putchar(*data++);
    }
    else
        printf("[invalid length %u]", i);
}

// Display MAC address
void disp_mac_addr(uint8_t *data)
{
    int i;

    for (i=0; i<6; i++)
        printf("%s%02X", i?":":"", data[i]);
}

// Display block of data
void disp_block(uint8_t *data, int len)
{
    int i=0, n;

    while (i < len)
    {
        n = MIN(len-i, 32);
        disp_bytes(&eventbuff[i], n);
        i += n;
        printf("\n");
        fflush(stdout);
    }
}

// Dummy function to trigger debug breakpoint
void gdb_break(void)
{
} // Trigger GDB break

// Initialise SDIO card, return RCA
int sdio_init(void)
{
    SDIO_MSG resp;
    int rca=0;
    U32DATA u32d;
    uint8_t data[520];

    sdio_cmd52(SD_FUNC_BUS, 0x06,    0, SD_RD, 0, 0);   
    usdelay(20000);
    sdio_cmd52(SD_FUNC_BUS, 0x06,    8, SD_WR, 0, 0);   
    usdelay(20000);
    sdio_cmd(0, 0, 0);
    sdio_cmd(8, 0x1aa, 0);
    // Enable I/O mode
    sdio_cmd(5, 0, 0);
    sdio_cmd(5, 0x200000, 0);
    // Assert SD device
    sdio_cmd(3, 0, &resp);
    rca = SWAP16(resp.rsp3.rcax);
    sdio_cmd7(rca, 0);
    // [0.243831] Set bus interface
    sdio_cmd52_writes(SD_FUNC_BUS, BUS_SPEED_CTRL_REG, 0x03, 1);
    sdio_cmd52_writes(SD_FUNC_BUS, BUS_BI_CTRL_REG, 0x42, 1);
    // [17.999101] Set block sizes
    sdio_cmd52_writes(SD_FUNC_BUS, BUS_BAK_BLKSIZE_REG, SD_BAK_BLK_BYTES, 2);
    sdio_cmd52_writes(SD_FUNC_BUS, BUS_RAD_BLKSIZE_REG, SD_RAD_BLK_BYTES, 2);
    // [17.999944] Enable I/O 
    sdio_cmd52_writes(SD_FUNC_BUS, BUS_IOEN_REG, 1<<SD_FUNC_BAK, 1);
    if (!sdio_cmd52_reads_check(SD_FUNC_BUS, BUS_IORDY_REG, 0xff, 2, 1))
        disp_log_break();
    // [18.001750] Set backplane window
    sdio_bak_window(BAK_BASE_ADDR);
    // [18.001905] Read chip ID 
    sdio_cmd53_read(SD_FUNC_BAK, SB_32BIT_WIN, u32d.bytes, 4);
    // [18.002173] Set chip clock
    sdio_cmd52_writes(SD_FUNC_BAK, BAK_CHIP_CLOCK_CSR_REG, 0x28, 1);
    sdio_cmd52_reads(SD_FUNC_BAK, BAK_CHIP_CLOCK_CSR_REG, &u32d.uint32, 1);
    sdio_cmd52_writes(SD_FUNC_BAK, BAK_CHIP_CLOCK_CSR_REG, 0x21, 1);
    // [18.004850] Disable pullups 
    sdio_cmd52_writes(SD_FUNC_BAK, BAK_PULLUP_REG, 0, 1);
    // Get chip ID again, and config base addr [18.005201]
    sdio_cmd53_read(SD_FUNC_BAK, SB_32BIT_WIN, u32d.bytes, 4);
    sdio_cmd53_read(SD_FUNC_BAK, SB_32BIT_WIN+0xfc, u32d.bytes, 4);
    // Reset cores [18.030305]
    sdio_bak_write32(ARM_IOCTRL_REG, 0x03);
    sdio_bak_write32(MAC_IOCTRL_REG, 0x07);
    sdio_bak_write32(MAC_RESETCTRL_REG, 0x00);
    sdio_bak_write32(MAC_IOCTRL_REG, 0x05);
    // [18.032572]
    sdio_bak_write32(SRAM_IOCTRL_REG, 0x03);
    sdio_bak_write32(SRAM_RESETCTRL_REG, 0x00);
    sdio_bak_write32(SRAM_IOCTRL_REG, 0x01);
    if (!sdio_bak_read32(SRAM_IOCTRL_REG, &u32d.uint32) || u32d.uint8!=1)
        disp_log_break();
    // [18.034039]
    sdio_bak_write32(SRAM_BANKX_IDX_REG, 0x03);
    sdio_bak_write32(SRAM_BANKX_PDA_REG, 0x00);
    // [18.034733]
    if (!sdio_bak_read32(SRAM_IOCTRL_REG, &u32d.uint32) || u32d.uint8!=1)
        disp_log_break();
    if (!sdio_bak_read32(SRAM_RESETCTRL_REG, &u32d.uint32) || u32d.uint8!=0)
        disp_log_break();
    // [18.035416]
    sdio_bak_read32(SRAM_BASE_ADDR, &u32d.uint32);
    sdio_bak_write32(SRAM_BANKX_IDX_REG, 0);
    sdio_bak_read32(SRAM_UNKNOWN_REG, &u32d.uint32);
    sdio_bak_write32(SRAM_BANKX_IDX_REG, 1);
    sdio_bak_read32(SRAM_UNKNOWN_REG, &u32d.uint32);
    sdio_bak_write32(SRAM_BANKX_IDX_REG, 2);
    sdio_bak_read32(SRAM_UNKNOWN_REG, &u32d.uint32);
    sdio_bak_write32(SRAM_BANKX_IDX_REG, 3);
    // [18.037502]
    if (!sdio_cmd52_reads_check(SD_FUNC_BUS, 0x00f1, 0xff, 1, 1))
        disp_log_break();
    sdio_cmd52_writes(SD_FUNC_BUS, 0x00f1, 3, 1);
    sdio_cmd53_read(SD_FUNC_BAK, 0x8600, u32d.bytes, 4);
    u32d.bytes[1] |= 0x40;
    sdio_cmd53_write(SD_FUNC_BAK, 0x8600, u32d.bytes, 4);
    // [18.052762]
    sdio_cmd52_writes(SD_FUNC_BUS, BUS_IOEN_REG, 1<<SD_FUNC_BAK, 1);
    sdio_cmd52_writes(SD_FUNC_BAK, BAK_CHIP_CLOCK_CSR_REG, 0, 1);
    usdelay(45000);
    sdio_cmd52_writes(SD_FUNC_BAK, BAK_CHIP_CLOCK_CSR_REG, 8, 1);
    if (!sdio_cmd52_reads_check(SD_FUNC_BAK, BAK_CHIP_CLOCK_CSR_REG, 0xff, 0x48, 1))
        disp_log_break();
    // [18.100946] Load firmware
    write_firmware();
    sdio_bak_window(0x58000);
    sdio_cmd53_read(SD_FUNC_BAK, 0xee80, data, 4);
    // [19.143195] Load config data
    write_nvram();
    sdio_cmd53_read(SD_FUNC_BAK, 0xffd4, data, 44);
    // [19.146150]
    sdio_bak_read32(SRAM_IOCTRL_REG, &u32d.uint32); 
    sdio_bak_read32(SRAM_RESETCTRL_REG, &u32d.uint32); 
    sdio_bak_write32(SB_INT_STATUS_REG, 0xffffffff);
    // [19.147404]
    sdio_bak_write32(ARM_IOCTRL_REG, 0x03);
    sdio_bak_write32(ARM_RESETCTRL_REG, 0x00);
    sdio_bak_write32(ARM_IOCTRL_REG, 0x01);
    sdio_bak_read32(ARM_IOCTRL_REG, &u32d.uint32);
    sdio_bak_window(BAK_BASE_ADDR);
    sdio_cmd52_writes(SD_FUNC_BAK, BAK_CHIP_CLOCK_CSR_REG, 0, 1);
    sdio_cmd52_writes(SD_FUNC_BAK, BAK_CHIP_CLOCK_CSR_REG, 0x10, 1);
    sdio_cmd52_reads(SD_FUNC_BAK, BAK_CHIP_CLOCK_CSR_REG, &u32d.uint32, 1);
    usdelay(50000);
    if (!sdio_cmd52_reads(SD_FUNC_BAK, BAK_CHIP_CLOCK_CSR_REG, &u32d.uint32, 1) || u32d.uint8!=0xd0)
        disp_log_break();
    // [19.190728]
    sdio_cmd52_writes(SD_FUNC_BAK, BAK_CHIP_CLOCK_CSR_REG, 0xd2, 1);
    sdio_bak_write32(SB_TO_SB_MBOX_DATA_REG, 0x40000);
    sdio_cmd52_writes(SD_FUNC_BUS, BUS_IOEN_REG, (1<<SD_FUNC_BAK) | (1<<SD_FUNC_RAD), 1);
    sdio_cmd52_reads(SD_FUNC_BUS, BUS_IORDY_REG, &u32d.uint32, 1);
    usdelay(100000);
    if (!sdio_cmd52_reads(SD_FUNC_BUS, BUS_IORDY_REG, &u32d.uint32, 1) || u32d.uint8!=0x06)
        disp_log_break();
    sdio_bak_write32(SB_INT_HOST_MASK_REG, 0x200000f0);
    sdio_bak_read32(SR_CONTROL1, &u32d.uint32);
    // [19.282972]
    sdio_bak_window(BAK_BASE_ADDR);
    sdio_cmd52_writes(SD_FUNC_BAK, BAK_WAKEUP_REG, 2, 1);
    sdio_cmd52_writes(SD_FUNC_BUS, BUS_BRCM_CARDCAP, 6, 1);
    sdio_cmd52_writes(SD_FUNC_BUS, BUS_INTEN_REG, 0x07, 1);
    sdio_cmd52_reads(SD_FUNC_BUS, BUS_INTPEND_REG, &u32d.uint32, 1);
    // [19.284023]
    sdio_bak_read32(SB_INT_STATUS_REG, &u32d.uint32);
    sdio_bak_write32(SB_INT_STATUS_REG, 0x200000c0);
    sdio_bak_read32(SB_TO_HOST_MBOX_DATA_REG, &u32d.uint32);
    sdio_bak_write32(SB_TO_SB_MBOX_REG, 0x02);
    sdio_bak_read32(SR_CONTROL1, &u32d.uint32);
    // [19.285708]
    sdio_bak_read32(0x68000 | 0x7ffc, &u32d.uint32);
    sdio_bak_window(0x38000);
    sdio_cmd53_read(SD_FUNC_BAK, 0x70d4, data, 64);
    // [19.286520]
    sdio_bak_read32(SB_INT_STATUS_REG, &u32d.uint32);
    sdio_bak_write32(SB_INT_STATUS_REG, 0x80);
    sdio_cmd53_read(SD_FUNC_RAD, SB_32BIT_WIN, data, 64);
    return(rca);
}

// Functions to access firmware image
// Open for reading
void firm_open_read(int addr)
{
#if INCLUDE_FIRMWARE
    firmware_pos = addr;
#else
    flash_open_read(addr);
#endif
}
// Read n bytes
void firm_read(uint8_t *dp, int len)
{
#if INCLUDE_FIRMWARE
    memcpy(dp, &firmware_bin[firmware_pos], len);
    firmware_pos += len;
#else
    flash_read(dp, len);
#endif
}
// Close
void firm_close(void)
{
#if !INCLUDE_FIRMWARE
    flash_close();
#endif
}

// Upload blocks of firmware from flash to chip RAM
int write_firmware(void)
{
    int len, n=0, nbytes=0, nblocks;
    uint32_t addr;

    firm_open_read(0);
    while (nbytes < FIRMWARE_LEN)
    {
        addr = sdio_bak_addr(nbytes);
        len = MIN(sizeof(txbuffer), FIRMWARE_LEN-nbytes);
        nblocks = len / SD_BAK_BLK_BYTES;
		if (nblocks > 0)
        {
            firm_read(txbuffer, nblocks*SD_BAK_BLK_BYTES);
            n = sdio_write_blocks(SD_FUNC_BAK, SB_32BIT_WIN+addr, txbuffer, nblocks);
            if (!n)
                break;
            nbytes += nblocks * SD_BAK_BLK_BYTES;
        }
        else
        {
            firm_read(txbuffer, len);
            sdio_cmd53_write(SD_FUNC_BAK, SB_32BIT_WIN+addr, txbuffer, len);
            nbytes += len;
        }
    }
    firm_close();
    return(nbytes);
}

// Upload blocks of config data to chip NVRAM
int write_nvram(void)
{
    int nbytes=0, len;

    sdio_bak_window(0x078000);
    while (nbytes < config_len)
    {
        len = MIN(config_len-nbytes, SD_BAK_BLK_BYTES);
        sdio_cmd53_write(SD_FUNC_BAK, 0xfd54+nbytes, &config_data[nbytes], len);
        nbytes += len;
    }
    return(nbytes);
}

// Set up SD interface
void sd_setup(void)
{
    gpio_set(SD_CLK_PIN, GPIO_OUT, GPIO_NOPULL);
    gpio_set(SD_CMD_PIN, GPIO_IN, GPIO_PULLUP);
    gpio_set(SD_D0_PIN, GPIO_IN, GPIO_PULLUP);
    gpio_set(SD_D1_PIN, GPIO_IN, GPIO_PULLUP);
    gpio_set(SD_D2_PIN, GPIO_IN, GPIO_PULLUP);
    gpio_set(SD_D3_PIN, GPIO_IN, GPIO_PULLUP);
}

#if INCLUDE_FIRMWARE
#include FIRMWARE_FNAME
#endif
// EOF

