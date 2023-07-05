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
#ifdef PLATFORM_HAS_POWER_SWITCH
#   define ADC_VREFINT_MAX 1638U   /* Derived from calculating (1.2V / 3.0V) * 4096 */
#   define ADC_VREFINT_MIN 1365U   /* Derived from calculating (1.2V / 3.6V) * 4096 */
#   define VREFINT_INTERVAL   10   /* in multiples of of systick intervals */
    static uint8_t monitor_ticks = VREFINT_INTERVAL;
#endif

void platform_timing_init(void)
{
    /* Setup heartbeat timer */
    systick_set_clocksource(STK_CSR_CLKSOURCE_AHB_DIV8);
    /* Interrupt us at 100 Hz */
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

#ifdef PLATFORM_HAS_POWER_SWITCH
    /* First check if target power is presently enabled */
    if (platform_target_get_power()) {
        /*
         * Every 10 systicks, set up an ADC conversion on the 9th tick, then
         * read back the value on the 10th, checking the internal bandgap reference
         * is still sat in the correct range. If it diverges down, this indicates
         * backfeeding and that VCC is being pulled higher than 3.3V. If it diverges
         * up, this indicates either backfeeding or overcurrent and that VCC is being
         * pulled below 3.3V. In either case, for safety, disable tpwr and set
         * a morse error of "TPWR ERROR"
         */
        if (monitor_ticks == 1) {
            /* On the tick prior to the one where we sample, start the bandgap conversion */
            uint8_t channel = ADC_CHANNEL_VREF;
            adc_set_regular_sequence(ADC1, 1, &channel);
            adc_start_conversion_direct(ADC1);
        } else if (monitor_ticks == 0) {
            /* When count exhausted, check the result of bandgap conversion */
            uint32_t ref = adc_read_regular(ADC1);
            /* Clear EOC bit. The GD32F103 does not automatically reset it on ADC read. */
            ADC_SR(ADC1) &= ~ADC_SR_EOC;
            monitor_ticks = VREFINT_INTERVAL;

            /* Now compare the reference against the known good range */
            if (ref > ADC_VREFINT_MAX || ref < ADC_VREFINT_MIN) {
                /* Something's wrong, so turn tpwr off and set the morse blink pattern */
                platform_target_set_power(false);
                morse("TPWR ERROR", true);
            }
        }
        --monitor_ticks;
    } else {
        /* Force clear EOC bit, if TPWR is disabled between start of conversion
           and reading the conversion */
        if (monitor_ticks == 0)
            ADC_SR(ADC1) &= ~ADC_SR_EOC;
        /* Allow for extra delay before testing the voltage for the first time */
        monitor_ticks = 2 * VREFINT_INTERVAL;
    }
#endif
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
