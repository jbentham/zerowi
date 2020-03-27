// ZeroWi bare-metal WiFi driver, see https://iosoft.blog/zerowi
// Raspberry Pi GPIO interface definitions
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

#define USE_MMAP    0

#define REG_BASE    0x20000000      // Pi Zero
//#define REG_BASE    0x3F000000    // Pi 3

#define PAGE_SIZE       0x1000

#define CLOCK_KHZ       250000

#define GPIO_IN         0
#define GPIO_OUT        1
#define GPIO_ALT0       4
#define GPIO_ALT1       5
#define GPIO_ALT2       6
#define GPIO_ALT3       7
#define GPIO_ALT4       3
#define GPIO_ALT5       2

#define GPIO_NOPULL     0
#define GPIO_PULLDN     1
#define GPIO_PULLUP     2

void flash_open_read(int addr);
void flash_read(uint8_t *dp, int len);
void flash_close(void);

void flash_init(int khz);
void spi0_cs(int set);
void spi0_xfer(uint8_t *txd, uint8_t *rxd, int len);
void mmap_init(void);
void *mmap_regs(uint32_t addr, int len);
void disp_mem(uint8_t *data, int len);
void dump_mem(void *addr, int len);
void gpio_mmap(void);
void gpio_set(int pin, int mode, int pull);
void gpio_mode(int pin, int mode);
void gpio_pull(int pin, int pull);
void gpio_out(int pin, int val);
uint8_t gpio_in(int pin);
uint8_t gpio_read(int pin, int npins);
void gpio_write(int pin, int npins, uint32_t val);
int ustime(void);
void usdelay(int usec);
int ustimeout(int *tickp, int usec);

// EOF
