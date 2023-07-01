/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2011 Mike Smith <drziplok@me.com>
 * Copyright (C) 2016 Gareth McMullin <gareth@blacksphere.co.nz>
 * Copyright (C) 2016 David Lawrence <dlaw@markforged.com>
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

#define IAP_PGM_CHUNKSIZE   512 /* should fit in RAM on any device */

#define MIN_RAM_SIZE            1024
#define RAM_USAGE_FOR_IAP_ROUTINES  32  /* IAP routines use 32 bytes at top of ram */

#define IAP_ENTRYPOINT  0x03000205
#define IAP_RAM_BASE    0x02000000

#define LPC15XX_DEVICE_ID  0x400743F8

static bool lpc15xx_read_uid(target *t, int argc, const char *argv[])
{
    (void)argc;
    (void)argv;
    struct lpc_flash *f = (struct lpc_flash *)t->flash;
    uint8_t uid[16];
    if (lpc_iap_call(f, uid, IAP_CMD_READUID))
        return false;
    tc_printf(t, "UID: 0x");
    for (uint32_t i = 0; i < sizeof(uid); ++i)
        tc_printf(t, "%02x", uid[i]);
    tc_printf(t, "\n");
    return true;
}

static bool lpc15xx_part_id(target *t, int argc, const char *argv[])
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

const struct command_s lpc15xx_cmd_list[] = {
    {"readuid", lpc15xx_read_uid, "Read out the 16-byte UID."},
    {"partid", lpc15xx_part_id, "Return the 32-bit part-id (aka device-id or chip-id)."},
    {NULL, NULL, NULL}
};

static void lpc15xx_add_flash(target *t, uint32_t addr, size_t len, size_t erasesize)
{
    struct lpc_flash *lf = lpc_add_flash(t, addr, len);
    lf->f.blocksize = erasesize;
    lf->f.buf_size = IAP_PGM_CHUNKSIZE;
    lf->f.write = lpc_flash_write_magic_vect;
    lf->iap_entry = IAP_ENTRYPOINT;
    lf->iap_ram = IAP_RAM_BASE;
    lf->iap_msp = IAP_RAM_BASE + MIN_RAM_SIZE - RAM_USAGE_FOR_IAP_ROUTINES;
}

bool lpc15xx_probe(target *t)
{
    /* read the device ID register */
    uint32_t device_id = target_mem_read32(t, LPC15XX_DEVICE_ID);
    switch (device_id) {
    case 0x00001517:  /* LPC1517 - M3 64K Flash 12K SRAM - UM10736 Rev 1.2 2017 Ch 34.7.11 Table 489 */
    case 0x00001518:  /* LPC1518 - M3 128K Flash 20K SRAM - UM10736 Rev 1.2 2017 Ch 34.7.11 Table 489 */
    case 0x00001519:  /* LPC1519 - M3 256K Flash 36K SRAM - UM10736 Rev 1.2 2017 Ch 34.7.11 Table 489 */
    case 0x00001547:  /* LPC1547 - M3 64K Flash 12K SRAM - UM10736 Rev 1.2 2017 Ch 34.7.11 Table 489 */
    case 0x00001548:  /* LPC1548 - M3 128K Flash 20K SRAM - UM10736 Rev 1.2 2017 Ch 34.7.11 Table 489 */
    case 0x00001549:  /* LPC1549 - M3 256K Flash 36K SRAM - UM10736 Rev 1.2 2017 Ch 34.7.11 Table 489 */
        t->driver = "LPC15xx";
        target_add_ram(t, 0x02000000, lpc_sram_size(device_id, 0x9000));
        lpc15xx_add_flash(t, 0x00000000, lpc_flash_size(device_id, 0x40000), 0x1000);
        target_add_commands(t, lpc15xx_cmd_list, "LPC15xx");
        return true;
    }

    if (device_id)
        DEBUG_INFO("LPC15xx: Unknown IDCODE 0x%08" PRIx32 "\n", device_id);

    return false;
}


