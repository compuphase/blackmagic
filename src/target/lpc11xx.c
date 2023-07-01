/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2011 Mike Smith <drziplok@me.com>
 * Copyright (C) 2016 Gareth McMullin <gareth@blacksphere.co.nz>
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

#define IAP_PGM_CHUNKSIZE 512 /* should fit in RAM on any device */

#define MIN_RAM_SIZE                1024
#define RAM_USAGE_FOR_IAP_ROUTINES  32  /* IAP routines use 32 bytes at top of ram */

#define IAP_ENTRY_MOST  0x1fff1ff1  /* all except LPC802, LPC804 & LPC84x */
#define IAP_ENTRY_84x   0x0f001ff1  /* LPC802, LPC804 & LPC84x */
#define IAP_RAM_BASE    0x10000000

#define LPC11XX_DEVICE_ID  0x400483F4
#define LPC8XX_DEVICE_ID   0x400483F8

#define LPC_RAM_BASE    0x10000000
#define LPC_FLASH_BASE  0x00000000

/*
 * CHIP    Ram Flash page sector   Rsvd pages  EEPROM
 * LPX80x   2k   16k   64   1024            2
 * LPC804   4k   32k   64   1024            2
 * LPC8N04  8k   32k   64   1024           32
 * LPC810   1k    4k   64   1024            0
 * LPC811   2k    8k   64   1024            0
 * LPC812   4k   16k   64   1024
 * LPC822   4k   16k   64   1024
 * LPC822   8k   32k   64   1024
 * LPC832   4k   16k   64   1024
 * LPC834   4k   32k   64   1024
 * LPC844   8k   64k   64   1024
 * LPC845  16k   64k   64   1024
 */

static bool lpc11xx_read_uid(target *t, int argc, const char *argv[])
{
    (void)argc;
    (void)argv;
    struct lpc_flash *f =(struct lpc_flash*)t->flash;
    uint8_t uid[16];
    if (lpc_iap_call(f, uid, IAP_CMD_READUID))
        return false;
    tc_printf(t, "UID: 0x");
    for (uint32_t i = 0; i < sizeof(uid); ++i)
        tc_printf(t, "%02x", uid[i]);
    tc_printf(t, "\n");
    return true;
}

static bool lpc11xx_part_id(target *t, int argc, const char *argv[])
{
    (void)argc;
    (void)argv;
    struct lpc_flash *f =(struct lpc_flash*)t->flash;
    uint32_t partid;
    if (lpc_iap_call(f, &partid, IAP_CMD_PARTID))
        return false;
    tc_printf(t, "Part ID: 0x%08x\n", partid);
    return true;
}

static const struct command_s lpc11xx_cmd_list[]= {
    {"readuid", lpc11xx_read_uid, "Read out the 16-byte UID."},
    {"partid", lpc11xx_part_id, "Return the 32-bit part-id (aka device-id or chip-id)."},
    {NULL, NULL, NULL}
};

static void lpc11xx_add_flash(target *t, uint32_t addr, size_t length, size_t erasesize, uint32_t iap_entry, uint8_t reserved_pages)
{
    struct lpc_flash *lf = lpc_add_flash(t, addr, length);
    lf->f.blocksize = erasesize;
    lf->f.buf_size = IAP_PGM_CHUNKSIZE;
    lf->f.write = lpc_flash_write_magic_vect;
    lf->iap_entry = iap_entry;
    lf->iap_ram = IAP_RAM_BASE;
    lf->iap_msp = IAP_RAM_BASE + MIN_RAM_SIZE - RAM_USAGE_FOR_IAP_ROUTINES;
    lf->reserved_pages = reserved_pages;
}

bool lpc11xx_probe(target *t)
{
    /* Read the DEVICE_ID register
     *
     * Nota Bene: the DEVICE_ID register at address 0x400483F4 is not valid for
     *   1) the LPC11xx & LPC11Cxx "XL" series, see UM10398 Rev.12.4 Chapter 3.1
     *   2) the LPC11U3x series, see UM10462 Rev.5.5 Chapter 3.1
     * But see the comment for the LPC8xx series below.
     */
    uint32_t device_id = target_mem_read32(t, LPC11XX_DEVICE_ID);
    switch (device_id) {
    case 0x2500102B:  /* LPC1102 - M0 32K Flash 8K SRAM - UM10429 Rev 6 2013 Ch 17.5.11 Table 173 */
    case 0x2548102B:  /* LPC1104 - M0 32K Flash 8K SRAM - UM10429 Rev 6 2013 Ch 17.5.11 Table 173 */
        t->driver = "LPC110x";
        target_add_ram(t, LPC_RAM_BASE, lpc_sram_size(device_id, 0x2000));
        lpc11xx_add_flash(t, LPC_FLASH_BASE, lpc_flash_size(device_id, 0x8000), 0x1000, IAP_ENTRY_MOST, 0);
        target_add_commands(t, lpc11xx_cmd_list, t->driver);
        return true;

    case 0x0A07102B:  /* LPC1110 - M0 4K Flash 1K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x1A07102B:  /* LPC1110 - M0 4K Flash 1K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x0A16D02B:  /* LPC1111/002 - M0 8K Flash 2K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x1A16D02B:  /* LPC1111/002 - M0 8K Flash 2K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x041E502B:  /* LPC1111/101 - M0 8K Flash 2K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x2516D02B:  /* LPC1111/102 - M0 8K Flash 2K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x0416502B:  /* LPC1111/201 - M0 8K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x2516902B:  /* LPC1111/202 - M0 8K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x042D502B:  /* LPC1112/101 - M0 16K Flash 2K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x2524D02B:  /* LPC1112/102 - M0 16K Flash 2K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x0A24902B:  /* LPC1112/102 - M0 16K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x1A24902B:  /* LPC1112/102 - M0 16K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x0A23902B:  /* LPC1112/102 - M0 16K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x1A23902B:  /* LPC1112/102 - M0 16K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x0425502B:  /* LPC1112/201 - M0 16K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x2524902B:  /* LPC1112/202 - M0 16K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x0434502B:  /* LPC1113/201 - M0 24K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x2532902B:  /* LPC1113/202 - M0 24K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x0434102B:  /* LPC1113/301 - M0 24K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x2532102B:  /* LPC1113/302 - M0 24K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x0A40902B:  /* LPC1114/102 - M0 32K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x1A40902B:  /* LPC1114/102 - M0 32K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x0444502B:  /* LPC1114/201 - M0 32K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x2540902B:  /* LPC1114/202 - M0 32K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x0444102B:  /* LPC1114/301 - M0 32K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x2540102B:  /* LPC1114/302 & LPC11D14/302 - M0 32K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
        t->driver = "LPC11xx";
        target_add_ram(t, LPC_RAM_BASE, lpc_sram_size(device_id, 0x2000));
        lpc11xx_add_flash(t, LPC_FLASH_BASE, lpc_flash_size(device_id, 0x8000), 0x1000, IAP_ENTRY_MOST, 0);
        target_add_commands(t, lpc11xx_cmd_list, t->driver);
        return true;

    case 0x1421102B:  /* LPC11C12/301 - M0 16K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x1440102B:  /* LPC11C14/301 - M0 32K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x1431102B:  /* LPC11C22/301 - M0 16K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x1430102B:  /* LPC11C24/301 - M0 32K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
        t->driver = "LPC11Cxx";
        target_add_ram(t, LPC_RAM_BASE, lpc_sram_size(device_id, 0x2000));
        lpc11xx_add_flash(t, LPC_FLASH_BASE, lpc_flash_size(device_id, 0x8000), 0x1000, IAP_ENTRY_MOST, 0);
        target_add_commands(t, lpc11xx_cmd_list, t->driver);
        return true;

    case 0x293E902B:  /* LPC11E11/101 - M0 8K Flash 4K SRAM - UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
    case 0x2954502B:  /* LPC11E12/201 - M0 16K Flash 6K SRAM - UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
    case 0x296A102B:  /* LPC11E13/301 - M0 24K Flash 8K SRAM - UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
    case 0x2980102B:  /* LPC11E14/401 - M0 32K Flash 10K SRAM - UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
    case 0x0000BC41:  /* LPC11E35/501 - M0 64K Flash 12K SRAM - UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
    case 0x00009C41:  /* LPC11E36/501 - M0 96K Flash 12K SRAM - UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
    case 0x00007C45:  /* LPC11E37/401 - M0 128K Flash 10K SRAM - UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
    case 0x00007C41:  /* LPC11E37/501 - M0 128K Flash 12K SRAM - UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
    case 0x0000DCC1:  /* LPC11E66 - M0+ 64K Flash 8K SRAM - UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
    case 0x0000BC81:  /* LPC11E67 - M0+ 128K Flash 16K SRAM - UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
    case 0x00007C01:  /* LPC11E68 - M0+ 256K Flash 32K SRAM - UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
        t->driver = "LPC11Exx";
        target_add_ram(t, LPC_RAM_BASE, lpc_sram_size(device_id, 0x8000));
        lpc11xx_add_flash(t, LPC_FLASH_BASE, lpc_flash_size(device_id, 0x40000), 0x1000, IAP_ENTRY_MOST, 0);
        target_add_commands(t, lpc11xx_cmd_list, t->driver);
        return true;

    case 0x095C802B:  /* LPC11U12/201 - M0 16K Flash 4K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    case 0x295C802B:  /* LPC11U12/201 - M0 16K Flash 4K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    case 0x097A802B:  /* LPC11U13/201 - M0 24K Flash 4K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    case 0x297A802B:  /* LPC11U13/201 - M0 24K Flash 4K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    case 0x0998802B:  /* LPC11U14/201 - M0 32K Flash 4K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    case 0x2998802B:  /* LPC11U14/201 - M0 32K Flash 4K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    case 0x2954402B:  /* LPC11U22/301 - M0 16K Flash 6K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    case 0x2972402B:  /* LPC11U23/301 - M0 24K Flash 6K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    case 0x2988402B:  /* LPC11U24/301 - M0 32K Flash 6K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    case 0x2980002B:  /* LPC11U24/401 - M0 32K Flash 8K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
        t->driver = "LPC11Uxx";
        target_add_ram(t, LPC_RAM_BASE, lpc_sram_size(device_id, 0x2000));
        lpc11xx_add_flash(t, LPC_FLASH_BASE, lpc_flash_size(device_id, 0x8000), 0x1000, IAP_ENTRY_MOST, 0);
        target_add_commands(t, lpc11xx_cmd_list, t->driver);
        return true;

    case 0x3640C02B:  /* LPC1224/101 - M0 32K Flash 4K SRAM - UM10441 Rev 2.2 2017 Ch 20.7.11 Table 303 */
    case 0x3642C02B:  /* LPC1224/121 - M0 48K Flash 4K SRAM - UM10441 Rev 2.2 2017 Ch 20.7.11 Table 303 */
    case 0x3650002B:  /* LPC1225/301 - M0 64K Flash 8K SRAM - UM10441 Rev 2.2 2017 Ch 20.7.11 Table 303 */
    case 0x3652002B:  /* LPC1225/321 - M0 80K Flash 8K SRAM - UM10441 Rev 2.2 2017 Ch 20.7.11 Table 303 */
    case 0x3660002B:  /* LPC1226/301 - M0 96K Flash 8K SRAM - UM10441 Rev 2.2 2017 Ch 20.7.11 Table 303 */
    case 0x3670002B:  /* LPC1227/301 & LPC12D27/301 - M0 128K Flash 8K SRAM - UM10441 Rev 2.2 2017 Ch 20.7.11 Table 303 */
        t->driver = "LPC122x";
        target_add_ram(t, LPC_RAM_BASE, lpc_sram_size(device_id, 0x2000));
        lpc11xx_add_flash(t, LPC_FLASH_BASE, lpc_flash_size(device_id, 0x20000), 0x1000, IAP_ENTRY_MOST, 0);
        target_add_commands(t, lpc11xx_cmd_list, t->driver);
        return true;

    case 0x2C42502B:  /* LPC1311 - M3 8K Flash 4K SRAM - UM10375 Rev 5 2012 Ch 21.13.11 Table 329 */
    case 0x1816902B:  /* LPC1311/01 - M3 8K Flash 4K SRAM - UM10375 Rev 5 2012 Ch 21.13.11 Table 329 */
    case 0x2C40102B:  /* LPC1313 - M3 32K Flash 8K SRAM - UM10375 Rev 5 2012 Ch 21.13.11 Table 329 */
    case 0x1830102B:  /* LPC1313/01 - M3 32K Flash 8K SRAM - UM10375 Rev 5 2012 Ch 21.13.11 Table 329 */
    case 0x3D01402B:  /* LPC1342 - M3 16K Flash 4K SRAM - UM10375 Rev 5 2012 Ch 21.13.11 Table 329 */
    case 0x3000002B:  /* LPC1343 - M3 32K Flash 8K SRAM - UM10375 Rev 5 2012 Ch 21.13.11 Table 329 */
    case 0x3D00002B:  /* LPC1343 - M3 32K Flash 8K SRAM - UM10375 Rev 5 2012 Ch 21.13.11 Table 329 */
        t->driver = "LPC13xx";
        target_add_ram(t, LPC_RAM_BASE, lpc_sram_size(device_id, 0x2000));
        lpc11xx_add_flash(t, LPC_FLASH_BASE, lpc_flash_size(device_id, 0x8000), 0x1000, IAP_ENTRY_MOST, 0);
        target_add_commands(t, lpc11xx_cmd_list, t->driver);
        return true;

    case 0x00008A04:  /* LPC8N04 - M0+ 32K Flash 8K SRAM - UM11074 Rev 1.3 2018 Ch 4.5.19 Table 25 */
        t->driver = "LPC8N04";
        target_add_ram(t, LPC_RAM_BASE, lpc_sram_size(device_id, 0x2000));
        /* UM11074/ Flash controller/15.2: The two topmost sectors
         * contain the initialization code and IAP firmware.
         * Do not touch them! */
        lpc11xx_add_flash(t, LPC_FLASH_BASE, 0x7800, 0x400, IAP_ENTRY_MOST, 0);
        target_add_commands(t, lpc11xx_cmd_list, t->driver);
        return true;
    }

    if (t->t_designer != AP_DESIGNER_SPECULAR && device_id)
        DEBUG_INFO("LPC1xxx: Unknown IDCODE 0x%08" PRIx32 "\n", device_id);

    /* Not documented, but the DEVICE_ID register at address 0x400483F8
     * for the LPC8xx series is also valid for the LPC11xx "XL" and the
     * LPC11U3x variants.
     */
    device_id = target_mem_read32(t, LPC8XX_DEVICE_ID);
    switch (device_id) {
    case 0x00008021:  /* LPC802M001JDH20 - M0+ 16K Flash 2K SRAM - UM11045 Rev 1.4 2018 Ch 4.5.12 Table 21 */
    case 0x00008022:  /* LPC802M011JDH20 - M0+ 16K Flash 2K SRAM - UM11045 Rev 1.4 2018 Ch 4.5.12 Table 21 */
    case 0x00008023:  /* LPC802M001JDH16 - M0+ 16K Flash 2K SRAM - UM11045 Rev 1.4 2018 Ch 4.5.12 Table 21 */
    case 0x00008024:  /* LPC802M001JHI33 - M0+ 16K Flash 2K SRAM - UM11045 Rev 1.4 2018 Ch 4.5.12 Table 21 */
        t->driver = "LPC802";
        target_add_ram(t, LPC_RAM_BASE, lpc_sram_size(device_id, 0x800));
        lpc11xx_add_flash(t, LPC_FLASH_BASE, lpc_flash_size(device_id, 0x4000), 0x400, IAP_ENTRY_84x, 2);
        target_add_commands(t, lpc11xx_cmd_list, t->driver);
        return true;

    case 0x00008040:  /* LPC804M101JBD64 - M0+ 32K Flash 4K SRAM - UM11065 Rev 1.0 2018 Ch 4.5.12 Table 21 */
    case 0x00008041:  /* LPC804M101JDH20 - M0+ 32K Flash 4K SRAM - UM11065 Rev 1.0 2018 Ch 4.5.12 Table 21 */
    case 0x00008042:  /* LPC804M101JDH24 - M0+ 32K Flash 4K SRAM - UM11065 Rev 1.0 2018 Ch 4.5.12 Table 21 */
    case 0x00008043:  /* LPC804M111JDH24 - M0+ 32K Flash 4K SRAM - UM11065 Rev 1.0 2018 Ch 4.5.12 Table 21 */
    case 0x00008044:  /* LPC804M101JHI33 - M0+ 32K Flash 4K SRAM - UM11065 Rev 1.0 2018 Ch 4.5.12 Table 21 */
        t->driver = "LPC804";
        target_add_ram(t, LPC_RAM_BASE, lpc_sram_size(device_id, 0x1000));
        lpc11xx_add_flash(t, LPC_FLASH_BASE, lpc_flash_size(device_id, 0x8000), 0x400, IAP_ENTRY_84x, 2);
        target_add_commands(t, lpc11xx_cmd_list, t->driver);
        return true;

    case 0x00008100:  /* LPC810M021FN8 - M0+ 4K Flash 1K SRAM - UM10601 Rev 1.6 2014 Ch 4.6.33 Table 50 */
    case 0x00008110:  /* LPC811M001JDH16 - M0+ 8K Flash 2K SRAM - UM10601 Rev 1.6 2014 Ch 4.6.33 Table 50 */
    case 0x00008120:  /* LPC812M101JDH16 - M0+ 16K Flash 4K SRAM - UM10601 Rev 1.6 2014 Ch 4.6.33 Table 50 */
    case 0x00008121:  /* LPC812M101JD20 - M0+ 16K Flash 4K SRAM - UM10601 Rev 1.6 2014 Ch 4.6.33 Table 50 */
    case 0x00008122:  /* LPC812M101JDH20 / LPC812M101JTB16 - M0+ 16K Flash 4K SRAM - UM10601 Rev 1.6 2014 Ch 4.6.33 Table 50 */
        t->driver = "LPC81x";
        target_add_ram(t, LPC_RAM_BASE, lpc_sram_size(device_id, 0x1000));
        lpc11xx_add_flash(t, LPC_FLASH_BASE, lpc_flash_size(device_id, 0x4000), 0x400, IAP_ENTRY_MOST, 0);
        target_add_commands(t, lpc11xx_cmd_list, t->driver);
        return true;

    case 0x00008221:  /* LPC822M101JHI33 - M0+ 16K Flash 4K SRAM - UM10800 Rev 1.2 2016 Ch 25.6.1.11 Table 324 */
    case 0x00008222:  /* LPC822M101JDH20 - M0+ 16K Flash 4K SRAM - UM10800 Rev 1.2 2016 Ch 25.6.1.11 Table 324 */
    case 0x00008241:  /* LPC824M201JHI33 - M0+ 32K Flash 8K SRAM - UM10800 Rev 1.2 2016 Ch 25.6.1.11 Table 324 */
    case 0x00008242:  /* LPC824M201JDH20 - M0+ 32K Flash 8K SRAM - UM10800 Rev 1.2 2016 Ch 25.6.1.11 Table 324 */
        t->driver = "LPC82x";
        target_add_ram(t, LPC_RAM_BASE, lpc_sram_size(device_id, 0x2000));
        lpc11xx_add_flash(t, LPC_FLASH_BASE, lpc_flash_size(device_id, 0x8000), 0x400, IAP_ENTRY_MOST, 0);
        target_add_commands(t, lpc11xx_cmd_list, t->driver);
        return true;

    case 0x00008322:  /* LPC832M101FDH20 - M0+ 16K Flash 4K SRAM - UM11021 Rev 1.1 2016 Ch 24.6.1.11 Table 317 */
    case 0x00008341:  /* LPC834M101FHI33 - M0+ 32K Flash 4K SRAM - UM11021 Rev 1.1 2016 Ch 24.6.1.11 Table 317 */
        t->driver = "LPC83x";
        target_add_ram(t, LPC_RAM_BASE, lpc_sram_size(device_id, 0x1000));
        lpc11xx_add_flash(t, LPC_FLASH_BASE, lpc_flash_size(device_id, 0x8000), 0x400, IAP_ENTRY_MOST, 0);
        target_add_commands(t, lpc11xx_cmd_list, t->driver);
        return true;

    case 0x00008441:  /* LPC844M201JBD64 - M0+ 64K Flash 8K SRAM - UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173 */
    case 0x00008442:  /* LPC844M201JBD48 - M0+ 64K Flash 8K SRAM - UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173 */
    case 0x00008443:  /* LPC844M201JHI48 - M0+ 64K Flash 8K SRAM - UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173; note: table 29 is wrong) */
    case 0x00008444:  /* LPC844M201JHI33 - M0+ 64K Flash 8K SRAM - UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173 */
    case 0x00008451:  /* LPC845M301JBD64 - M0+ 64K Flash 16K SRAM - UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173 */
    case 0x00008452:  /* LPC845M301JBD48 - M0+ 64K Flash 16K SRAM - UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173 */
    case 0x00008453:  /* LPC845M301JHI48 - M0+ 64K Flash 16K SRAM - UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173 */
    case 0x00008454:  /* LPC845M301JHI33 - M0+ 64K Flash 16K SRAM - UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173 */
        t->driver = "LPC84x";
        target_add_ram(t, LPC_RAM_BASE, lpc_sram_size(device_id, 0x4000));
        lpc11xx_add_flash(t, LPC_FLASH_BASE, lpc_flash_size(device_id, 0x10000), 0x400, IAP_ENTRY_84x, 0);
        target_add_commands(t, lpc11xx_cmd_list, t->driver);
        return true;

    case 0x0003D440:  /* LPC11U34/311 - M0 40K Flash 8K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    case 0x0001cc40:  /* LPC11U34/421 - M0 48K Flash 8K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    case 0x0001BC40:  /* LPC11U35/401 - M0 64K Flash 8K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    case 0x0000BC40:  /* LPC11U35/501 - M0 64K Flash 8K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    case 0x00019C40:  /* LPC11U36/401 - M0 96K Flash 8K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    case 0x00017C40:  /* LPC11U37/401 - M0 128K Flash 8K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    case 0x00007C44:  /* LPC11U37/401 - M0 128K Flash 8K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    case 0x00007C40:  /* LPC11U37/501 - M0 128K Flash 8K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
        t->driver = "LPC11U3x";
        target_add_ram(t, LPC_RAM_BASE, lpc_sram_size(device_id, 0x2000));
        lpc11xx_add_flash(t, LPC_FLASH_BASE, lpc_flash_size(device_id, 0x20000), 0x1000, IAP_ENTRY_MOST, 0);
        target_add_commands(t, lpc11xx_cmd_list, t->driver);
        return true;

    case 0x0000DCC8:  /* LPC11U66 - M0+ 64K Flash 8K SRAM - UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
    case 0x0000BC88:  /* LPC11U67 - M0+ 128K Flash 16K SRAM - UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
    case 0x0000BC80:  /* LPC11U67 - M0+ 128K Flash 16K SRAM - UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
    case 0x00007C08:  /* LPC11U68 - M0+ 256K Flash 32K SRAM - UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
    case 0x00007C00:  /* LPC11U68 - M0+ 256K Flash 32K SRAM - UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
        t->driver = "LPC11U6x";
        target_add_ram(t, LPC_RAM_BASE, lpc_sram_size(device_id, 0x8000));
        lpc11xx_add_flash(t, LPC_FLASH_BASE, lpc_flash_size(device_id, 0x40000), 0x1000, IAP_ENTRY_MOST, 0);
        target_add_commands(t, lpc11xx_cmd_list, t->driver);
        return true;

    case 0x00010013:  /* LPC1111/103 - M0 8K Flash 2K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x00010012:  /* LPC1111/203 - M0 8K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x00020023:  /* LPC1112/103 - M0 16K Flash 2K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x00020022:  /* LPC1112/203 - M0 16K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x00030032:  /* LPC1113/203 - M0 24K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x00030030:  /* LPC1113/303 - M0 24K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x00040042:  /* LPC1114/203 - M0 32K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x00040040:  /* LPC1114/303 - M0 32K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x00040060:  /* LPC1114/323 - M0 48K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x00040070:  /* LPC1114/333 - M0 56K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    case 0x00050080:  /* LPC1115/303 - M0 64K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
        t->driver = "LPC11xx-XL";
        target_add_ram(t, LPC_RAM_BASE, lpc_sram_size(device_id, 0x2000));
        lpc11xx_add_flash(t, LPC_FLASH_BASE, lpc_flash_size(device_id, 0x10000), 0x1000, IAP_ENTRY_MOST, 0);
        target_add_commands(t, lpc11xx_cmd_list, t->driver);
        return true;

    case 0x00140040:  /* LPC1124/303 - 32K Flash 8K SRAM */
    case 0x00150080:  /* LPC1125/303 - 64K Flash 8K SRAM */
        t->driver = "LPC112x";
        target_add_ram(t, LPC_RAM_BASE, lpc_sram_size(device_id, 0x2000));
        lpc11xx_add_flash(t, LPC_FLASH_BASE, lpc_flash_size(device_id, 0x10000), 0x1000, IAP_ENTRY_MOST, 0);
        target_add_commands(t, lpc11xx_cmd_list, t->driver);
        return true;

    case 0x3A010523:  /* LPC1315 - M3 32K Flash 8K SRAM - UM10524 Rev 4 2013 Ch 21.13.11 Table 358 */
    case 0x1A018524:  /* LPC1316 - M3 48K Flash 8K SRAM - UM10524 Rev 4 2013 Ch 21.13.11 Table 358 */
    case 0x1A020525:  /* LPC1317 - M3 64K Flash 8K SRAM - UM10524 Rev 4 2013 Ch 21.13.11 Table 358 */
    case 0x28010541:  /* LPC1345 - M3 32K Flash 8K SRAM - UM10524 Rev 4 2013 Ch 21.13.11 Table 358 */
    case 0x08018542:  /* LPC1346 - M3 48K Flash 8K SRAM - UM10524 Rev 4 2013 Ch 21.13.11 Table 358 */
    case 0x08020543:  /* LPC1347 - M3 64K Flash 8K SRAM - UM10524 Rev 4 2013 Ch 21.13.11 Table 358 */
        t->driver = "LPC13xx";
        target_add_ram(t, LPC_RAM_BASE, lpc_sram_size(device_id, 0x2000));
        lpc11xx_add_flash(t, LPC_FLASH_BASE, lpc_flash_size(device_id, 0x10000), 0x1000, IAP_ENTRY_MOST, 0);
        target_add_commands(t, lpc11xx_cmd_list, t->driver);
        return true;
    }

    if (device_id)
        DEBUG_INFO("LPC8xx, LPC1xxx(XL): Unknown IDCODE 0x%08" PRIx32 "\n", device_id);

    return false;
}

