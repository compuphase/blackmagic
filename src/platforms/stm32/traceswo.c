/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2012  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * Copyright (C) 2023  CompuPhase, Thiadmer Riemersma
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


/* This file implements capture of the TRACESWO output, in "Manchester" mode
 * (also called RZ mode).
 *
 * More on the protocol is found in these references:
 * - ARM DDI 0403D - ARMv7M Architecture Reference Manual
 * - ARM DDI 0337I - Cortex-M3 Technical Reference Manual
 * - ARM DDI 0314H - CoreSight Components Technical Reference Manual
 *
 * The decoder is a state machine that acts on the transitions of the TRACESWO
 * pin. On the Black Magic Probe, this pin is connected to GPIO pin 6 of port A
 * ("PA6"). An external interrupt is set on this pin, for both rising and
 * falling edges. Precision time-stamps are also needed, for which we use the
 * cycle counter (CYCCNT, DWT unit).
 *
 * The ISR for PA6 stores the decoded bytes in a ring buffer. That buffer is
 * subsequently transmitted to the host over USB. Transfer is started by
 * traceswo_flush(). On completion of the transfer, the USB stack invokes a
 * callback, trace_buf_drain(), which continues the transfer if there is more
 * data in the ring buffer.
 *
 * traceswo_flush() must be called on a regular basis (otherwise, transfers will
 * never start). It is currently called from the "systick" ISR. In firmware
 * releases up to 1.9.1, the systick interrupt runs at 100 Hz, which is rather
 * slow. Therefore, I changed this to 1 kHz. There is already a merged PR in
 * the "mainline" (development line) of the original project that increases the
 * sysreq clock to 1 kHz too.
 *
 * Further development may focus on the error margin. In this implementation,
 * I chose a tolerance of 1/4th of the "half-bit" period, and this 12.5% of the
 * bit period. If you would relax the tolerance to 25% of the bit period, the
 * time spans for "start of bit" transitions and "mid-period" transitions touch.
 * There is no more time span where a transition is invalid. Thus, the ERROR
 * state gets redundant, and the calculations to determine the criterions for
 * the ERROR state are redundant as well. So the code can be simplified, and
 * run a little faster as a consequence.
 */

#include "general.h"
#include "cdcacm.h"
#include "traceswo.h"

#include <libopencm3/cm3/dwt.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/scs.h>
#include <libopencm3/stm32/exti.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>


static bool decoding = false;   /* SWO decoding enabled y/n */

/* The USB endpoint for TRACESWO is defined with a maximum packet size of 64.
   We use a ring buffer for intermediate storage, and for ease of implementation,
   its size must be a power of 2. Note that the common implementation of a ring
   buffer requires one extra (vacant) element (in order to distinguish a full
   queue from an empty queue), so the queue can hold one less item than its size.
   Hence, I chose a buffer size of 128, the next power-of-2 higher than 64. */
#define TRACEPACKET_SIZE  64    /* see cdcacm.c */
#define RINGBUFFER_SIZE   128   /* must be a power of 2 */
static uint8_t trace_buffer[RINGBUFFER_SIZE];
static unsigned trace_buf_read = 0;
static unsigned trace_buf_write = 0;
static bool trace_zlp = false;
static uint8_t traceswo_status_flags = 0;

static bool trace_buf_push(void)
{
    /* If there is only one producer (that writes data into the buffer) and one
       consumer (that reads & removes data), a ring buffer is a non-locking
       data structure. That means that no critical sections or semaphores are
       needed to protect against race conditions. This routine is called by
       traceswo_flush() and trace_buf_drain(), so technically, we have *two*
       consumers. Therefore, this routine is protected against nested execution. */
    static bool busy = false;
    if (__atomic_test_and_set(&busy, __ATOMIC_RELAXED))
        return false;

    /* Check how much data there is in the ring buffer, and limit it to the
       packet buffer size. */
    unsigned rd = trace_buf_read;
    unsigned wr = trace_buf_write;
    unsigned sz = (wr >= rd) ? wr - rd : RINGBUFFER_SIZE - rd + wr;
    if (sz > TRACEPACKET_SIZE)
        sz = TRACEPACKET_SIZE;

    /* Copy the ring buffer into a local (linear) buffer. */
    uint8_t buf[TRACEPACKET_SIZE] __attribute__((aligned(4)));
    unsigned next_rd = (rd + sz) & (RINGBUFFER_SIZE - 1);
    if (next_rd >= rd) {
        memcpy(buf, trace_buffer + rd, next_rd - rd);
    } else {
        unsigned tail_sz = RINGBUFFER_SIZE - rd;
        memcpy(buf, trace_buffer + rd, tail_sz);
        memcpy(buf + tail_sz, trace_buffer, next_rd);
    }

    /* Clear the number of copied bytes from ring buffer. */
    trace_buf_read = next_rd;

    /* Transmit the local copy. */
    if (decoding)
        traceswo_decode(usbdev, CDCACM_UART_DATA_EP, buf, sz);
    else
        usbd_ep_write_packet(usbdev, TRACE_IN_EP, buf, sz);

    __atomic_clear(&busy, __ATOMIC_RELAXED);

    /* If sending a full size packet, the host may cache the received data
       internally (in the assumption that more data follows), instead of passing
       it on to the application. A zero-length packet (ZLP) is then needed to
       signal the receiving host that the data is complete. Here, we just mark
       that a ZLP may be needed; it is up to the caller to handle it. */
    return (sz == TRACEPACKET_SIZE);
}

void trace_buf_drain(usbd_device *dev, uint8_t ep)
{
    /* The USB stack calls trace_buf_drain() after completing a transmit (after
       the remote host acknowledged reception). This is therefore a good moment
       to transmit a next packet, if more data is waiting. */
    (void)dev;
    (void)ep;
    if (trace_buf_read != trace_buf_write || trace_zlp)
        trace_zlp = trace_buf_push();
}

void traceswo_flush(void)
{
    /* traceswo_flush() is called on a systick timer. It starts transmitting a
       packet, if there is data in the ring buffer. */
    if (trace_buf_read != trace_buf_write)
        trace_zlp = trace_buf_push();
}

static bool enable_cycle_counter(void)
{
    if (DWT_CTRL & DWT_CTRL_NOCYCCNT)
        return false;
    SCS_DEMCR |= SCS_DEMCR_TRCENA;
    DWT_CTRL |= DWT_CTRL_CYCCNTENA;
    return true;
}

static inline uint32_t read_cycle_counter(void)
{
    return DWT_CYCCNT;
}

static inline void __exti_reset_request(uint32_t extis)
{
    /* overrule the default implementation in libopencm3 with an inline function,
       to maximize performance */
    EXTI_PR = extis;
}


/* Manchester decoding allows clock recovery, but that hinges on the decoding
   of the start bit. If the TRACESWO pin toggles during initialization, those
   "glitches" may be mis-interpreted as a start bit. This might be a very long
   start bit, causing the error reset time to be very long as well. To counter
   this, we set a maximum on the bit period duration. This does imply that we
   now impose in minimum bitrate on Manchester.
   (Actually, the limit is on half the bitrate.) */
#define BMP_CLOCKFREQ   72000000                /* BMP runs at 72 MHz (we use the
                                                   DWT cycle counter as the timer) */
#define STARTBIT_LIMIT  (BMP_CLOCKFREQ / 2000)  /* 500 microseconds for a half-bit
                                                   -> minimum bitrate = 1000 bps */

/* Manchester decoding is based on a state machine. There are four states. */
enum {
  IDLE,
  STARTBIT,
  DECODING,
  ERROR
};

void exti9_5_isr(void)  /*EXTI9_5_IRQHandler*/
{
    #define RISING_EDGE()   (GPIOA_IDR & GPIO6)
    #define FALLING_EDGE()  (!RISING_EDGE())

    /* Apart from the state, a number of other variables have to be maintained,
       such as the detected clock rate and the criterions derived from that
       clock rate. */
    static int state = IDLE;
    static uint32_t halfperiod = 0;             /* the bit period is twice this value */
    static uint32_t sync_mark = 0;              /* timestamp mark (timestamp of previous transition) */
    static uint32_t space_criterion;            /* when the time between flanks is too long, restart decoding (from implied idle state) */
    static uint32_t bitstart_low, bitstart_high;/* interval in which the transition at the start of a bit is expected */
    static uint32_t bitmid_low, bitmid_high;    /* interval in which the transition halfway a bit is expected */
    static unsigned char byte, bitmask;         /* holds the bits of a partially decoded byte */

    uint32_t timestamp = read_cycle_counter();  /* the timestamp */
    uint32_t deltatime = timestamp - sync_mark; /* unsigned arithmetic is required, to handle wrap-around */

    switch (state) {
    case IDLE:
        /* TRACESWO specfies the idle state as "low", so we only act on "up"
           transitions. The "up" transition is the start flank of the start-bit.
           The time is marked only to measure the length of the pulse of the
           start bit. */
        if (RISING_EDGE()) {
            sync_mark = timestamp;
            state = STARTBIT;
        }
        break;

    case STARTBIT:
        /* The start bit is a "1" bit. Therefore, it has a "down" transition
           halfway its bit period. We measure this period, and then calculate
           the criterions for data bits & space (return to idle) conditions. */
        if (FALLING_EDGE()) {
            /* We are coming from IDLE, so the measured delta-time between the
               "up" flank of at the IDLE state and the "down" flank of this
               stage, is half the bit period. */
            halfperiod = deltatime;
            if (halfperiod > STARTBIT_LIMIT)
                state = IDLE;   /* start bit length out of range, return to wait
                                   for a (valid) start bit */
            uint32_t bitperiod = halfperiod << 1;
            /* A "space" has a duration of at least 1.5 bit period. After a
               space, the decoding restarts with a start bit. */
            /* The next "up" or "down" transition halfway the bit period is
               expected a "bitperiod" after this transition. However, we apply
               a margin of 1/4th of the "halfperiod" (1/8th of the bit period,
               or 12.5%).
               Note, however, that there may be (or may not be) a transition on
               the start of a bit. Those transitions need to be ignored.
               In summary:
                 - A transition arriving after bitperiod/2 +/- margin, is the
                   start of the next bit; it need not be present and should be
                   ignored.
                 - A transition arriving after bitperiod +/- margin, is the
                   "halfway" transition of the bit, which we need to act on.
                 - An "up" transition arriving after 1.5*bitperiod - margin
                   means that a space has passed, and that this is the first
                   flank of the new start bit.
                 - Everything else is an error. */
            uint32_t margin = halfperiod >> 2;  /* halfperiod divided by 4 */
            bitstart_low = halfperiod - margin; /* criterion for flank at start of a bit, low mark */
            bitstart_high = halfperiod + margin;/* criterion for flank at start of a bit, high mark */
            bitmid_low = bitperiod - margin;    /* criterion for flank halfway a bit, low mark */
            bitmid_high = bitperiod + margin;   /* criterion for flank halfway a bit, high mark */
            space_criterion = bitperiod + halfperiod - margin;
            sync_mark = timestamp;
            byte = 0;
            bitmask = 0x01;
            state = DECODING;
        }
        break;

    case DECODING:
        if (deltatime >= bitmid_low && deltatime <= bitmid_high) {
            if (FALLING_EDGE())
                byte |= bitmask;    /* set bit when there's a falling edge halfway  */
            if ((bitmask <<= 1) == 0) {
                /* Done 8 bits -> store byte in the ring buffer. */
                unsigned next_wr = (trace_buf_write + 1) & (RINGBUFFER_SIZE - 1);
                if (next_wr == trace_buf_read) {
                    /* queue is full */
                    traceswo_status_flags |= SWOFLAG_BUFFER_FULL;
                } else {
                    trace_buffer[trace_buf_write] = byte;
                    trace_buf_write = next_wr;
                }
                /* reset, prepare to decode next byte */
                byte = 0;
                bitmask = 0x01;
            }
            /* TRACESWO always restarts after 64 data bits. There is hence no
               need to adjust for clock drift. However, we synchronize on the
               transitions halfway the bit period, and those flanks may come a
               little late or a little early. So we have two options here:
                 - sync_mark = sync_mark + bitperiod (the theoretical time)
                 - sync_mark = sync_mark + deltatime (which amounts to using
                   the current timestamp as the synchronization point)
               The middle ground is to take the average of these two. */
            sync_mark += halfperiod + (deltatime >> 1);
        } else if (deltatime >= bitstart_low && deltatime <= bitstart_high) {
            /* As said, there is always a transition halfway a bit period, but
               not necessarily one at the start of a bit. Therefore, it is much
               easier to synchronize on the halfperiod transitions. So we just
               completely ignore any potential transition at the start of a bit
               (if there is any) */
        } else if (deltatime >= space_criterion && RISING_EDGE()) {
            sync_mark = timestamp;
            state = STARTBIT; /* restart when idle for at least 1.5 bit cycle */
        } else {
            sync_mark = timestamp;
            state = ERROR;
            traceswo_status_flags |= SWOFLAG_DECODE_ERROR;  /* flag that error state has been entered */
        }
        break;

    case ERROR:
        if (RISING_EDGE() && deltatime >= space_criterion)
            state = STARTBIT; /* restart when idle for at least 1.5 bit period */
        /* The synchronization mark is only used here to detect idle time for at
           least 1.5 bit period (in order to restart). Note that a "space" may
           be as short as a bit period. So by insisting on 1.5 bit period (minus
           a margin), we may not pick up the decoding at the earliest possible
           time. However, as there is only guaranteed to be a transition halfway
           every bit period, the time between two transitions can be one bit
           period: the same as the shortest duration of a space. Thus (and since
           we have lost synchronization to the bit stream), we cannot distinguish
           a short space from a gap between transitions in a normal stream. This
           is why, once we entered "error" state, we must wait for a longer
           space. */
        sync_mark = timestamp;
        /* If the period between flanks is smaller than the current halfbit
           time, it is likely that we dropped halfway into a stream and that we
           mistook a full bit period for the halfbit of the start bit. Hence,
           reduce the halfbit time and recalculate the space_criterion. */
        if (deltatime < halfperiod) {
            halfperiod = deltatime;
            space_criterion = (halfperiod << 1) + halfperiod + (halfperiod >> 2);
                              /* ^^^ = bitperiod */            /* ^^^ = margin */
        }
        break;
    }

    __exti_reset_request(EXTI6);
}

bool traceswo_init(uint32_t swo_chan_bitmask)
{
    /* Make sure to clear the buffer. */
    trace_buf_read = 0;
    trace_buf_write = 0;
    trace_zlp = false;
    traceswo_status_flags = 0;

    /* Set up the TRACESWO pin for interrupt on both rising and falling edge. */
    rcc_periph_clock_enable(RCC_GPIOA); /* enable GPIOA clock, if not already enabled */
    rcc_periph_clock_enable(RCC_AFIO);  /* enable AFIO clock for external interrupts */

    /* Note: TRACESWO is GPIO6 at port GPIOA, set it to input and set an
       interrupt trigger onto it; external interrupts 5 to 9 share a level. */
    gpio_set_mode(GPIOA, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, GPIO6);
    exti_select_source(EXTI6, GPIOA);
    exti_set_trigger(EXTI6, EXTI_TRIGGER_BOTH);
    exti_enable_request(EXTI6);
    nvic_set_priority(NVIC_EXTI9_5_IRQ, IRQ_PRI_TRACE); /* high priority */
    nvic_enable_irq(NVIC_EXTI9_5_IRQ);

    /* Use CYCCNT for timestamp (free running counter).
       Porting note: Cortex M0 MCUs may not provide the cycle counter. */
    if (!enable_cycle_counter())
        return false;

    traceswo_setmask(swo_chan_bitmask);
    decoding = (swo_chan_bitmask != 0);
    return true;
}

void traceswo_close(void)
{
    exti_disable_request(EXTI6);
    nvic_disable_irq(NVIC_EXTI9_5_IRQ);
    /* We leave the cycle counter (CYCCNT) enabled, because it may also be used
       for profiling or other functions. */
}

uint8_t traceswo_status(void)
{
    uint32_t result = traceswo_status_flags;
    traceswo_status_flags = 0;
    if (nvic_get_irq_enabled(NVIC_EXTI9_5_IRQ))
        result |= SWOFLAG_ACTIVE;
    return result;
}
