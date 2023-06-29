/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2011  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* This file implements the SW-DP interface. */

#include "general.h"
#include "timing.h"
#include "adiv5.h"

enum {
    SWDIO_STATUS_FLOAT = 0,
    SWDIO_STATUS_DRIVE
};
static void swdptap_turnaround(int dir) __attribute__ ((optimize(3)));
static uint32_t swdptap_seq_in(int ticks) __attribute__ ((optimize(3)));
static bool swdptap_seq_in_parity(uint32_t *value, int ticks)
    __attribute__ ((optimize(3)));
static void swdptap_seq_out(uint32_t MS, int ticks)
    __attribute__ ((optimize(3)));
static void swdptap_seq_out_parity(uint32_t MS, int ticks)
    __attribute__ ((optimize(3)));


#define __nop()   __asm__("nop")  /* single cycle delay */

__attribute__( ( always_inline ) ) static inline void __cycle_delay(uint32_t cycles)
{
  __asm__ volatile (
        "mov    r0, %0\n"
    "1:\n"
        "sub    r0, #1\n"
        "cmp    r0, #0\n"
        "bgt    1b\n"
      :
      : "r" (cycles)
      : "r0", "memory");
}


static void swdptap_turnaround(int dir)
{
    static int olddir = SWDIO_STATUS_FLOAT;

    /* Don't turnaround if direction not changing */
    if (dir == olddir)
        return;
    olddir = dir;

#ifdef DEBUG_SWD_BITS
    DEBUG("%s", dir ? "\n-> ":"\n<- ");
#endif

    if (dir == SWDIO_STATUS_FLOAT)
        SWDIO_MODE_FLOAT();

    /* during the clock cycle, SWDIO is guaranteed to float:
       - either dir == SWDIO_STATUS_FLOAT, in which case it was just set to tri-state
       - or dir == SWDIO_STATUS_DRIVE, meaning that it already was floating on entry */
    gpio_set(SWCLK_PORT, SWCLK_PIN);
    __cycle_delay(swd_delay_cnt);
    gpio_clear(SWCLK_PORT, SWCLK_PIN);
    __cycle_delay(swd_delay_cnt);

    if (dir == SWDIO_STATUS_DRIVE)
        SWDIO_MODE_DRIVE();
}

static uint32_t swdptap_seq_in(int clocks)
{
#ifdef DEBUG_SWD_BITS
    int org_clocks = clocks;
#endif
    uint32_t mask = 1;
    uint32_t ret = 0;

    swdptap_turnaround(SWDIO_STATUS_FLOAT);
    if (swd_delay_cnt) {
        while (clocks--) {
            gpio_clear(SWCLK_PORT, SWCLK_PIN);
            __cycle_delay(swd_delay_cnt);
            if (gpio_get(SWDIO_PORT, SWDIO_PIN))
                ret |= mask;
            gpio_set(SWCLK_PORT, SWCLK_PIN);
            __cycle_delay(swd_delay_cnt);
            mask <<= 1;
        }
    } else {
        while (clocks--) {
            gpio_clear(SWCLK_PORT, SWCLK_PIN);
            if (gpio_get(SWDIO_PORT, SWDIO_PIN))
                ret |= mask;
            gpio_set(SWCLK_PORT, SWCLK_PIN);
            mask <<= 1;
        }
    }
    gpio_clear(SWCLK_PORT, SWCLK_PIN);
#ifdef DEBUG_SWD_BITS
    for (int i = 0; i < org_clocks; i++)
        DEBUG("%d", (ret & (1 << i)) ? 1 : 0);
#endif
    return ret;
}

static bool swdptap_seq_in_parity(uint32_t *value, int clocks)
{
#ifdef DEBUG_SWD_BITS
    int org_clocks = clocks;
#endif
    uint32_t mask = 1;
    uint32_t ret = 0;

    swdptap_turnaround(SWDIO_STATUS_FLOAT);
    if (swd_delay_cnt) {
        while (clocks--) {
            gpio_clear(SWCLK_PORT, SWCLK_PIN);
            if (gpio_get(SWDIO_PORT, SWDIO_PIN))
                ret |= mask;
            __cycle_delay(swd_delay_cnt);
            gpio_set(SWCLK_PORT, SWCLK_PIN);
            __cycle_delay(swd_delay_cnt);
            mask <<= 1;
        }
    } else {
        while (clocks--) {
            gpio_clear(SWCLK_PORT, SWCLK_PIN);
            if (gpio_get(SWDIO_PORT, SWDIO_PIN))
                ret |= mask;
            gpio_set(SWCLK_PORT, SWCLK_PIN);
            mask <<= 1;
        }
    }
    int parity = __builtin_popcount(ret);
    gpio_clear(SWCLK_PORT, SWCLK_PIN);
    if (gpio_get(SWDIO_PORT, SWDIO_PIN))
        parity += 1;
    __cycle_delay(swd_delay_cnt);
    gpio_set(SWCLK_PORT, SWCLK_PIN);
    __cycle_delay(swd_delay_cnt);
#ifdef DEBUG_SWD_BITS
    for (int i = 0; i < org_clocks; i++)
        DEBUG("%d", (ret & (1 << i)) ? 1 : 0);
#endif
    *value = ret;
    /* Terminate the read cycle now */
    gpio_clear(SWCLK_PORT, SWCLK_PIN);
    __cycle_delay(swd_delay_cnt);
    swdptap_turnaround(SWDIO_STATUS_DRIVE);
    return (parity & 1);
}

static void swdptap_seq_out(uint32_t MS, int ticks)
{
#ifdef DEBUG_SWD_BITS
    for (int i = 0; i < ticks; i++)
        DEBUG("%d", (MS & (1 << i)) ? 1 : 0);
#endif
    swdptap_turnaround(SWDIO_STATUS_DRIVE);
    if (swd_delay_cnt) {
        while (ticks--) {
            gpio_clear(SWCLK_PORT, SWCLK_PIN);
            __cycle_delay(swd_delay_cnt);
            gpio_set_val(SWDIO_PORT, SWDIO_PIN, MS & 1);
            gpio_set(SWCLK_PORT, SWCLK_PIN);
            __cycle_delay(swd_delay_cnt);
            MS >>= 1;
        }
    } else {
        while (ticks--) {
            gpio_clear(SWCLK_PORT, SWCLK_PIN);
            gpio_set_val(SWDIO_PORT, SWDIO_PIN, MS & 1);
            gpio_set(SWCLK_PORT, SWCLK_PIN);
            MS >>= 1;
        }
    }
    gpio_clear(SWCLK_PORT, SWCLK_PIN);
}

static void swdptap_seq_out_parity(uint32_t MS, int ticks)
{
#ifdef DEBUG_SWD_BITS
    for (int i = 0; i < ticks; i++)
        DEBUG("%d", (MS & (1 << i)) ? 1 : 0);
#endif
    int parity = __builtin_popcount(MS);    /* calculate first, "bits" variable is changed in the loop */
    swdptap_turnaround(SWDIO_STATUS_DRIVE);
    if (swd_delay_cnt) {
        while (ticks--) {
            gpio_clear(SWCLK_PORT, SWCLK_PIN);
            __cycle_delay(swd_delay_cnt);
            gpio_set_val(SWDIO_PORT, SWDIO_PIN, MS & 1);
            gpio_set(SWCLK_PORT, SWCLK_PIN);
            __cycle_delay(swd_delay_cnt);
            MS >>= 1;
        }
        gpio_clear(SWCLK_PORT, SWCLK_PIN);
        __cycle_delay(swd_delay_cnt);
    } else {
        while (ticks--) {
            gpio_clear(SWCLK_PORT, SWCLK_PIN);
            gpio_set_val(SWDIO_PORT, SWDIO_PIN, MS & 1);
            gpio_set(SWCLK_PORT, SWCLK_PIN);
            MS >>= 1;
        }
        gpio_clear(SWCLK_PORT, SWCLK_PIN);
    }
    gpio_set_val(SWDIO_PORT, SWDIO_PIN, parity & 1);
    gpio_set(SWCLK_PORT, SWCLK_PIN);
    __cycle_delay(swd_delay_cnt);
    gpio_clear(SWCLK_PORT, SWCLK_PIN);
}

int swdptap_init(ADIv5_DP_t *dp)
{
    dp->seq_in  = swdptap_seq_in;
    dp->seq_in_parity  = swdptap_seq_in_parity;
    dp->seq_out = swdptap_seq_out;
    dp->seq_out_parity  = swdptap_seq_out_parity;

    return 0;
}
