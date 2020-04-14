// ZeroWi bare-metal WiFi driver, see https://iosoft.blog/zerowi
// IOCTL interface definitions
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

#define IOCTL_WAIT_USEC     2000
#define IOCTL_MAX_BLKLEN    256

// Event structures
typedef struct {
    whd_event_eth_hdr_t   hdr;
    struct whd_event_msg  msg;
    uint8_t data[1];
} ETH_EVENT;
typedef struct {
    uint8_t pad[10];
    whd_event_ether_header_t eth_hdr;
    union {
        ETH_EVENT event;
        uint8_t data[1];
    };
} ETH_EVENT_FRAME;

typedef struct {
    uint8_t  seq,       // sdpcm_sw_header
             chan,
             nextlen,
             hdrlen,
             flow,
             credit,
             reserved[2];
    uint32_t cmd;       // cdc_header
    uint16_t outlen,
             inlen;
    uint32_t flags,
             status;
    uint8_t data[IOCTL_MAX_BLKLEN];
} IOCTL_CMD;

typedef struct {
    uint16_t len;
    uint8_t  reserved1,
             flags,
             reserved2[2],
             pad[2];
} IOCTL_GLOM_HDR;

typedef struct {
    IOCTL_GLOM_HDR glom_hdr;
    IOCTL_CMD  cmd;
} IOCTL_GLOM_CMD;

typedef struct
{
    uint16_t len,           // sdpcm_header.frametag
             notlen;
    union 
    {
        IOCTL_CMD cmd;
        IOCTL_GLOM_CMD glom_cmd;
    };
} IOCTL_MSG;

typedef struct {
    uint16_t len,       // sdpcm_header.frametag
             notlen;
    uint8_t  seq,       // sdpcm_sw_header
             chan,
             nextlen,
             hdrlen,
             flow,
             credit,
             reserved[2];
} IOCTL_EVENT_HDR;

#define SSID_MAXLEN         32

#define EVENT_SET_SSID      0
#define EVENT_JOIN          1
#define EVENT_AUTH          3
#define EVENT_LINK          16
#define EVENT_MAX           208
#define SET_EVENT(msk, e)   msk[e/8] |= 1 << (e & 7)

typedef struct {
    int num;
    char *str;
} EVT_STR;
#define EVT(e)      {e, #e}

#define NO_EVTS     {EVT(-1)}
#define ESCAN_EVTS  {EVT(WLC_E_ESCAN_RESULT), EVT(-1)}
#define JOIN_EVTS   {EVT(WLC_E_SET_SSID), EVT(WLC_E_LINK), EVT(WLC_E_AUTH), \
        EVT(WLC_E_DEAUTH_IND), EVT(WLC_E_DISASSOC_IND), EVT(WLC_E_PSK_SUP), EVT(-1)}

extern char ioctl_event_hdr_fields[];
extern int txglom;

int ioctl_get_event(IOCTL_EVENT_HDR *hp, uint8_t *data, int maxlen);
int ioctl_enable_evts(EVT_STR *evtp);
char *ioctl_evt_str(int event);
char *ioctl_evt_status_str(int status);
int ioctl_get_data(char *name, int wait_msec, uint8_t *data, int dlen);
int ioctl_set_uint32(char *name, int wait_msec, uint32_t val);
int ioctl_set_intx2(char *name, int wait_msec, int val1, int val2);
int ioctl_set_data(char *name, int wait_msec, void *data, int len);
int ioctl_wr_int32(int cmd, int wait_msec, int val);
int ioctl_wr_data(int cmd, int wait_msec, void *data, int len);
int ioctl_cmd(int cmd, char *name, int wait_msec, int wr, void *data, int dlen);
int ioctl_wait(int usec);
int ioctl_ready(void);
void disp_fields(void *data, char *fields, int maxlen);

// EOF


