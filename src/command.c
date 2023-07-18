/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2011  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 * Copyright (C) 2021 Uwe Bonnes
 *                            (bon@elektron.ikp.physik.tu-darmstadt.de)
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

/* This file implements a basic command interpreter for GDB 'monitor'
 * commands.
 */

#include <ctype.h>
#include "general.h"
#include "exception.h"
#include "command.h"
#include "gdb_packet.h"
#include "target.h"
#include "target_internal.h"
#include "morse.h"
#include "version.h"
#include "serialno.h"
#include "jtagtap.h"

#ifdef ENABLE_RTT
#   include "rtt.h"
#endif
#ifdef PLATFORM_HAS_TRACESWO
#   include "traceswo.h"
#endif

static bool cmd_version(target *t, int argc, const char **argv);
#ifdef PLATFORM_HAS_PRINTSERIAL
static bool cmd_serial(target *t, int argc, const char **argv);
#endif
static bool cmd_help(target *t, int argc, const char **argv);

static bool cmd_jtag_scan(target *t, int argc, const char **argv);
static bool cmd_swdp_scan(target *t, int argc, const char **argv);
static bool cmd_auto_scan(target *t, int argc, const char **argv);
static bool cmd_frequency(target *t, int argc, const char **argv);
static bool cmd_targets(target *t, int argc, const char **argv);
static bool cmd_morse(target *t, int argc, const char **argv);
static bool cmd_halt_timeout(target *t, int argc, const char **argv);
static bool cmd_connect_reset(target *t, int argc, const char **argv);
static bool cmd_reset(target *t, int argc, const char **argv);
static bool cmd_tdi_low_reset(target *t, int argc, const char **argv);
#ifdef PLATFORM_HAS_POWER_SWITCH
static bool cmd_target_power(target *t, int argc, const char **argv);
#endif
#ifdef PLATFORM_HAS_TRACESWO
static bool cmd_traceswo(target *t, int argc, const char **argv);
#endif
static bool cmd_heapinfo(target *t, int argc, const char **argv);
#ifdef ENABLE_RTT
static bool cmd_rtt(target *t, int argc, const char **argv);
#endif
#if defined(PLATFORM_HAS_DEBUG) && (PC_HOSTED == 0)
static bool cmd_debug_bmp(target *t, int argc, const char **argv);
#endif
#ifdef PLATFORM_HAS_UART_WHEN_SWDP
static bool cmd_convert_tdio(target *t, int argc, const char **argv);
#endif

const struct command_s cmd_list[] = {
    {"version", cmd_version, "Display firmware version info"},
#ifdef PLATFORM_HAS_PRINTSERIAL
    {"serial", cmd_serial, "Display probe hardware serial number"},
#endif
    {"help", cmd_help, "Display help for monitor commands"},
    {"jtag_scan", cmd_jtag_scan, "Scan JTAG chain for devices" },
    {"swdp_scan", cmd_swdp_scan, "Scan SW-DP for devices: [target id]" },
    {"auto_scan", cmd_auto_scan, "Automatically scan all chain types for devices"},
    {"frequency", cmd_frequency, "limit SWJ clock frequency: [frequency]" },
    {"targets", cmd_targets, "Display list of available targets" },
    {"morse", cmd_morse, "Display morse error message" },
    {"halt_timeout", cmd_halt_timeout, "Timeout to wait until Cortex-M is halted: [timeout, default 2000ms]" },
    {"connect_rst", cmd_connect_reset, "Configure connect under reset: [enable|disable]" },
    {"reset", cmd_reset, "Pulse the nRST line - disconnects target: [pulse length (ms)|enable|disable])" },
    {"tdi_low_reset", cmd_tdi_low_reset,
        "Pulse nRST with TDI set low to attempt to wake certain targets up (eg LPC82x)"},
#ifdef PLATFORM_HAS_POWER_SWITCH
    {"tpwr", cmd_target_power, "Supplies power to the target: [enable|disable]"},
#endif
#ifdef ENABLE_RTT
    {"rtt", cmd_rtt, "[enable|disable|status|channel [0..15]|ident [name]|cblock|ram|poll [maxms minms maxerr]]"},
#endif
#ifdef PLATFORM_HAS_TRACESWO
#  if defined TRACESWO_PROTOCOL && TRACESWO_PROTOCOL == 2
    {"traceswo", cmd_traceswo, "Start trace capture, NRZ mode: [baudrate] [decode channel ...]" },
#  else
    {"traceswo", cmd_traceswo, "Start trace capture, Manchester mode: [decode channel ...]" },
#  endif
#endif
    {"heapinfo", cmd_heapinfo, "Set semihosting heapinfo: [heap_base heap_limit stack_base stack_limit]" },
#if defined(PLATFORM_HAS_DEBUG) && PC_HOSTED == 0
    {"debug_bmp", cmd_debug_bmp, "Output BMP \"debug\" strings to the secondary UART: [enable|disable]"},
#endif
#ifdef PLATFORM_HAS_UART_WHEN_SWDP
    {"convert_tdio", cmd_convert_tdio,"Switch TDI/O pins to UART TX/RX functions: [enable|disable]"},
#endif
    {NULL, NULL, NULL}
};

static const char command_format_error[] = "Unrecognized command format\n";

bool connect_assert_nrst;
#if defined(PLATFORM_HAS_DEBUG) && PC_HOSTED == 0
bool debug_bmp;
#endif
unsigned cortexm_wait_timeout = 2000; /* Timeout to wait for Cortex to react on halt command. */

int command_process(target *t, char *cmd)
{
    while (*cmd && *cmd <= ' ')
        cmd++;      /* skip leading whitespace (if any) */
    if (!*cmd)
        return -1;  /* nothing in the string, bail out */

    /* count tokens in the string */
    size_t argc = 1;/* start at 1, because we know there is something in the string */
    char *s = cmd;
    while (*s) {
        if (*s && *s <= ' ') {
            /* skipped over the argument, check if more follows */
            while (*s && *s <= ' ')
                s++;
            if (*s > ' ')
                argc++; /* more text follows whitespace -> another argument */
        } else {
            s++;
        }
    }

    const char **argv = alloca(sizeof(const char*) * argc);

    /* Tokenize cmd to fill argv */
    argc = 0;
    for (const char *part = strtok(cmd, " \t"); part; part = strtok(NULL, " \t"))
        argv[argc++] = part;

    /* Look for match and call handler */
    for(const struct command_s *c = cmd_list; c->cmd; c++) {
        /* Accept a partial match as GDB does.
         * So 'mon ver' will match 'monitor version'
         * Also note that the command "handler" functions return a bool, but
         * this function returns 0 for OK, 1 for Error and -1 for 'unrecognized'
         */
        if (strncmp(argv[0], c->cmd, strlen(argv[0])) == 0)
            return !c->handler(t, argc, argv);
    }

    if (!t)
        return -1;  /* not a general command, and no target attached -> unrecognized */
    return target_command(t, argc, argv);
}

#define BOARD_IDENT "Black Magic Probe" PLATFORM_IDENT FIRMWARE_VERSION

bool cmd_version(target *t, int argc, const char **argv)
{
    (void)t;
    (void)argc;
    (void)argv;
#if PC_HOSTED == 1
    char ident[256];
    gdb_ident(ident, sizeof(ident));
    DEBUG_WARN("%s\n", ident);
#else
    gdb_out(BOARD_IDENT);
    gdb_outf(", Hardware Version %d\n", platform_hwversion());
    gdb_out("Copyright (C) 2022 Black Magic Debug Project\n");
    gdb_out("License GPLv3+: GNU GPL version 3 or later "
        "<http://gnu.org/licenses/gpl.html>\n\n");
#endif

    return true;
}

bool cmd_help(target *t, int argc, const char **argv)
{
    (void)argc;
    (void)argv;

    if (!t || t->tc->destroy_callback) {
        gdb_out("General commands:\n");
        for (const struct command_s *c = cmd_list; c->cmd; c++)
            gdb_outf("\t%s -- %s\n", c->cmd, c->help);
        if (!t)
            return true;
    }

    target_command_help(t);
    return true;
}

static bool cmd_jtag_scan(target *t, int argc, const char **argv)
{
    (void)t;
    uint8_t irlens[argc];

    if (platform_target_voltage())
        gdb_outf("Target voltage: %s\n", platform_target_voltage());

    if (argc > 1) {
        /* Accept a list of IR lengths on command line */
        for (int i = 1; i < argc; i++)
            irlens[i-1] = atoi(argv[i]);
        irlens[argc-1] = 0;
    }

    if(connect_assert_nrst)
        platform_nrst_set_val(true); /* will be deasserted after attach */

    int devs = 0;
    volatile struct exception e;
    TRY_CATCH (e, EXCEPTION_ALL) {
#if PC_HOSTED == 1
        devs = platform_jtag_scan(argc > 1 ? irlens : NULL);
#else
        devs = jtag_scan(argc > 1 ? irlens : NULL);
#endif
    }
    switch (e.type) {
    case EXCEPTION_TIMEOUT:
        gdb_outf("Timeout during scan. Is target stuck in WFI?\n");
        break;
    case EXCEPTION_ERROR:
        gdb_outf("Exception: %s\n", e.msg);
        break;
    }

    if(devs == 0) {
        platform_target_clk_output_enable(false);
        platform_nrst_set_val(false);
        gdb_out("JTAG device scan failed!\n");
        return false;
    }

    cmd_targets(NULL, 0, NULL);
    platform_target_clk_output_enable(false);
    morse(NULL, false);
    return true;
}

bool cmd_swdp_scan(target *t, int argc, const char **argv)
{
    (void)t;
    volatile uint32_t targetid = 0;
    if (argc > 1)
        targetid  = strtol(argv[1], NULL, 0);

    if (platform_target_voltage())
        gdb_outf("Target voltage: %s\n", platform_target_voltage());
    if (connect_assert_nrst)
        platform_nrst_set_val(true); /* will be deasserted after attach */

    int devs = 0;
    volatile struct exception e;
    TRY_CATCH (e, EXCEPTION_ALL) {
#if PC_HOSTED == 1
        devs = platform_adiv5_swdp_scan(targetid);
#else
        devs = adiv5_swdp_scan(targetid);
#endif
    }
    switch (e.type) {
    case EXCEPTION_TIMEOUT:
        gdb_outf("Timeout during scan. Is target stuck in WFI?\n");
        break;
    case EXCEPTION_ERROR:
        gdb_outf("Exception: %s\n", e.msg);
        break;
    }

    if (devs == 0) {
        platform_target_clk_output_enable(false);
        platform_nrst_set_val(false);
        gdb_out("SW-DP scan failed!\n");
        return false;
    }

    cmd_targets(NULL, 0, NULL);
    platform_target_clk_output_enable(false);
    morse(NULL, false);
    return true;
}

bool cmd_auto_scan(target *t, int argc, const char **argv)
{
    (void)t;
    (void)argc;
    (void)argv;

    if (platform_target_voltage())
        gdb_outf("Target voltage: %s\n", platform_target_voltage());
    if (connect_assert_nrst)
        platform_nrst_set_val(true); /* will be deasserted after attach */

    uint32_t devs = 0;
    volatile struct exception e;
    TRY_CATCH (e, EXCEPTION_ALL) {
#if PC_HOSTED == 1
        devs = platform_jtag_scan(NULL);
#else
        devs = jtag_scan(NULL);
#endif
        if (devs > 0)
            break;
        gdb_out("JTAG scan found no devices, trying SWD!\n");

#if PC_HOSTED == 1
        devs = platform_adiv5_swdp_scan(0);
#else
        devs = adiv5_swdp_scan(0);
#endif
        if (devs > 0)
            break;

        gdb_out("SW-DP scan found no devices.\n");
    }
    switch (e.type) {
    case EXCEPTION_TIMEOUT:
        gdb_outf("Timeout during scan. Is target stuck in WFI?\n");
        break;
    case EXCEPTION_ERROR:
        gdb_outf("Exception: %s\n", e.msg);
        break;
    }

    if (devs == 0) {
        platform_target_clk_output_enable(false);
        platform_nrst_set_val(false);
        gdb_out("auto scan failed!\n");
        return false;
    }

    cmd_targets(NULL, 0, NULL);
    platform_target_clk_output_enable(false);
    morse(NULL, false);
    return true;
}

bool cmd_frequency(target *t, int argc, const char **argv)
{
    (void)t;
    if (argc == 2) {
        char *multiplier = NULL;
        uint32_t frequency = strtoul(argv[1], &multiplier, 10);
        if (!multiplier) {
            gdb_outf("Frequency must be an integral value possibly followed by 'k' or 'M'");
            return false;
        }
        switch (*multiplier) {
        case 'k':
            frequency *= 1000;
            break;
        case 'M':
            frequency *= 1000 * 1000;
            break;
        }
        platform_max_frequency_set(frequency);
    }
    uint32_t freq = platform_max_frequency_get();
    if (freq == FREQ_FIXED)
        gdb_outf("SWJ freq fixed\n");
    else
        gdb_outf("SWJ freq = %" PRIu32 " kHz\n", freq/1000);
    return true;
}

static void display_target(int i, target *t, void *context)
{
    (void)context;
    if (!strcmp(target_driver_name(t), "ARM Cortex-M")) {
        gdb_outf("***%2d%sUnknown %s Designer %3x Partno %3x %s\n",
                 i, target_attached(t)?" * ":" ",
                 target_driver_name(t),
                 target_designer(t),
                 target_idcode(t),
                 (target_core_name(t)) ? target_core_name(t): "");
    } else {
        gdb_outf("%2d   %c  %s %s\n", i, target_attached(t)?'*':' ',
                 target_driver_name(t),
                 (target_core_name(t)) ? target_core_name(t): "");
    }
}

bool cmd_targets(target *t, int argc, const char **argv)
{
    (void)t;
    (void)argc;
    (void)argv;
    gdb_out("Available Targets:\n");
    gdb_out("No. Att Driver\n");
    if (!target_foreach(display_target, NULL)) {
        gdb_out("No usable targets found.\n");
        return false;
    }

    return true;
}

bool cmd_morse(target *t, int argc, const char **argv)
{
    (void)t;
    (void)argc;
    (void)argv;
    if (morse_msg) {
        gdb_outf("%s\n", morse_msg);
        DEBUG_WARN("%s\n", morse_msg);
    } else {
        gdb_out("No message\n");
    }
    return true;
}

bool parse_enable_or_disable(const char *s, bool *out)
{
    size_t len = strlen(s);
    if (len && strncmp(s, "enable", len) == 0) {
        *out = true;
    } else if (len && strncmp(s, "disable", len) == 0) {
        *out = false;
    } else {
        gdb_out("'enable' or 'disable' argument must be provided\n");
        return false;
    }
    return true;
}

static bool cmd_connect_reset(target *t, int argc, const char **argv)
{
    (void)t;
    bool print_status = false;
    if (argc == 1) {
        print_status = true;
    } else if (argc == 2) {
        if (parse_enable_or_disable(argv[1], &connect_assert_nrst))
            print_status = true;
    } else {
        gdb_out(command_format_error);
    }

    if (print_status)
        gdb_outf("Assert nRST during connect: %s\n", connect_assert_nrst ? "enabled" : "disabled");
    return true;
}

static bool cmd_halt_timeout(target *t, int argc, const char **argv)
{
    (void)t;
    if (argc > 1)
        cortexm_wait_timeout = atol(argv[1]);
    gdb_outf("Cortex-M timeout to wait for device to halt: %d\n", cortexm_wait_timeout);
    return true;
}

static bool cmd_reset(target *t, int argc, const char **argv)
{
    (void)t;
    (void)argc;
    (void)argv;
    target_list_free();
    int pulse_len_ms = -1;
    if (argc == 2 && isdigit(argv[1][0]))
        pulse_len_ms = strtol(argv[1], NULL, 0);
    if (argc == 1 || pulse_len_ms > 0) {
        platform_nrst_set_val(true);
        /* when asserting ~reset, the platform_nrst_set_val() function adds a
           short delay, so that reset stays low for a minimum period if
           pulse_len_ms is zero */
        platform_delay(pulse_len_ms);
        platform_nrst_set_val(false);
    } else if (argc == 2) {
        bool assert_reset;
        if (parse_enable_or_disable(argv[1], &assert_reset))
            platform_nrst_set_val(assert_reset);
    } else {
        gdb_out(command_format_error);
    }
    return true;
}

static bool cmd_tdi_low_reset(target *t, int argc, const char **argv)
{
    (void)t;
    (void)argc;
    (void)argv;
    jtag_proc.jtagtap_next(true, false);
    cmd_reset(NULL, 0, NULL);
    return true;
}

#ifdef PLATFORM_HAS_POWER_SWITCH
static bool cmd_target_power(target *t, int argc, const char **argv)
{
    (void)t;
    if (argc == 1) {
        gdb_outf("Target Power: %s\n", platform_target_get_power() ? "enabled" : "disabled");
    } else if (argc == 2) {
        bool want_enable = false;
        bool is_enabled = platform_target_get_power();
        if (parse_enable_or_disable(argv[1], &want_enable)) {
            if (want_enable && !is_enabled
                && platform_target_voltage_sense() > POWER_CONFLICT_THRESHOLD) {
                /* Want to enable target power, but VREF > 0.5V sensed -> cancel */
                gdb_outf("Target already powered (%s)\n", platform_target_voltage());
            } else if (want_enable != is_enabled) {
                platform_target_set_power(want_enable);
                gdb_outf("%s target power\n", want_enable ? "Enabling" : "Disabling");
                /* Enabling 'tpwr' must clear the error message, iff the error
                   is 'tpwr'-related. */
                if (morse_msg && strncmp(morse_msg, "TPWR", 4) == 0)
                    morse(NULL, false);
            } else {
                /* No change -> different reply */
                gdb_outf("Target Power: %s\n", is_enabled ? "enabled" : "disabled");
            }
        }
    } else {
        gdb_out(command_format_error);
    }
    return true;
}
#endif

#ifdef ENABLE_RTT
static const char *on_or_off(const bool value)
{
    return value ? "on" : "off";
}

static bool cmd_rtt(target *t, int argc, const char **argv)
{
    (void)t;
    const size_t command_len = argc > 1 ? strlen(argv[1]) : 0;
    if (argc == 1 || (argc == 2 && strncmp(argv[1], "enabled", command_len) == 0)) {
        rtt_enabled = true;
        rtt_found = false;
        memset(rtt_channel, 0, sizeof(rtt_channel));
    } else if (argc == 2 && strncmp(argv[1], "disabled", command_len) == 0) {
        rtt_enabled = false;
        rtt_found = false;
    } else if (argc == 2 && strncmp(argv[1], "status", command_len) == 0) {
        gdb_outf("rtt: %s found: %s ident: ", on_or_off(rtt_enabled), rtt_found ? "yes" : "no");
        if (rtt_ident[0] == '\0')
            gdb_out("off");
        else
            gdb_outf("\"%s\"", rtt_ident);
        gdb_outf(" halt: %s", on_or_off(target_mem_access_needs_halt(t)));
        gdb_out(" channels: ");
        if (rtt_auto_channel)
            gdb_out("auto ");
        for (size_t i = 0; i < MAX_RTT_CHAN; i++) {
            if (rtt_channel_enabled[i])
                gdb_outf("%" PRIu32 " ", (uint32_t)i);
        }
        if (rtt_flag_ram)
            gdb_outf("ram: 0x%08" PRIx32 " 0x%08" PRIx32, rtt_ram_start, rtt_ram_end);
        gdb_outf(
            "\nmax poll ms: %u min poll ms: %u max errs: %u\n", rtt_max_poll_ms, rtt_min_poll_ms, rtt_max_poll_errs);
    } else if (argc >= 2 && strncmp(argv[1], "channel", command_len) == 0) {
        /* mon rtt channel switches to auto rtt channel selection
           mon rtt channel number... selects channels given */
        for (size_t i = 0; i < MAX_RTT_CHAN; i++)
            rtt_channel_enabled[i] = false;
        if (argc == 2)
            rtt_auto_channel = true;
        else {
            rtt_auto_channel = false;
            for (size_t i = 2; i < (size_t)argc; ++i) {
                const uint32_t channel = strtoul(argv[i], NULL, 0);
                if (channel < MAX_RTT_CHAN)
                    rtt_channel_enabled[channel] = true;
            }
        }
    } else if (argc == 2 && strncmp(argv[1], "ident", command_len) == 0) {
        rtt_ident[0] = '\0';
    } else if (argc == 2 && strncmp(argv[1], "poll", command_len) == 0) {
        gdb_outf("%u %u %u\n", rtt_max_poll_ms, rtt_min_poll_ms, rtt_max_poll_errs);
    } else if (argc == 2 && strncmp(argv[1], "cblock", command_len) == 0) {
        gdb_outf("cbaddr: 0x%x\n", rtt_cbaddr);
        gdb_out("ch ena i/o buffer@      size   head   tail flag\n");
        for (uint32_t i = 0; i < rtt_num_up_chan + rtt_num_down_chan; ++i) {
            gdb_outf("%2" PRIu32 "   %c %s 0x%08" PRIx32 " %6" PRIu32 " %6" PRIu32 " %6" PRIu32 " %4" PRIu32 "\n", i,
                rtt_channel_enabled[i] ? 'y' : 'n', i < rtt_num_up_chan ? "out" : "in ", rtt_channel[i].buf_addr,
                rtt_channel[i].buf_size, rtt_channel[i].head, rtt_channel[i].tail, rtt_channel[i].flag);
        }
    } else if (argc == 3 && strncmp(argv[1], "ident", command_len) == 0) {
        strncpy(rtt_ident, argv[2], sizeof(rtt_ident));
        rtt_ident[sizeof(rtt_ident) - 1U] = '\0';
        for (size_t i = 0; i < sizeof(rtt_ident); i++) {
            if (rtt_ident[i] == '_')
                rtt_ident[i] = ' ';
        }
    } else if (argc == 2 && strncmp(argv[1], "ram", command_len) == 0) {
        rtt_flag_ram = false;
    } else if (argc == 4 && strncmp(argv[1], "ram", command_len) == 0) {
        const int cnt1 = sscanf(argv[2], "%" SCNx32, &rtt_ram_start);
        const int cnt2 = sscanf(argv[3], "%" SCNx32, &rtt_ram_end);
        rtt_flag_ram = cnt1 == 1 && cnt2 == 1 && rtt_ram_end > rtt_ram_start;
        if (!rtt_flag_ram)
            gdb_out("address?\n");
    } else if (argc == 5 && strncmp(argv[1], "poll", command_len) == 0) {
        /* set polling params */
        rtt_max_poll_ms = strtoul(argv[2], NULL, 0);
        rtt_min_poll_ms = strtoul(argv[3], NULL, 0);
        rtt_max_poll_errs = strtoul(argv[4], NULL, 0);
    } else {
        gdb_out(command_format_error);
    }
    return true;
}
#endif

#ifdef PLATFORM_HAS_TRACESWO
static bool cmd_traceswo(target *t, int argc, const char **argv)
{
    char serial_no[DFU_SERIAL_LENGTH];
    (void)t;
#if TRACESWO_PROTOCOL == 2
    uint32_t baudrate = SWO_DEFAULT_BAUD;
#endif
    uint32_t swo_channelmask = 0; /* swo decoding off */
    uint8_t decode_arg = 1;
#if TRACESWO_PROTOCOL == 2
    /* argument: optional baud rate for async mode */
    if (argc > 1 && argv[1][0] >= '0' && argv[1][0] <= '9') {
        baudrate = atoi(argv[1]);
        if (baudrate == 0)
            baudrate = SWO_DEFAULT_BAUD;
        decode_arg = 2;
    }
#endif
    /* argument: 'decode' literal */
    if((argc > decode_arg) &&  !strncmp(argv[decode_arg], "decode", strlen(argv[decode_arg]))) {
        swo_channelmask = 0xFFFFFFFF; /* decoding all channels */
        /* arguments: channels to decode */
        if (argc > decode_arg + 1) {
            swo_channelmask = 0;
            for (int i = decode_arg+1; i < argc; i++) { /* create bitmask of channels to decode */
                int channel = atoi(argv[i]);
                if (channel >= 0 && channel < 32)
                    swo_channelmask |= 1U << channel;
            }
        }
    }

#if TRACESWO_PROTOCOL == 2
    gdb_outf("Baudrate: %lu ", baudrate);
#endif
    gdb_outf("Channel mask: ");
    for (size_t i = 0; i < 32U; ++i) {
        const uint32_t bit = (swo_channelmask >> (31U - i)) & 1U;
        gdb_outf("%" PRIu32, bit);
    }
    gdb_outf("\n");

#if TRACESWO_PROTOCOL == 2
    traceswo_init(baudrate, swo_channelmask);
#else
    traceswo_init(swo_channelmask);
#endif
    serial_no_read(serial_no);
    gdb_outf("%s:%02X:%02X\n", serial_no, 5, 0x85);
    return true;
}
#endif

#if defined(PLATFORM_HAS_DEBUG) && (PC_HOSTED == 0)
static bool cmd_debug_bmp(target *t, int argc, const char **argv)
{
    (void)t;
    bool print_status = false;
    if (argc == 1) {
        print_status = true;
    } else if (argc == 2) {
        if (parse_enable_or_disable(argv[1], &debug_bmp)) {
            print_status = true;
        }
    } else {
        gdb_out(command_format_error);
    }

    if (print_status) {
        gdb_outf("Debug mode is %s\n",
             debug_bmp ? "enabled" : "disabled");
    }
    return true;
}
#endif

#ifdef PLATFORM_HAS_UART_WHEN_SWDP
static bool cmd_convert_tdio(target *t, int argc, const char **argv)
{
    (void)t;

    bool print_status = false;
    if (argc == 1) {
        print_status = true;
    } else if (argc == 2) {
        bool want_enable;
        if (parse_enable_or_disable(argv[1], &want_enable)) {
            usbuart_convert_tdio(want_enable);
            print_status = true;
        }
    } else {
        gdb_out(command_format_error);
    }

    if (print_status)
        gdb_outf("TDI/TDO mapping to TxD/RxD: %s\n",
                 (usbuart_convert_tdio_enabled()) ? "enabled" : "disabled");

    return true;
}
#endif

#ifdef PLATFORM_HAS_PRINTSERIAL
bool cmd_serial(target *t, int argc, const char **argv)
{
    (void) t;
    (void) argc;
    (void) argv;

    print_serial();
    return true;
}
#endif

static bool cmd_heapinfo(target *t, int argc, const char **argv)
{
    if (t == NULL) {
        gdb_out("not attached\n");
    } else if (argc == 5) {
        target_addr heap_base = strtoul(argv[1], NULL, 16);
        target_addr heap_limit = strtoul(argv[2], NULL, 16);
        target_addr stack_base = strtoul(argv[3], NULL, 16);
        target_addr stack_limit = strtoul(argv[4], NULL, 16);
        gdb_outf("heapinfo heap_base: %p heap_limit: %p stack_base: %p stack_limit: %p\n",
            heap_base, heap_limit, stack_base, stack_limit);
        target_set_heapinfo(t, heap_base, heap_limit, stack_base, stack_limit);
    } else {
        gdb_out(command_format_error);
    }
    return true;
}
