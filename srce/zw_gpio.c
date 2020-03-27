// ZeroWi bare-metal WiFi driver, see https://iosoft.blog/zerowi
// Raspberry Pi GPIO interface
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
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include "zw_gpio.h"

#define INCLUDE_MAIN    0

#if USE_MMAP
#include <sys/mman.h>
#endif

#define PAGE_SIZE       0x1000

#define USEC_BASE       (REG_BASE + 0x3000)
#define USEC_SIZE       PAGE_SIZE

#define GPIO_BASE       (REG_BASE + 0x200000)
#define GPIO_SIZE       0x20000
#define GPIO_MODE0      (uint32_t *)GPIO_BASE
#define GPIO_SET0       (uint32_t *)(GPIO_BASE + 0x1c)
#define GPIO_CLR0       (uint32_t *)(GPIO_BASE + 0x28)
#define GPIO_LEV0       (uint32_t *)(GPIO_BASE + 0x34)
#define GPIO_GPPUD      (uint32_t *)(GPIO_BASE + 0x94)
#define GPIO_GPPUDCLK0  (uint32_t *)(GPIO_BASE + 0x98)

#define SPI0_BASE       (REG_BASE + 0x204000)
#define SPI0_CS         (uint32_t *)SPI0_BASE
#define SPI0_FIFO       (uint32_t *)(SPI0_BASE + 0x04)
#define SPI0_CLK        (uint32_t *)(SPI0_BASE + 0x08)
#define SPI0_DLEN       (uint32_t *)(SPI0_BASE + 0x0c)
#define SPI0_DC         (uint32_t *)(SPI0_BASE + 0x14)

#if USE_MMAP
#define GPIO_REG(a)     ((uint32_t *)((uint32_t)a - GPIO_BASE + (uint32_t)gpio_block))
#define USEC_REG()      ((uint32_t *)(usec_block+4))
#else
#define GPIO_REG(a)     ((uint32_t *)a)
#define USEC_REG()      ((uint32_t *)(USEC_BASE+4))
#endif

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

#define LED_PIN         47
#define SPI0_CE0_PIN    8
#define SPI0_MISO_PIN   9
#define SPI0_MOSI_PIN   10
#define SPI0_SCLK_PIN   11

volatile void *gpio_block, *usec_block;

#if INCLUDE_MAIN
int main(int argc, char **argv)
{
    int ticks=0, ledon=0;
    uint8_t rxdata[100];

    gpio_set(LED_PIN, GPIO_OUT, GPIO_NOPULL);
    gpio_out(LED_PIN, 0);
    spi0_init(10000);
    ustimeout(&ticks, 0);
    while (1)
    {
        if (ustimeout(&ticks, 500000))
        {
            gpio_out(LED_PIN, ledon = !ledon);
            flash_open_read(4);
            flash_read(rxdata, 10);
            flash_close();
            disp_mem(rxdata, 10);
            printf("\n");
            fflush(stdout);
        }
    }
    return(0);
}
#endif

// Start a flash read cycle (EN25Q80 device)
void flash_open_read(int addr)
{
    uint8_t rxdata[4], txdata[4]={3, (uint8_t)(addr>>16), (uint8_t)(addr>>8), (uint8_t)(addr)};
    
    spi0_cs(1);
    spi0_xfer(txdata, rxdata, 4);
}
// Read next block
void flash_read(uint8_t *dp, int len)
{
    while (len--)
    {
        *SPI0_FIFO = 0;
        while((*SPI0_CS & (1<<17)) == 0) ;
        *dp++ = *SPI0_FIFO;
    }
}
// End a flash ycle
void flash_close(void)
{
    spi0_cs(0);
}

// Initialise flash interface (SPI0)
void flash_init(int khz)
{
    gpio_set(SPI0_CE0_PIN, GPIO_ALT0, GPIO_NOPULL);
    gpio_set(SPI0_MISO_PIN, GPIO_ALT0, GPIO_PULLUP);
    gpio_set(SPI0_MOSI_PIN, GPIO_ALT0, GPIO_NOPULL);
    gpio_set(SPI0_SCLK_PIN, GPIO_ALT0, GPIO_NOPULL);
    *SPI0_CS = 0x30;
    *SPI0_CLK = CLOCK_KHZ / khz;
}

// Set / clear SPI chip select
void spi0_cs(int set)
{
    *SPI0_CS = set ? *SPI0_CS | 0x80 : *SPI0_CS & ~0x80;
}

// Transfer 1 SPI byte
void spi0_xfer(uint8_t *txd, uint8_t *rxd, int len)
{
    while (len--)
    {
        *SPI0_FIFO = *txd++;
        while((*SPI0_CS & (1<<17)) == 0) ;
        *rxd++ = *SPI0_FIFO;
    }
}

// Initialise GPIO and usec I/O blocks
void mmap_init(void)
{
#if USE_MMAP
    gpio_block = mmap_regs(GPIO_BASE, GPIO_SIZE);
    usec_block = mmap_regs(USEC_BASE, USEC_SIZE);
#endif    
}

// Display memory
void disp_mem(uint8_t *data, int len)
{
    int i;
    
    for (i=0; i<len; i++)
        printf("%02X ", data[i]);
}

// Dump memory area
void dump_mem(void *addr, int len)
{
    int i;

    for (i = 0; i<len; i++, addr+=4)
    {
        if (i%8 == 0)
            printf("\n%04lX:", (uint32_t)addr);
        printf(" %08lX", *GPIO_REG(addr));
    }
}

#if USE_MMAP
// Map a register area for read/write access
void *mmap_regs(uint32_t addr, int len)
{
    void *mem, *block;
    int fd;

    if ((mem = malloc(len+PAGE_SIZE)) == NULL)
    {
        printf("allocation error \n");
        exit(1);
    }
    mem += PAGE_SIZE - ((uint32_t)mem % PAGE_SIZE);
    if ((fd = open("/dev/mem", O_RDWR | O_SYNC)) < 0)
    {
        printf("can't open /dev/mem \n");
        exit(1);
    }
    block = mmap(mem, len, PROT_READ | PROT_WRITE,
                           MAP_SHARED|MAP_FIXED, fd, addr);
    close(fd);
    if (block == MAP_FAILED)
    {
        printf("mmap failed\n");
        exit(1);
    }
    return(block);
}
#endif    

// Set input or output with pullups
void gpio_set(int pin, int mode, int pull)
{
    gpio_mode(pin, mode);
    gpio_pull(pin, pull);
}

// Set input or output
void gpio_mode(int pin, int mode)
{
    uint32_t *reg = GPIO_REG(GPIO_MODE0) + pin / 10, shift = (pin % 10) * 3;

    *reg = (*reg & ~(7 << shift)) | (mode << shift);
}

// Set I/P pullup or pulldown
void gpio_pull(int pin, int pull)
{
    uint32_t *reg = GPIO_REG(GPIO_GPPUDCLK0) + pin / 32;

    *GPIO_REG(GPIO_GPPUD) = pull;
    usdelay(2);
    *reg = pin << (pin % 32);
    usdelay(2);
    *GPIO_REG(GPIO_GPPUD) = 0;
    *reg = 0;
}

// Set an O/P pin
void gpio_out(int pin, int val)
{
    uint32_t *reg = (val ? GPIO_REG(GPIO_SET0) : GPIO_REG(GPIO_CLR0)) + pin/32;

    *reg = 1 << (pin % 32);
}

// Get an I/P pin value
uint8_t gpio_in(int pin)
{
    uint32_t *reg = GPIO_REG(GPIO_LEV0) + pin/32;

    return (((*reg) >> (pin % 32)) & 1);
}

// Set value on multiple O/P pins (on the same 32-bit port)
void gpio_write(int pin, int npins, uint32_t val)
{
    uint32_t *clr=GPIO_REG(GPIO_CLR0)+pin/32, *set=GPIO_REG(GPIO_SET0)+pin/32;
    
    *set = val << (pin % 32);
    *clr = ((~val) & ((1<<npins)-1)) << (pin % 32);
}

// Get byte value from multiple I/P pins
uint8_t gpio_read(int pin, int npins)
{
    uint32_t *reg = GPIO_REG(GPIO_LEV0) + pin/32;

    return(((*reg) >> (pin % 32)) & ((1 << npins) - 1));
}

// Return timer tick value in microseconds
int ustime(void)
{
    return(*USEC_REG());
}

// Delay given number of microseconds
void usdelay(int usec)
{
    int ticks;

    ustimeout(&ticks, 0);
    while (!ustimeout(&ticks, usec)) ;
}

// Return non-zero if timeout
int ustimeout(int *tickp, int usec)
{
    int t = *USEC_REG();

    if (usec == 0 || t - *tickp >= usec)
    {
        *tickp = t;
        return (1);
    }
    return (0);
}

// EOF
