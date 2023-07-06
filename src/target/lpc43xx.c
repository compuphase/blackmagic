/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2014 Allen Ibara <aibara>
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
#include "target.h"
#include "target_internal.h"
#include "cortexm.h"
#include "lpc_common.h"

#define LPC43XX_CHIPID  0x40043200

#define IAP_ENTRYPOINT_LOCATION 0x10400100

#define LPC43XX_ETBAHB_SRAM_BASE 0x2000C000
#define LPC43XX_ETBAHB_SRAM_SIZE (16*1024)

#define LPC43XX_WDT_MODE 0x40080000
#define LPC43XX_WDT_CNT  0x40080004
#define LPC43XX_WDT_FEED 0x40080008
#define LPC43XX_WDT_PERIOD_MAX 0xFFFFFF
#define LPC43XX_WDT_PROTECT (1 << 4)

#define IAP_RAM_SIZE    LPC43XX_ETBAHB_SRAM_SIZE
#define IAP_RAM_BASE    LPC43XX_ETBAHB_SRAM_BASE

#define IAP_PGM_CHUNKSIZE   4096

#define FLASH_NUM_BANK      2
#define FLASH_NUM_SECTOR    15

static bool lpc43xx_cmd_erase(target *t, int argc, const char *argv[]);
static bool lpc43xx_cmd_reset(target *t, int argc, const char *argv[]);
static bool lpc43xx_cmd_mkboot(target *t, int argc, const char *argv[]);
static int lpc43xx_flash_init(target *t);
static int lpc43xx_flash_erase(struct target_flash *f, target_addr addr, size_t len);
static void lpc43xx_set_internal_clock(target *t);
static void lpc43xx_wdt_set_period(target *t);
static void lpc43xx_wdt_pet(target *t);

const struct command_s lpc43xx_cmd_list[] = {
    {"erase_mass", lpc43xx_cmd_erase, "Erase entire flash memory"},
    {"mkboot", lpc43xx_cmd_mkboot, "Make flash bank bootable"},
    {"reset", lpc43xx_cmd_reset, "Reset target"},
    {NULL, NULL, NULL}
};

const struct command_s lpc43xx_noflash_cmd_list[] = {
    {"reset", lpc43xx_cmd_reset, "Reset target"},
    {NULL, NULL, NULL}
};

static void lpc43xx_add_flash(target *t, uint32_t iap_entry,
                       uint8_t bank, uint8_t base_sector,
                       uint32_t addr, size_t len, size_t erasesize)
{
    struct lpc_flash *lf = lpc_add_flash(t, addr, len);
    lf->f.erase = lpc43xx_flash_erase;
    lf->f.blocksize = erasesize;
    lf->f.buf_size = IAP_PGM_CHUNKSIZE;
    lf->bank = bank;
    lf->base_sector = base_sector;
    lf->iap_entry = iap_entry;
    lf->iap_ram = IAP_RAM_BASE;
    lf->iap_msp = IAP_RAM_BASE + IAP_RAM_SIZE;
    lf->wdt_kick = lpc43xx_wdt_pet;
}

bool lpc43xx_probe(target *t)
{
    uint32_t chipid;
    uint32_t iap_entry;

    chipid = target_mem_read32(t, LPC43XX_CHIPID);

    switch(chipid) {
    case 0x4284E02B:  /* LPC18[S]xx - M3 512K~1M Flash 72K SRAM - UM10430 Rev 3.1 2019 Ch 10.4.10 Table 97, note: single "CHIP ID" for a group of MCUs with different memory sizes */
    case 0x7284E02B:  /* LPC18[S]xx - M3 512K~1M Flash 72K SRAM - UM10430 Rev 3.1 2019 Ch 10.4.10 Table 97, note: single "CHIP ID" for a group of MCUs with different memory sizes */
        t->driver = "LPC18xx";
        target_add_ram(t, 0x10000000, 0x08000); /* first SRAM bank: 32 KiB */
        target_add_ram(t, 0x10080000, 0x0a000); /* second SRAM bank: 32+8 KiB */
        iap_entry = target_mem_read32(t, IAP_ENTRYPOINT_LOCATION);
        lpc43xx_add_flash(t, iap_entry, 0, 0, 0x1A000000, 0x10000, 0x2000);   /* bank A, 8 KiB sectors */
        lpc43xx_add_flash(t, iap_entry, 0, 8, 0x1A010000, 0x70000, 0x10000);  /* bank A, 64 KiB sectors */
        lpc43xx_add_flash(t, iap_entry, 1, 0, 0x1B000000, 0x10000, 0x2000);   /* bank B, 8 KiB sectors */
        lpc43xx_add_flash(t, iap_entry, 1, 8, 0x1B010000, 0x70000, 0x10000);  /* bank B, 64 KiB sectors */
        return true;

    case 0x5284E02B:  /* LPC18[S]x0 - M3 no Flash 104K~136K SRAM - UM10430 Rev 3.1 2019 Ch 10.4.10 Table 97, note: single "CHIP ID" for a group of MCUs with different memory sizes */
    case 0x6284E02B:  /* LPC18[S]x0 - M3 no Flash 104K~136K SRAM - UM10430 Rev 3.1 2019 Ch 10.4.10 Table 97, note: single "CHIP ID" for a group of MCUs with different memory sizes */
        t->driver = "LPC18xx";
        target_add_ram(t, 0x10000000, 0x10000); /* first SRAM bank: 64 KiB */
        target_add_ram(t, 0x10010000, 0x08000); /* second SRAM bank: 32 KiB (consecutive to first bank) */
        target_add_ram(t, 0x10080000, 0x0a000); /* third SRAM bank: 32+8 KiB */
        return true;

    case 0x4906002B:  /* LPC43[S]xx - M4/M0 512K~1M Flash 72K SRAM - UM10503 Rev 2.3 2017, Ch 11.4.11 Table 106, note: single "CHIP ID" for a group of MCUs with different memory sizes */
    case 0x7906002B:  /* LPC43[S]xx - M4/M0 512K~1M Flash 72K SRAM - UM10503 Rev 2.3 2017, Ch 11.4.11 Table 106, note: single "CHIP ID" for a group of MCUs with different memory sizes */
        t->driver = "LPC43xx";
        target_add_ram(t, 0x10000000, 0x08000); /* first SRAM bank: 32 KiB */
        target_add_ram(t, 0x10080000, 0x0a000); /* second SRAM bank: 32+8 KiB */
        iap_entry = target_mem_read32(t, IAP_ENTRYPOINT_LOCATION);
        lpc43xx_add_flash(t, iap_entry, 0, 0, 0x1A000000, 0x10000, 0x2000);   /* bank A, 8 KiB sectors */
        lpc43xx_add_flash(t, iap_entry, 0, 8, 0x1A010000, 0x70000, 0x10000);  /* bank A, 64 KiB sectors */
        lpc43xx_add_flash(t, iap_entry, 1, 0, 0x1B000000, 0x10000, 0x2000);   /* bank B, 8 KiB sectors */
        lpc43xx_add_flash(t, iap_entry, 1, 8, 0x1B010000, 0x70000, 0x10000);  /* bank B, 64 KiB sectors */
        target_add_commands(t, lpc43xx_cmd_list, t->driver);
        t->target_options |= CORTEXM_TOPT_INHIBIT_NRST;
        return true;

    case 0x5906002B:  /* LPC43[S]x0 - M4/M0 no Flash 136K~200K SRAM - UM10503 Rev 2.3 2017, Ch 11.4.11 Table 106, note: single "CHIP ID" for a group of MCUs with different memory sizes */
    case 0x6906002B:  /* LPC43[S]x0 - M4/M0 no Flash 136K~200K SRAM - UM10503 Rev 2.3 2017, Ch 11.4.11 Table 106, note: single "CHIP ID" for a group of MCUs with different memory sizes */
        t->driver = "LPC43xx";
        target_add_ram(t, 0x10000000, 0x18000); /* first SRAM bank: 96 KiB */
        target_add_ram(t, 0x10018000, 0x08000); /* second SRAM bank: 32 KiB (consecutive to first bank) */
        target_add_ram(t, 0x10080000, 0x12000); /* third SRAM bank: up to 72 KiB */
        target_add_commands(t, lpc43xx_noflash_cmd_list, t->driver);
        t->target_options |= CORTEXM_TOPT_INHIBIT_NRST;
        return true;
    }

    return false;
}

/* Reset all major systems _except_ debug */
static bool lpc43xx_cmd_reset(target *t, int argc, const char *argv[])
{
    (void)argc;
    (void)argv;

    /* Cortex-M4 Application Interrupt and Reset Control Register */
    static const uint32_t AIRCR = 0xE000ED0C;
    /* Magic value key */
    static const uint32_t reset_val = 0x05FA0004;

    /* System reset on target */
    target_mem_write(t, AIRCR, &reset_val, sizeof(reset_val));

    return true;
}

static bool lpc43xx_cmd_erase(target *t, int argc, const char *argv[])
{
    (void)argc;
    (void)argv;

    lpc43xx_flash_init(t);

    for (int bank = 0; bank < FLASH_NUM_BANK; bank++)
    {
        struct lpc_flash *f = (struct lpc_flash *)t->flash;
        if (lpc_iap_call(f, NULL, IAP_CMD_PREPARE,
                         0, FLASH_NUM_SECTOR-1, bank))
            return false;

        if (lpc_iap_call(f, NULL, IAP_CMD_ERASE,
                         0, FLASH_NUM_SECTOR-1, CPU_CLK_KHZ, bank))
            return false;
    }

    tc_printf(t, "Erase OK.\n");

    return true;
}

static int lpc43xx_flash_init(target *t)
{
    /* Deal with WDT */
    lpc43xx_wdt_set_period(t);

    /* Force internal clock */
    lpc43xx_set_internal_clock(t);

    /* Initialize flash IAP */
    struct lpc_flash *f = (struct lpc_flash *)t->flash;
    if (lpc_iap_call(f, NULL, IAP_CMD_INIT))
        return -1;

    return 0;
}

static int lpc43xx_flash_erase(struct target_flash *f, target_addr addr, size_t len)
{
    if (lpc43xx_flash_init(f->t))
        return -1;

    return lpc_flash_erase(f, addr, len);
}

static void lpc43xx_set_internal_clock(target *t)
{
    const uint32_t val2 = (1 << 11) | (1 << 24);
    target_mem_write32(t, 0x40050000 + 0x06C, val2);
}

/*
 * Call Boot ROM code to make a flash bank bootable by computing and writing the
 * correct signature into the exception table near the start of the bank.
 *
 * This is done indepently of writing to give the user a chance to verify flash
 * before changing it.
 */
static bool lpc43xx_cmd_mkboot(target *t, int argc, const char *argv[])
{
    /* Usage: mkboot 0 or mkboot 1 */
    if (argc != 2) {
        tc_printf(t, "Expected bank argument 0 or 1.\n");
        return false;
    }

    const long int bank = strtol(argv[1], NULL, 0);

    if ((bank != 0) && (bank != 1)) {
        tc_printf(t, "Unexpected bank number, should be 0 or 1.\n");
        return false;
    }

    lpc43xx_flash_init(t);

    /* special command to compute/write magic vector for signature */
    struct lpc_flash *f = (struct lpc_flash *)t->flash;
    if (lpc_iap_call(f, NULL, IAP_CMD_SET_ACTIVE_BANK, bank, CPU_CLK_KHZ)) {
        tc_printf(t, "Set bootable failed.\n");
        return false;
    }

    tc_printf(t, "Set bootable OK.\n");
    return true;
}

static void lpc43xx_wdt_set_period(target *t)
{
    /* Check if WDT is on */
    uint32_t wdt_mode = target_mem_read32(t, LPC43XX_WDT_MODE);

    /* If WDT on, we can't disable it, but we may be able to set a long period */
    if (wdt_mode && !(wdt_mode & LPC43XX_WDT_PROTECT))
        target_mem_write32(t, LPC43XX_WDT_CNT, LPC43XX_WDT_PERIOD_MAX);
}

static void lpc43xx_wdt_pet(target *t)
{
    /* Check if WDT is on */
    uint32_t wdt_mode = target_mem_read32(t, LPC43XX_WDT_MODE);

    /* If WDT on, pet */
    if (wdt_mode) {
        target_mem_write32(t, LPC43XX_WDT_FEED, 0xAA);
        target_mem_write32(t, LPC43XX_WDT_FEED, 0xFF);
    }
}

