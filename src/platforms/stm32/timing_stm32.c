/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2015 Gareth McMullin <gareth@blacksphere.co.nz>
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
#include "general.h"
#include "morse.h"

#include <libopencm3/cm3/systick.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/adc.h>

static volatile uint32_t time_ms;
uint32_t swd_delay_cnt = 0;
uint8_t running_status = 0;

static int morse_tick = 0;

void platform_timing_init(void)
{
    /* Setup heartbeat timer */
    systick_set_clocksource(STK_CSR_CLKSOURCE_AHB_DIV8);
    /* Interrupt us at 1 kHz */
    systick_set_reload(rcc_ahb_frequency / (8 * SYSTICKHZ) - 1);
    /* SYSTICK_IRQ with low priority */
    nvic_set_priority(NVIC_SYSTICK_IRQ, 14 << 4);
    systick_interrupt_enable();
    systick_counter_enable();
}

void platform_delay(uint32_t ms)
{
    platform_timeout timeout;
    platform_timeout_set(&timeout, ms);
    while (!platform_timeout_is_expired(&timeout));
}

void sys_tick_handler(void)
{
    time_ms += SYSTICKMS;

    if (morse_tick >= MORSECNT) {
        if (running_status)
            gpio_toggle(LED_PORT, LED_IDLE_RUN);
        SET_ERROR_STATE(morse_update());
        morse_tick = 0;
    } else {
        morse_tick++;
    }
}

uint32_t platform_time_ms(void)
{
    return time_ms;
}

/* Assume CYCLES_PER_BIT CPU cycles per SWD clock cycle (1 bit pushed out)
 * Assume DELAY_LOOP_CYCLES cycles per delay loop, with 2 delay loops per SWD clock
 */

/* Values for STM32F103 at 72 MHz */
#define CYCLES_PER_BIT      16
#define DELAY_LOOP_CYCLES   8
void platform_max_frequency_set(uint32_t freq)
{
    if (CYCLES_PER_BIT * freq >= rcc_ahb_frequency) {
        swd_delay_cnt = 0;
        return;
    }
    int divisor = (rcc_ahb_frequency - CYCLES_PER_BIT * freq) / 2;
    /* divide & round upwards */
    swd_delay_cnt = (divisor + (DELAY_LOOP_CYCLES * freq - 1)) / (DELAY_LOOP_CYCLES * freq);
}

uint32_t platform_max_frequency_get(void)
{
    return rcc_ahb_frequency / (CYCLES_PER_BIT + 2 * DELAY_LOOP_CYCLES * swd_delay_cnt);
}
