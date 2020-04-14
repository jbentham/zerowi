// ZeroWi bare-metal WiFi driver, see https://iosoft.blog/zerowi
// IOCTL interface
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

#include "whd_types.h"
#include "whd_wlioctl.h"
#include "whd_events.h"

#include "zw_sdio.h"
#include "zw_regs.h"
#include "zw_ioctl.h"
#include "zw_gpio.h"

#define IOCTL_POLL_MSEC     2

IOCTL_MSG ioctl_txmsg, ioctl_rxmsg;
int txglom;
uint16_t ioctl_reqid=0;
uint8_t event_mask[EVENT_MAX / 8];
EVT_STR *current_evts;
char ioctl_event_hdr_fields[] =  
    "2:len 2: 1:seq 1:chan 1: 1:hdrlen 1:flow 1:credit";
#define MAX_EVENT_STATUS 16
char *event_status[MAX_EVENT_STATUS] = {
    "SUCCESS","FAIL","TIMEOUT","NO_NETWORK","ABORT","NO_ACK",
    "UNSOLICITED","ATTEMPT","PARTIAL","NEWSCAN","NEWASSOC",
    "11HQUIET","SUPPRESS","NOCHANS","CCXFASTRM","CS_ABORT" };

// Get event data, return data length excluding header
int ioctl_get_event(IOCTL_EVENT_HDR *hp, uint8_t *data, int maxlen)
{
    int n=0, dlen=0, blklen;

    hp->len = 0;
    if (sdio_cmd53_read(SD_FUNC_RAD, SB_32BIT_WIN, (void *)hp, sizeof(IOCTL_EVENT_HDR)) &&
        hp->len>sizeof(IOCTL_EVENT_HDR) && hp->notlen>0 && hp->len==(hp->notlen^0xffff))
    {
        dlen = hp->len - sizeof(IOCTL_EVENT_HDR);
        while (n<dlen && n<maxlen)
        {
            blklen = MIN(MIN(maxlen-n, hp->len-n), IOCTL_MAX_BLKLEN);
            sdio_cmd53_read(SD_FUNC_RAD, SB_32BIT_WIN, (void *)(&data[n]), blklen);
            n += blklen;
        }
        while (n < dlen)
        {
            blklen = MIN(hp->len-n, IOCTL_MAX_BLKLEN);
            sdio_cmd53_read(SD_FUNC_RAD, SB_32BIT_WIN, 0, blklen);
            n += blklen;
        }
    }
    return(dlen > maxlen ? maxlen : dlen);
}

// Enable events
int ioctl_enable_evts(EVT_STR *evtp)
{
    current_evts = evtp;
    memset(event_mask, 0, sizeof(event_mask));
    while (evtp->num >= 0)
    {
        if (evtp->num / 8 < sizeof(event_mask))
            SET_EVENT(event_mask, evtp->num);
        evtp++;
    }
    return(ioctl_set_data("event_msgs", 0, event_mask, sizeof(event_mask)));
}

// Return string corresponding to event number, without "WLC_E_" prefix
char *ioctl_evt_str(int event)
{
    EVT_STR *evtp=current_evts;

    while (evtp && evtp->num>=0 && evtp->num!=event)
        evtp++;
    return(evtp && evtp->num>=0 && strlen(evtp->str)>6 ? &evtp->str[6] : "?");
}

// Return string corresponding to event status
char *ioctl_evt_status_str(int status)
{
    return(status>=0 && status<MAX_EVENT_STATUS ? event_status[status] : "?");
}

// Get data block from IOCTL variable
int ioctl_get_data(char *name, int wait_msec, uint8_t *data, int dlen)
{
    return(ioctl_cmd(WLC_GET_VAR, name, wait_msec, 0, data, dlen));
}

// Set data block in IOCTL variable
int ioctl_set_data(char *name, int wait_msec, void *data, int len)
{
    return(ioctl_cmd(WLC_SET_VAR, name, wait_msec, 1, data, len));
}

// Set an unsigned integer IOCTL variable
int ioctl_set_uint32(char *name, int wait_msec, uint32_t val)
{
    U32DATA u32 = {.uint32=val};

    return(ioctl_cmd(WLC_SET_VAR, name, wait_msec, 1, u32.bytes, 4));
}

// Set 2 integers in IOCTL variable
int ioctl_set_intx2(char *name, int wait_msec, int val1, int val2)
{
    int data[2] = {val1, val2};

    return(ioctl_cmd(WLC_SET_VAR, name, wait_msec, 1, data, 8));
}

// IOCTL write with integer parameter
int ioctl_wr_int32(int cmd, int wait_msec, int val)
{
    U32DATA u32 = {.uint32=(uint32_t)val};

    return(ioctl_cmd(cmd, 0, wait_msec, 1, u32.bytes, 4));
}

// IOCTL write data
int ioctl_wr_data(int cmd, int wait_msec, void *data, int len)
{
    return(ioctl_cmd(cmd, 0, wait_msec, 1, data, len));
}

// IOCTL read data
int ioctl_rd_data(int cmd, int wait_msec, void *data, int len)
{
    return(ioctl_cmd(cmd, 0, wait_msec, 0, data, len));
}

// Do an IOCTL transaction, get response, optionally waiting for it
int ioctl_cmd(int cmd, char *name, int wait_msec, int wr, void *data, int dlen)
{
    static uint8_t txseq=1;
    IOCTL_MSG *msgp = &ioctl_txmsg, *rsp = &ioctl_rxmsg;
    IOCTL_CMD *cmdp = txglom ? &msgp->glom_cmd.cmd : &msgp->cmd;
    int ret=0, namelen = name ? strlen(name)+1 : 0;
    int txdlen = wr ? namelen + dlen : MAX(namelen, dlen);
    int hdrlen = cmdp->data - (uint8_t *)&ioctl_txmsg;
    int txlen = ((hdrlen + txdlen + 3) / 4) * 4; //, rxlen;
    uint32_t val=0;

    // Prepare IOCTL command
    memset(msgp, 0, sizeof(ioctl_txmsg));
    memset(rsp, 0, sizeof(ioctl_rxmsg));
    msgp->notlen = ~(msgp->len = hdrlen+txdlen);
    if (txglom)
    {
        msgp->glom_cmd.glom_hdr.len = hdrlen + txdlen - 4;
        msgp->glom_cmd.glom_hdr.flags = 1;
    }
    cmdp->seq = txseq++;
    cmdp->hdrlen = txglom ? 20 : 12;
    cmdp->cmd = cmd;
    cmdp->outlen = txdlen;
    cmdp->flags = ((uint32_t)++ioctl_reqid << 16) | (wr ? 2 : 0);
    if (namelen)
        memcpy(cmdp->data, name, namelen);
    if (wr)
        memcpy(&cmdp->data[namelen], data, dlen);
    // Send IOCTL command
    sdio_cmd53_write(SD_FUNC_RAD, SB_32BIT_WIN, (void *)msgp, txlen);
    ioctl_wait(IOCTL_WAIT_USEC);
    while (wait_msec>=0 && ret==0)
    {
        // Wait for response to be available
        wait_msec -= IOCTL_POLL_MSEC;
        sdio_bak_read32(SB_INT_STATUS_REG, &val);
        // If response is waiting..
        if (val & 0xff)
        {
            // ..request response
            sdio_bak_write32(SB_INT_STATUS_REG, val);
            // Fetch response
            ret = sdio_cmd53_read(SD_FUNC_RAD, SB_32BIT_WIN, (void *)rsp, txlen);
            // Discard response if not matching request
            if ((rsp->cmd.flags>>16) != ioctl_reqid)
                ret = 0;
            // Exit if error response
            if (ret && (rsp->cmd.flags & 1))
            {
                ret = 0;
                break;
            }
            // If OK, copy data to buffer
            if (ret && !wr && data && dlen)
                memcpy(data, rsp->cmd.data, dlen);
        }
        // If no response, wait
        else
            usdelay(IOCTL_POLL_MSEC * 1000);
    }
    return(ret);
}

// Wait until IOCTL command has been processed
int ioctl_wait(int usec)
{
    int tout, ready=0;

    ustimeout(&tout, 0);
    while (!ready && !ustimeout(&tout, usec))
        ready = ioctl_ready();
    return(ready);
}

// Check if IOCTL command has been processed (data bit 1 low)
int ioctl_ready(void)
{
    return(!gpio_in(SD_D1_PIN));
}

// Display fields in structure
// Fields in descriptor are num:id (little-endian) or num;id (big_endian)
void disp_fields(void *data, char *fields, int maxlen)
{
    char *strs=fields, delim=0;
    uint8_t *dp = (uint8_t *)data;
    int n, dlen;
    int val;
    
    while (*strs && dp-(uint8_t *)data<maxlen)
    {
        dlen = 0;
        while (*strs>='0' && *strs<='9')
            dlen = dlen*10 + *strs++ - '0';
        delim = *strs++;
        if (*strs > ' ')
        {
            while (*strs >= '0')
                putchar(*strs++);
            putchar('=');
            if (dlen <= 4)
            {
                val = 0;
                for (n=0; n<dlen; n++)
                    val |= (uint32_t)(*dp++) << ((delim==':' ? n : dlen-n-1) * 8);
                printf("%02X ", val);
            }
            else
            {
                for (n=0; n<dlen; n++)
                    printf("%02X", *dp++);
                putchar(' ');
            }
        }
        else
            dp += dlen;
        while (*strs == ' ')
            strs++;
    }
}


// EOF

