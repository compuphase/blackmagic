/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2012 Gareth McMullin <gareth@blacksphere.co.nz>
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
#include "adiv5.h"

#define IAP_PGM_CHUNKSIZE           4096

#define MIN_RAM_SIZE                8192 // LPC1751
#define RAM_USAGE_FOR_IAP_ROUTINES  32 // IAP routines use 32 bytes at top of ram

#define IAP_ENTRYPOINT              0x1FFF1FF1
#define IAP_RAM_BASE                0x10000000

#define MEMMAP                      0x400FC040
#define LPC17xx_JTAG_IDCODE         0x4BA00477
#define LPC17xx_SWDP_IDCODE         0x2BA01477
#define FLASH_NUM_SECTOR            30

#define LPC17xx_MEMMAP              UINT32_C(0x400fc040)
#define LPC17xx_MPU_BASE            UINT32_C(0xe000ed90)
#define LPC17xx_MPU_CTRL            (LPC17xx_MPU_BASE + 0x04U)

struct flash_param {
    uint16_t opcode;
    uint16_t pad0;
    uint32_t command;
    uint32_t words[4];
    uint32_t result[5]; // return code and maximum of 4 result parameters
} __attribute__((aligned(4)));

struct lpc17xx_priv {
    uint32_t mpu_ctrl_state;
    uint32_t memmap_state;
};

static void lpc17xx_extended_reset(target *t);
static bool lpc17xx_enter_flash_mode(target *t);
static bool lpc17xx_exit_flash_mode(target *t);
static bool lpc17xx_read_uid(target *t, int argc, const char *argv[]);
static bool lpc17xx_part_id(target *t, int argc, const char *argv[]);
static bool lpc17xx_cmd_erase(target *t, int argc, const char *argv[]);
enum iap_status lpc17xx_iap_call(target *t, struct flash_param *param, enum iap_cmd cmd, ...);

static const struct command_s lpc17xx_cmd_list[] = {
    {"readuid", lpc17xx_read_uid, "Read out the 16-byte UID."},
    {"partid", lpc17xx_part_id, "Return the 32-bit part-id (aka device-id or chip-id)."},
    {"erase_mass", lpc17xx_cmd_erase, "Erase entire flash memory"},
    {NULL, NULL, NULL}
};

static void lpc17xx_add_flash(target *t, uint32_t addr, size_t len, size_t erasesize, unsigned int base_sector)
{
    struct lpc_flash *lf = lpc_add_flash(t, addr, len);
    lf->f.blocksize = erasesize;
    lf->base_sector = base_sector;
    lf->f.buf_size = IAP_PGM_CHUNKSIZE;
    lf->f.write = lpc_flash_write_magic_vect;
    lf->iap_entry = IAP_ENTRYPOINT;
    lf->iap_ram = IAP_RAM_BASE;
    lf->iap_msp = IAP_RAM_BASE + MIN_RAM_SIZE - RAM_USAGE_FOR_IAP_ROUTINES;
}

bool lpc17xx_probe(target *t)
{
    /* Read the IDCODE register from the SW-DP */
    ADIv5_AP_t *ap = cortexm_ap(t);
    uint32_t ap_idcode = ap->dp->idcode;

    if (ap_idcode == LPC17xx_JTAG_IDCODE || ap_idcode == LPC17xx_SWDP_IDCODE) {
        /* LPC176x/5x family. See UM10360.pdf 33.7 JTAG TAP Identification*/
    } else {
        return false;
    }

    if ((t->cpuid & CPUID_PARTNO_MASK) == CORTEX_M3)  {
        /*
         * Now that we're sure it's a Cortex-M3, we need to halt the
         * target and make an IAP call to get the part number.
         * There appears to have no other method of reading the part number.
         */
        target_halt_request(t);

        /* Allocate private storage so the flash mode entry/exit routines can save state */
        struct lpc17xx_priv *priv = calloc(1, sizeof(*priv));
        if (!priv) { /* calloc failed: heap exhaustion */
            DEBUG_WARN("calloc: failed in %s\n", __func__);
            return false;
        }
        t->target_storage = priv;

        /* Prepare Flash mode */
        lpc17xx_enter_flash_mode(t);
        /* Read the Part ID */
        struct flash_param param;
        lpc17xx_iap_call(t, &param, IAP_CMD_PARTID);
        target_halt_resume(t, false);

        if (param.result[0])
            return false;

        size_t flash_size = lpc_flash_size(param.result[1], 0);
        size_t sram_size = lpc_sram_size(param.result[1], 0x8000);
        switch (param.result[1]) {
        case 0x25001118:  /* LPC1751 - M3 32K Flash 8K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
        case 0x25001110:  /* LPC1751 (No CRP) - M3 32K Flash 8K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
        case 0x25001121:  /* LPC1752 - M3 64K Flash 16K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
        case 0x25011722:  /* LPC1754 - M3 128K Flash 32K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
        case 0x25011723:  /* LPC1756 - M3 256K Flash 32K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
        case 0x25013F37:  /* LPC1758 - M3 512K Flash 64K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
        case 0x25113737:  /* LPC1759 - M3 512K Flash 64K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
        case 0x26012033:  /* LPC1763 - M3 256K Flash 64K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
        case 0x26011922:  /* LPC1764 - M3 128K Flash 32K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
        case 0x26013733:  /* LPC1765 - M3 256K Flash 64K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
        case 0x26013F33:  /* LPC1766 - M3 256K Flash 64K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
        case 0x26012837:  /* LPC1767 - M3 512K Flash 64K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
        case 0x26013F37:  /* LPC1768 - M3 512K Flash 64K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
        case 0x26113F37:  /* LPC1769 - M3 512K Flash 64K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
        case 0x27011132:  /* LPC1774 - M3 128K Flash 32K SRAM - UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
        case 0x27191F43:  /* LPC1776 - M3 256K Flash 64K SRAM - UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
        case 0x27193747:  /* LPC1777 - M3 512K Flash 64K SRAM - UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
        case 0x27193F47:  /* LPC1778 - M3 512K Flash 64K SRAM - UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
        case 0x281D1743:  /* LPC1785 - M3 256K Flash 64K SRAM - UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
        case 0x281D1F43:  /* LPC1786 - M3 256K Flash 64K SRAM - UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
        case 0x281D3747:  /* LPC1787 - M3 512K Flash 64K SRAM - UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
        case 0x281D3F47:  /* LPC1788 - M3 512K Flash 64K SRAM - UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
            t->driver = "LPC17xx";
            t->extended_reset = lpc17xx_extended_reset;
            t->enter_flash_mode = lpc17xx_enter_flash_mode;
            t->exit_flash_mode = lpc17xx_exit_flash_mode;
            if (sram_size <= 0x8000) {
                target_add_ram(t, 0x10000000, sram_size);
            } else {
                /* first main SRAM block is 32 KiB */
                target_add_ram(t, 0x10000000, 0x8000);
                /* one or two more 16 KiB blocks are present (these are consecutive,
                   and so they might be considered a single 32 KiB block) */
                target_add_ram(t, 0x2007C000, 0x4000);
                if (sram_size > 0xc000)
                    target_add_ram(t, 0x20080000, 0x4000);
            }
            if (flash_size <= 0x10000) {
                lpc17xx_add_flash(t, 0x00000000, flash_size, 0x1000, 0);
            } else {
                /* first 64 KiB is in 4 KiB sectors */
                lpc17xx_add_flash(t, 0x00000000, 0x10000, 0x1000, 0);
            }
            if (flash_size > 0x10000) {
                /* remaining Flash is in 32 KiB sectors */
                lpc17xx_add_flash(t, 0x00010000, flash_size - 0x10000, 0x8000, 16);
            }
            target_add_commands(t, lpc17xx_cmd_list, "LPC17xx");
            return true;
        }
    }
    return false;
}

static bool lpc17xx_enter_flash_mode(target *t)
{
    struct lpc17xx_priv *priv = (struct lpc17xx_priv*)t->target_storage;
    /* Disable the MPU, if enabled */
    priv->mpu_ctrl_state = target_mem_read32(t, LPC17xx_MPU_CTRL);
    target_mem_write32(t, LPC17xx_MPU_CTRL, 0);
    /* And store the memory mapping state */
    priv->memmap_state = target_mem_read32(t, LPC17xx_MEMMAP);
    return true;
}

static bool lpc17xx_exit_flash_mode(target *t)
{
    struct lpc17xx_priv *priv = (struct lpc17xx_priv*)t->target_storage;
    /* Restore the memory mapping and MPU state (in that order!) */
    target_mem_write32(t, LPC17xx_MEMMAP, priv->memmap_state);
    target_mem_write32(t, LPC17xx_MPU_CTRL, priv->mpu_ctrl_state);
    return true;
}

static bool lpc17xx_read_uid(target *t, int argc, const char *argv[])
{
    (void)argc;
    (void)argv;

    lpc17xx_enter_flash_mode(t);
    struct flash_param param;
    lpc17xx_iap_call(t, &param, IAP_CMD_READUID);
    lpc17xx_exit_flash_mode(t);

    tc_printf(t, "UID: 0x");
    const uint8_t *uid = (const uint8_t*)&param.result[1];
    for (uint32_t i = 0; i < 16; ++i)
        tc_printf(t, "%02x", uid[i]);
    tc_printf(t, "\n");
    return true;
}

static bool lpc17xx_part_id(target *t, int argc, const char *argv[])
{
    (void)argc;
    (void)argv;

    lpc17xx_enter_flash_mode(t);
    struct flash_param param;
    lpc17xx_iap_call(t, &param, IAP_CMD_PARTID);
    lpc17xx_exit_flash_mode(t);

    tc_printf(t, "Part ID: 0x%08x\n", param.result[1]);
    return true;
}

static bool lpc17xx_cmd_erase(target *t, int argc, const char *argv[])
{
    (void)argc;
    (void)argv;
    struct flash_param param;

    if (lpc17xx_iap_call(t, &param, IAP_CMD_PREPARE, 0, FLASH_NUM_SECTOR-1)) {
        DEBUG_WARN("lpc17xx_cmd_erase: prepare failed %d\n",
                   (unsigned int)param.result[0]);
        return false;
    }

    if (lpc17xx_iap_call(t, &param, IAP_CMD_ERASE, 0, FLASH_NUM_SECTOR-1, CPU_CLK_KHZ)) {
        DEBUG_WARN("lpc17xx_cmd_erase: erase failed %d\n",
                   (unsigned int)param.result[0]);
        return false;
    }

    if (lpc17xx_iap_call(t, &param, IAP_CMD_BLANKCHECK, 0, FLASH_NUM_SECTOR-1)) {
        DEBUG_WARN("lpc17xx_cmd_erase: blankcheck failed %d\n",
                   (unsigned int)param.result[0]);
        return false;
    }
    tc_printf(t, "Erase OK.\n");
    return true;
}

/**
 * Target has been reset, make sure to remap the boot ROM
 * from 0x00000000 leaving the user flash visible
 */
static void lpc17xx_extended_reset(target *t)
{
    /* From Ch 33.6 Debug memory re-mapping (Page 643) UM10360.pdf (Rev 2) */
    target_mem_write32(t, MEMMAP, 1);
}

enum iap_status lpc17xx_iap_call(target *t, struct flash_param *param, enum iap_cmd cmd, ...) {
    param->opcode = ARM_THUMB_BREAKPOINT;
    param->command = cmd;

    /* fill out the remainder of the parameters */
    va_list ap;
    va_start(ap, cmd);
    for (int i = 0; i < 4; i++)
        param->words[i] = va_arg(ap, uint32_t);
    va_end(ap);

    /* copy the structure to RAM */
    target_mem_write(t, IAP_RAM_BASE, param, sizeof(struct flash_param));

    /* set up for the call to the IAP ROM */
    uint32_t regs[t->regs_size / sizeof(uint32_t)];
    target_regs_read(t, regs);
    regs[0] = IAP_RAM_BASE + offsetof(struct flash_param, command);
    regs[1] = IAP_RAM_BASE + offsetof(struct flash_param, result);
    regs[REG_MSP] = IAP_RAM_BASE + MIN_RAM_SIZE - RAM_USAGE_FOR_IAP_ROUTINES;
    regs[REG_LR] = IAP_RAM_BASE | 1;
    regs[REG_PC] = IAP_ENTRYPOINT;
    target_regs_write(t, regs);

    /* start the target and wait for it to halt again */
    target_halt_resume(t, false);
    while (!target_halt_poll(t, NULL));

    /* copy back just the parameters structure */
    target_mem_read(t, (void *)param, IAP_RAM_BASE, sizeof(struct flash_param));
    return param->result[0];
}

