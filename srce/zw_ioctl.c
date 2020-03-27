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

#include "zw_sdio.h"
#include "zw_regs.h"
#include "zw_ioctl.h"
#include "zw_gpio.h"

IOCTL_MSG ioctl_txmsg, ioctl_rxmsg;
int txglom;
uint16_t ioctl_reqid=0;

char ioctl_cmd_fields[] =  "1:seq\0" "1:chan\0" "1:nextlen\0" "1:hdrlen\0"
                           "1:flow\0" "1:credit\0" "1:\0" "1:\0"
                           "4:cmd\0" "2:outlen\0" "2:inlen\0"
                           "4:flags\0" "4:status\0" "";

// Get event data
int ioctl_get_event(uint8_t *data, int maxlen)
{
    IOCTL_EVENT_HDR hdr={0};
    int n=0, dlen;

    if (sdio_cmd53_read(SD_FUNC_RAD, SB_32BIT_WIN, (void *)&hdr, sizeof(IOCTL_EVENT_HDR)) &&
        hdr.len>0 && hdr.notlen>0 && hdr.len==(hdr.notlen^0xffff))
    {
        while (n<hdr.len && n<maxlen)
        {
            dlen = MIN(MIN(maxlen-n, hdr.len-n), IOCTL_MAX_DLEN);
            sdio_cmd53_read(SD_FUNC_RAD, SB_32BIT_WIN, (void *)(&data[n]), dlen);
            n += dlen;
        }
        while (n < hdr.len)
        {
            dlen = MIN(hdr.len-n, IOCTL_MAX_DLEN);
            sdio_cmd53_read(SD_FUNC_RAD, SB_32BIT_WIN, 0, dlen);
            n += dlen;
        }
    }
    return(n);
}

// Get an IOCTL variable
int ioctl_getvar(char *name, uint8_t *data, int dlen)
{
    return(ioctl_cmd(0, IOCTL_GETVAR, name, data, dlen));
}

// Set an integer IOCTL variable
int ioctl_set_uint32(char *name, uint32_t val)
{
    U32DATA u32 = {.uint32=val};

    return(ioctl_cmd(1, IOCTL_SETVAR, name, u32.bytes, 4));
}

// Set an IOCTL variable with data value
int ioctl_set_data(char *name, void *data, int len)
{
    return(ioctl_cmd(1, IOCTL_SETVAR, name, data, len));
}

// IOCTL write with integer parameter
int ioctl_cmd_int32(int cmd, int val)
{
    U32DATA u32 = {.uint32=(uint32_t)val};

    return(ioctl_cmd(1, cmd, 0, u32.bytes, 4));
}

// IOCTL write with data
int ioctl_cmd_data(int cmd, void *data, int len)
{
    return(ioctl_cmd(1, cmd, 0, data, len));
}

// Do an IOCTL transaction
int ioctl_cmd(int wr, int cmd, char *name, void *data, int dlen)
{
    static uint8_t txseq=1;
    IOCTL_MSG *msgp = &ioctl_txmsg;
    IOCTL_CMD *cmdp = txglom ? &msgp->glom_cmd.cmd : &msgp->cmd;
    int ret=0, namelen = name ? strlen(name)+1 : 0;
    int txdlen = wr ? namelen + dlen : MAX(namelen, dlen);
    int hdrlen = cmdp->data - (uint8_t *)&ioctl_txmsg;
    int txlen = ((hdrlen + txdlen + 3) / 4) * 4;
    uint32_t val;

    memset(msgp, 0, sizeof(ioctl_txmsg));
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
    sdio_cmd53_write(SD_FUNC_RAD, SB_32BIT_WIN, (void *)msgp, txlen);
    ioctl_wait(IOCTL_WAIT_USEC);
    sdio_bak_read32(SB_INT_STATUS_REG, &val);
    if (val & 0xff)
    {
        sdio_bak_write32(SB_INT_STATUS_REG, val);
        ret = sdio_cmd53_read(SD_FUNC_RAD, SB_32BIT_WIN, (void *)&ioctl_rxmsg, txlen);
        if (ioctl_rxmsg.cmd.flags >> 16 != ioctl_reqid)
            ret = 0;
        else if (!wr && data && dlen)
            memcpy(data, ioctl_rxmsg.cmd.data, dlen);
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


// EOF

