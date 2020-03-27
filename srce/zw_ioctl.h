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

#define IOCTL_WAIT_USEC 2000

#define IOCTL_GETVAR    262
#define IOCTL_SETVAR    263
#define IOCTL_MAX_DLEN  256

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
    uint8_t data[IOCTL_MAX_DLEN];
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

extern int txglom;

int ioctl_get_event(uint8_t *data, int maxlen);
int ioctl_getvar(char *name, uint8_t *data, int dlen);
int ioctl_set_uint32(char *name, uint32_t val);
int ioctl_set_data(char *name, void *data, int len);
int ioctl_cmd_int32(int cmd, int val);
int ioctl_cmd_data(int cmd, void *data, int len);
int ioctl_cmd(int wr, int cmd, char *name, void *data, int dlen);
int ioctl_wait(int usec);
int ioctl_ready(void);

// EOF


