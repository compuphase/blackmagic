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
#include "target.h"
#include "target_internal.h"
#include "cortexm.h"
#include "lpc_common.h"

#include <stdarg.h>

struct flash_param {
    uint16_t opcode;
    uint16_t pad0;
    uint32_t command;
    uint32_t words[4];
    uint32_t status;
    uint32_t result[4];
} __attribute__((aligned(4)));

char *iap_error[] = {
    "CMD_SUCCESS",
    "Invalid command",
    "Unaligned src address",
    "Dst address not on boundary",
    "Src not mapped",
    "Dst not mapped",
    "Invalid byte count",
    "Invalid sector",
    "Sector not blank",
    "Sector not prepared",
    "Compare error",
    "Flash interface busy",
    "Invalid or missing parameter",
    "Address not on boundary",
    "Address not mapped",
    "Checksum error",
    "16",
    "17",
    "18",
    "19",
    "20",
    "21",
    "22",
    "FRO not powered",
    "Flash not powered",
    "25",
    "26",
    "Flash clock disabled",
    "Reinvoke error",
    "Invalid image",
    "30",
    "31",
    "Flash erase failed",
    "Page is invalid",
};

static int lpc_flash_write(struct target_flash *tf,
                           target_addr dest, const void *src, size_t len);

struct lpc_flash *lpc_add_flash(target *t, target_addr addr, size_t length)
{
    struct lpc_flash *lf = calloc(1, sizeof(*lf));
    struct target_flash *f;

    if (!lf) {          /* calloc failed: heap exhaustion */
        DEBUG_WARN("calloc: failed in %s\n", __func__);
        return NULL;
    }

    f = &lf->f;
    f->start = addr;
    f->length = length;
    f->erase = lpc_flash_erase;
    f->write = lpc_flash_write;
    f->erased = 0xff;
    target_add_flash(t, f);
    return lf;
}

enum iap_status lpc_iap_call(struct lpc_flash *f, void *result, enum iap_cmd cmd, ...)
{
    target *t = f->f.t;
    struct flash_param param = {
        .opcode = ARM_THUMB_BREAKPOINT,
        .command = cmd,
    };

    /* Pet WDT before each IAP call, if it is on */
    if (f->wdt_kick)
        f->wdt_kick(t);

    /* save IAP RAM to restore after IAP call */
    struct flash_param backup_param;
    target_mem_read(t, &backup_param, f->iap_ram, sizeof(backup_param));

    /* save registers to restore after IAP call */
    uint32_t backup_regs[t->regs_size / sizeof(uint32_t)];
    target_regs_read(t, backup_regs);

    /* fill out the remainder of the parameters (which may be just dummy values,
       depending on whether the command actually takes parameters) */
    va_list ap;
    va_start(ap, cmd);
    for (int i = 0; i < 4; i++)
        param.words[i] = va_arg(ap, uint32_t);
    va_end(ap);

    /* copy the structure to RAM */
    target_mem_write(t, f->iap_ram, &param, sizeof(param));

    /* set up for the call to the IAP ROM */
    uint32_t regs[t->regs_size / sizeof(uint32_t)];
    target_regs_read(t, regs);
    regs[0] = f->iap_ram + offsetof(struct flash_param, command);
    regs[1] = f->iap_ram + offsetof(struct flash_param, status);
    regs[REG_MSP] = f->iap_msp;
    regs[REG_LR] = f->iap_ram | 1;
    regs[REG_PC] = f->iap_entry;
    target_regs_write(t, regs);

    /* start the target and wait for it to halt again */
    target_halt_resume(t, false);
    while (!target_halt_poll(t, NULL));

    /* copy back just the parameters structure */
    target_mem_read(t, &param, f->iap_ram, sizeof(param));

    /* restore the original data in RAM and registers */
    target_mem_write(t, f->iap_ram, &backup_param, sizeof(param));
    target_regs_write(t, backup_regs);

    /* if the user expected a result, set the result (16 bytes). */
    if (result != NULL)
        memcpy(result, param.result, sizeof(param.result));

#if defined(ENABLE_DEBUG)
    if (param.status != IAP_STATUS_CMD_SUCCESS) {
        if (param.status > (sizeof(iap_error) / sizeof(char*)))
            DEBUG_WARN("IAP  cmd %d : %" PRId32 "\n", cmd, param.status);
        else
            DEBUG_WARN("IAP  cmd %d : %s\n", cmd, iap_error[param.status]);
        DEBUG_WARN("return parameters: %08" PRIx32 " %08" PRIx32 " %08" PRIx32
                   " %08" PRIx32 "\n", param.result[0],
                   param.result[1], param.result[2], param.result[3]);
    }
#endif
    return param.status;
}

static uint8_t lpc_sector_for_addr(struct lpc_flash *f, uint32_t addr)
{
    return f->base_sector + (addr - f->f.start) / f->f.blocksize;
}

#define LPX80X_SECTOR_SIZE 0x400
#define LPX80X_PAGE_SIZE    0x40

int lpc_flash_erase(struct target_flash *tf, target_addr addr, size_t len)
{
    struct lpc_flash *f = (struct lpc_flash *)tf;
    uint32_t start = lpc_sector_for_addr(f, addr);
    uint32_t end = lpc_sector_for_addr(f, addr + len - 1);
    uint32_t last_full_sector = end;

    if (lpc_iap_call(f, NULL, IAP_CMD_PREPARE, start, end, f->bank))
        return -1;

    /* Only LPC80x has reserved pages!*/
    if (f->reserved_pages && ((addr + len) >=  tf->length - 0x400) ) {
        last_full_sector -= 1;
    }
    if (start <= last_full_sector) {
        /* Sector erase */
        if (lpc_iap_call(f, NULL, IAP_CMD_ERASE, start, last_full_sector, CPU_CLK_KHZ, f->bank))
            return -2;

        /* check erase ok */
        if (lpc_iap_call(f, NULL, IAP_CMD_BLANKCHECK, start, last_full_sector, f->bank))
            return -3;
    }
    if (last_full_sector != end) {
        uint32_t page_start = (addr + len - LPX80X_SECTOR_SIZE) / LPX80X_PAGE_SIZE;
        uint32_t page_end = page_start +  LPX80X_SECTOR_SIZE/LPX80X_PAGE_SIZE - 1 - f->reserved_pages;
        if (lpc_iap_call(f, NULL, IAP_CMD_PREPARE, end, end, f->bank))
            return -1;

        if (lpc_iap_call(f, NULL, IAP_CMD_ERASE_PAGE, page_start, page_end, CPU_CLK_KHZ, f->bank))
            return -2;
        /* Blank check omitted!*/
    }
    return 0;
}

static int lpc_flash_write(struct target_flash *tf,
                    target_addr dest, const void *src, size_t len)
{
    struct lpc_flash *f = (struct lpc_flash *)tf;
    /* prepare... */
    uint32_t sector = lpc_sector_for_addr(f, dest);
    if (lpc_iap_call(f, NULL, IAP_CMD_PREPARE, sector, sector, f->bank)) {
        DEBUG_WARN("Prepare failed\n");
        return -1;
    }
    uint32_t bufaddr = ALIGN(f->iap_ram + sizeof(struct flash_param), 4);
    target_mem_write(f->f.t, bufaddr, src, len);
    /* Only LPC80x has reserved pages!*/
    if ((!f->reserved_pages) || ((dest + len) <= (tf->length - len))) {
        /* Write payload to target ram */
        /* set the destination address and program */
        if (lpc_iap_call(f, NULL, IAP_CMD_PROGRAM, dest, bufaddr, len, CPU_CLK_KHZ))
            return -2;
    } else {
        /* On LPC80x, write top sector in pages.
         * Silently ignore write to the 2 reserved pages at top!*/
        len -= 0x40 * f->reserved_pages;
        while (len) {
            if (lpc_iap_call(f, NULL, IAP_CMD_PREPARE, sector, sector, f->bank)) {
                DEBUG_WARN("Prepare failed\n");
                return -1;
            }
            /* set the destination address and program */
            if (lpc_iap_call(f, NULL, IAP_CMD_PROGRAM, dest, bufaddr, LPX80X_PAGE_SIZE, CPU_CLK_KHZ))
                return -2;
            dest += LPX80X_PAGE_SIZE;
            bufaddr += LPX80X_PAGE_SIZE;
            len -= LPX80X_PAGE_SIZE;
        }
    }
    return 0;
}

int lpc_flash_write_magic_vect(struct target_flash *f,
                               target_addr dest, const void *src, size_t len)
{
    if (dest == 0) {
        /* Fill in the magic vector to allow booting the flash */
        uint32_t *w = (uint32_t *)src;
        uint32_t sum = 0;

        /* compute checksum of first 7 vectors */
        for (unsigned i = 0; i < 7; i++)
            sum += w[i];
        /* two's complement is written to 8'th vector */
        w[7] = ~sum + 1;
    }
    return lpc_flash_write(f, dest, src, len);
}


struct lpc_meminfo_s {
    uint32_t device_id;
    size_t flash, sram;
};
static const struct lpc_meminfo_s lpc_meminfo[] = {
    { 0x00008A04,   0x8000,  0x2000 },  /* LPC8N04 - M0+ 32K Flash 8K SRAM - UM11074 Rev 1.3 2018 Ch 4.5.19 Table 25 */
    { 0x00008021,   0x4000,   0x800 },  /* LPC802M001 - M0+ 16K Flash 2K SRAM - UM11045 Rev 1.4 2018 Ch 4.5.12 Table 21 */
    { 0x00008023,   0x4000,   0x800 },  /* LPC802M001 - M0+ 16K Flash 2K SRAM - UM11045 Rev 1.4 2018 Ch 4.5.12 Table 21 */
    { 0x00008024,   0x4000,   0x800 },  /* LPC802M001 - M0+ 16K Flash 2K SRAM - UM11045 Rev 1.4 2018 Ch 4.5.12 Table 21 */
    { 0x00008022,   0x4000,   0x800 },  /* LPC802M011 - M0+ 16K Flash 2K SRAM - UM11045 Rev 1.4 2018 Ch 4.5.12 Table 21 */
    { 0x00008040,   0x8000,  0x1000 },  /* LPC804M101 - M0+ 32K Flash 4K SRAM - UM11065 Rev 1.0 2018 Ch 4.5.12 Table 21 */
    { 0x00008041,   0x8000,  0x1000 },  /* LPC804M101 - M0+ 32K Flash 4K SRAM - UM11065 Rev 1.0 2018 Ch 4.5.12 Table 21 */
    { 0x00008042,   0x8000,  0x1000 },  /* LPC804M101 - M0+ 32K Flash 4K SRAM - UM11065 Rev 1.0 2018 Ch 4.5.12 Table 21 */
    { 0x00008043,   0x8000,  0x1000 },  /* LPC804M111 - M0+ 32K Flash 4K SRAM - UM11065 Rev 1.0 2018 Ch 4.5.12 Table 21 */
    { 0x00008044,   0x8000,  0x1000 },  /* LPC804M101 - M0+ 32K Flash 4K SRAM - UM11065 Rev 1.0 2018 Ch 4.5.12 Table 21 */
    { 0x00008100,   0x1000,   0x400 },  /* LPC810M021 - M0+ 4K Flash 1K SRAM - UM10601 Rev 1.6 2014 Ch 4.6.33 Table 50 */
    { 0x00008110,   0x2000,   0x800 },  /* LPC811M001 - M0+ 8K Flash 2K SRAM - UM10601 Rev 1.6 2014 Ch 4.6.33 Table 50 */
    { 0x00008120,   0x4000,  0x1000 },  /* LPC812M101 - M0+ 16K Flash 4K SRAM - UM10601 Rev 1.6 2014 Ch 4.6.33 Table 50 */
    { 0x00008121,   0x4000,  0x1000 },  /* LPC812M101 - M0+ 16K Flash 4K SRAM - UM10601 Rev 1.6 2014 Ch 4.6.33 Table 50 */
    { 0x00008122,   0x4000,  0x1000 },  /* LPC812M101 - M0+ 16K Flash 4K SRAM - UM10601 Rev 1.6 2014 Ch 4.6.33 Table 50 */
    { 0x00008221,   0x4000,  0x1000 },  /* LPC822M101 - M0+ 16K Flash 4K SRAM - UM10800 Rev 1.2 2016 Ch 25.6.1.11 Table 324 */
    { 0x00008222,   0x4000,  0x1000 },  /* LPC822M101 - M0+ 16K Flash 4K SRAM - UM10800 Rev 1.2 2016 Ch 25.6.1.11 Table 324 */
    { 0x00008241,   0x8000,  0x2000 },  /* LPC824M201 - M0+ 32K Flash 8K SRAM - UM10800 Rev 1.2 2016 Ch 25.6.1.11 Table 324 */
    { 0x00008242,   0x8000,  0x2000 },  /* LPC824M201 - M0+ 32K Flash 8K SRAM - UM10800 Rev 1.2 2016 Ch 25.6.1.11 Table 324 */
    { 0x00008322,   0x4000,  0x1000 },  /* LPC832M101 - M0+ 16K Flash 4K SRAM - UM11021 Rev 1.1 2016 Ch 24.6.1.11 Table 317 */
    { 0x00008341,   0x8000,  0x1000 },  /* LPC834M101 - M0+ 32K Flash 4K SRAM - UM11021 Rev 1.1 2016 Ch 24.6.1.11 Table 317 */
    { 0x00008441,  0x10000,  0x2000 },  /* LPC844M201 - M0+ 64K Flash 8K SRAM - UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173 */
    { 0x00008442,  0x10000,  0x2000 },  /* LPC844M201 - M0+ 64K Flash 8K SRAM - UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173 */
    { 0x00008443,  0x10000,  0x2000 },  /* LPC844M201 - M0+ 64K Flash 8K SRAM - UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173; note: table 29 is wrong) */
    { 0x00008444,  0x10000,  0x2000 },  /* LPC844M201 - M0+ 64K Flash 8K SRAM - UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173 */
    { 0x00008451,  0x10000,  0x4000 },  /* LPC845M301 - M0+ 64K Flash 16K SRAM - UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173 */
    { 0x00008452,  0x10000,  0x4000 },  /* LPC845M301 - M0+ 64K Flash 16K SRAM - UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173 */
    { 0x00008453,  0x10000,  0x4000 },  /* LPC845M301 - M0+ 64K Flash 16K SRAM - UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173 */
    { 0x00008454,  0x10000,  0x4000 },  /* LPC845M301 - M0+ 64K Flash 16K SRAM - UM11029 Rev 1.7 2021 Ch 8.6.49 Table 173 */
    { 0x00008651,  0x10000,  0x2000 },  /* LPC865M201 - M0+ 64K Flash 8K SRAM - UM11607 Rev 3 2023 Ch 4.5.12 Table 20 */
    { 0x00008652,  0x10000,  0x2000 },  /* LPC865M201 - M0+ 64K Flash 8K SRAM - UM11607 Rev 3 2023 Ch 4.5.12 Table 20 */
    { 0x00008654,  0x10000,  0x2000 },  /* LPC865M201 - M0+ 64K Flash 8K SRAM - UM11607 Rev 3 2023 Ch 4.5.12 Table 20 */
    { 0x2500102B,   0x8000,  0x2000 },  /* LPC1102 - M0 32K Flash 8K SRAM - UM10429 Rev 6 2013 Ch 17.5.11 Table 173 */
    { 0x2548102B,   0x8000,  0x2000 },  /* LPC1104 - M0 32K Flash 8K SRAM - UM10429 Rev 6 2013 Ch 17.5.11 Table 173 */
    { 0x0A07102B,   0x1000,   0x400 },  /* LPC1110 - M0 4K Flash 1K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x1A07102B,   0x1000,   0x400 },  /* LPC1110 - M0 4K Flash 1K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x0A16D02B,   0x2000,   0x800 },  /* LPC1111/002 - M0 8K Flash 2K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x1A16D02B,   0x2000,   0x800 },  /* LPC1111/002 - M0 8K Flash 2K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x041E502B,   0x2000,   0x800 },  /* LPC1111/101 - M0 8K Flash 2K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x2516D02B,   0x2000,   0x800 },  /* LPC1111/102 - M0 8K Flash 2K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x00010013,   0x2000,   0x800 },  /* LPC1111/103 - M0 8K Flash 2K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x0416502B,   0x4000,  0x1000 },  /* LPC1111/201 - M0 8K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x2516902B,   0x4000,  0x1000 },  /* LPC1111/202 - M0 8K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x00010012,   0x4000,  0x1000 },  /* LPC1111/203 - M0 8K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x042D502B,   0x4000,   0x800 },  /* LPC1112/101 - M0 16K Flash 2K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x2524D02B,   0x4000,   0x800 },  /* LPC1112/102 - M0 16K Flash 2K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x0A24902B,   0x4000,  0x1000 },  /* LPC1112/102 - M0 16K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x1A24902B,   0x4000,  0x1000 },  /* LPC1112/102 - M0 16K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x0A23902B,   0x4000,  0x1000 },  /* LPC1112/102 - M0 16K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x1A23902B,   0x4000,  0x1000 },  /* LPC1112/102 - M0 16K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x00020023,   0x4000,   0x800 },  /* LPC1112/103 - M0 16K Flash 2K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x0425502B,   0x4000,  0x1000 },  /* LPC1112/201 - M0 16K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x2524902B,   0x4000,  0x1000 },  /* LPC1112/202 - M0 16K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x00020022,   0x4000,  0x1000 },  /* LPC1112/203 - M0 16K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x0434502B,   0x6000,  0x1000 },  /* LPC1113/201 - M0 24K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x2532902B,   0x6000,  0x1000 },  /* LPC1113/202 - M0 24K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x00030032,   0x6000,  0x1000 },  /* LPC1113/203 - M0 24K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x0434102B,   0x6000,  0x2000 },  /* LPC1113/301 - M0 24K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x2532102B,   0x6000,  0x2000 },  /* LPC1113/302 - M0 24K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x00030030,   0x6000,  0x2000 },  /* LPC1113/303 - M0 24K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x0A40902B,   0x8000,  0x1000 },  /* LPC1114/102 - M0 32K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x1A40902B,   0x8000,  0x1000 },  /* LPC1114/102 - M0 32K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x0444502B,   0x8000,  0x1000 },  /* LPC1114/201 - M0 32K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x2540902B,   0x8000,  0x1000 },  /* LPC1114/202 - M0 32K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x00040042,   0x8000,  0x1000 },  /* LPC1114/203 - M0 32K Flash 4K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x0444102B,   0x8000,  0x2000 },  /* LPC1114/301 - M0 32K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x2540102B,   0x8000,  0x2000 },  /* LPC1114/302 & LPC11D14/302 - M0 32K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x00040040,   0x8000,  0x2000 },  /* LPC1114/303 - M0 32K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x00040060,   0xc000,  0x2000 },  /* LPC1114/323 - M0 48K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x00040070,   0xe000,  0x2000 },  /* LPC1114/333 - M0 56K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x00050080,  0x10000,  0x2000 },  /* LPC1115/303 - M0 64K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x4D4C802B,   0x4000,  0x1000 },  /* LPC11A02UK - M0 16K Flash 4K SRAM - UM10527 Rev 3 2012, Ch 20.7.11 Table 228 */
    { 0x4D80002B,   0x8000,  0x2000 },  /* LPC11A04UK - M0 32K Flash 8K SRAM - UM10527 Rev 3 2012, Ch 20.7.11 Table 228 */
    { 0x455EC02B,   0x2000,   0x800 },  /* LPC11A11/001 - M0 8K Flash 2K SRAM - UM10527 Rev 3 2012, Ch 20.7.11 Table 228 */
    { 0x4574802B,   0x4000,  0x1000 },  /* LPC11A12/101 - M0 16K Flash 4K SRAM - UM10527 Rev 3 2012, Ch 20.7.11 Table 228 */
    { 0x458A402B,   0x6000,  0x1800 },  /* LPC11A13/201 - M0 24K Flash 6K SRAM - UM10527 Rev 3 2012, Ch 20.7.11 Table 228 */
    { 0x35A0002B,   0x8000,  0x2000 },  /* LPC11A14/301 - M0 32K Flash 8K SRAM - UM10527 Rev 3 2012, Ch 20.7.11 Table 228 */
    { 0x45A0002B,   0x8000,  0x2000 },  /* LPC11A14/301 - M0 32K Flash 8K SRAM - UM10527 Rev 3 2012, Ch 20.7.11 Table 228 */
    { 0x1421102B,   0x4000,  0x2000 },  /* LPC11C12/301 - M0 16K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x1440102B,   0x8000,  0x2000 },  /* LPC11C14/301 - M0 32K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x1431102B,   0x4000,  0x2000 },  /* LPC11C22/301 - M0 16K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x1430102B,   0x8000,  0x2000 },  /* LPC11C24/301 - M0 32K Flash 8K SRAM - UM10398 Rev 12.4 2016 Ch 26.5.11 Table 387 */
    { 0x293E902B,   0x2000,  0x1000 },  /* LPC11E11/101 - M0 8K Flash 4K SRAM - UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
    { 0x2954502B,   0x4000,  0x1800 },  /* LPC11E12/201 - M0 16K Flash 6K SRAM - UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
    { 0x296A102B,   0x6000,  0x2000 },  /* LPC11E13/301 - M0 24K Flash 8K SRAM - UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
    { 0x2980102B,   0x8000,  0x2800 },  /* LPC11E14/401 - M0 32K Flash 10K SRAM - UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
    { 0x0000BC41,  0x10000,  0x3000 },  /* LPC11E35/501 - M0 64K Flash 12K SRAM - UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
    { 0x00009C41,  0x18000,  0x3000 },  /* LPC11E36/501 - M0 96K Flash 12K SRAM - UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
    { 0x00007C45,  0x20000,  0x2800 },  /* LPC11E37/401 - M0 128K Flash 10K SRAM - UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
    { 0x00007C41,  0x20000,  0x3000 },  /* LPC11E37/501 - M0 128K Flash 12K SRAM - UM10518 Rev 3.5 2016 Ch 19.12.11 Table 311 */
    { 0x0000DCC1,  0x10000,  0x2000 },  /* LPC11E66 - M0+ 64K Flash 8K SRAM - UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
    { 0x0000BC81,  0x20000,  0x4000 },  /* LPC11E67 - M0+ 128K Flash 16K SRAM - UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
    { 0x00007C01,  0x40000,  0x8000 },  /* LPC11E68 - M0+ 256K Flash 32K SRAM - UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
    { 0x095C802B,   0x4000,  0x1000 },  /* LPC11U12/201 - M0 16K Flash 4K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    { 0x295C802B,   0x4000,  0x1000 },  /* LPC11U12/201 - M0 16K Flash 4K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    { 0x097A802B,   0x6000,  0x1000 },  /* LPC11U13/201 - M0 24K Flash 4K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    { 0x297A802B,   0x6000,  0x1000 },  /* LPC11U13/201 - M0 24K Flash 4K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    { 0x0998802B,   0x8000,  0x1000 },  /* LPC11U14/201 - M0 32K Flash 4K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    { 0x2998802B,   0x8000,  0x1000 },  /* LPC11U14/201 - M0 32K Flash 4K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    { 0x2954402B,   0x4000,  0x1800 },  /* LPC11U22/301 - M0 16K Flash 6K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    { 0x2972402B,   0x6000,  0x1800 },  /* LPC11U23/301 - M0 24K Flash 6K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    { 0x2988402B,   0x8000,  0x1800 },  /* LPC11U24/301 - M0 32K Flash 6K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    { 0x2980002B,   0x8000,  0x2000 },  /* LPC11U24/401 - M0 32K Flash 8K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    { 0x0003D440,   0xa000,  0x2000 },  /* LPC11U34/311 - M0 40K Flash 8K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    { 0x0001cc40,   0xc000,  0x2000 },  /* LPC11U34/421 - M0 48K Flash 8K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    { 0x0001BC40,  0x10000,  0x2000 },  /* LPC11U35/401 - M0 64K Flash 8K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    { 0x0000BC40,  0x10000,  0x2000 },  /* LPC11U35/501 - M0 64K Flash 8K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    { 0x00019C40,  0x18000,  0x2000 },  /* LPC11U36/401 - M0 96K Flash 8K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    { 0x00017C40,  0x20000,  0x2000 },  /* LPC11U37/401 - M0 128K Flash 8K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    { 0x00007C44,  0x20000,  0x2000 },  /* LPC11U37/401 - M0 128K Flash 8K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    { 0x00007C40,  0x20000,  0x2000 },  /* LPC11U37/501 - M0 128K Flash 8K SRAM - UM10462 Rev 5.5 2016 Ch 20.13.11 Table 377 */
    { 0x0000DCC8,  0x10000,  0x2000 },  /* LPC11U66 - M0+ 64K Flash 8K SRAM - UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
    { 0x0000BC88,  0x20000,  0x4000 },  /* LPC11U67 - M0+ 128K Flash 16K SRAM - UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
    { 0x0000BC80,  0x20000,  0x4000 },  /* LPC11U67 - M0+ 128K Flash 16K SRAM - UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
    { 0x00007C08,  0x40000,  0x8000 },  /* LPC11U68 - M0+ 256K Flash 32K SRAM - UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
    { 0x00007C00,  0x40000,  0x8000 },  /* LPC11U68 - M0+ 256K Flash 32K SRAM - UM10732 Rev 1.8 2016 Ch 27.5.11 Table 377 */
    { 0x3640C02B,   0x8000,  0x1000 },  /* LPC1224/101 - M0 32K Flash 4K SRAM - UM10441 Rev 2.2 2017 Ch 20.7.11 Table 303 */
    { 0x3642C02B,   0xc000,  0x1000 },  /* LPC1224/121 - M0 48K Flash 4K SRAM - UM10441 Rev 2.2 2017 Ch 20.7.11 Table 303 */
    { 0x3650002B,  0x10000,  0x2000 },  /* LPC1225/301 - M0 64K Flash 8K SRAM - UM10441 Rev 2.2 2017 Ch 20.7.11 Table 303 */
    { 0x3652002B,  0x14000,  0x2000 },  /* LPC1225/321 - M0 80K Flash 8K SRAM - UM10441 Rev 2.2 2017 Ch 20.7.11 Table 303 */
    { 0x3660002B,  0x18000,  0x2000 },  /* LPC1226/301 - M0 96K Flash 8K SRAM - UM10441 Rev 2.2 2017 Ch 20.7.11 Table 303 */
    { 0x3670002B,  0x20000,  0x2000 },  /* LPC1227/301 & LPC12D27/301 - M0 128K Flash 8K SRAM - UM10441 Rev 2.2 2017 Ch 20.7.11 Table 303 */
    { 0x2C42502B,   0x2000,  0x1000 },  /* LPC1311 - M3 8K Flash 4K SRAM - UM10375 Rev 5 2012 Ch 21.13.11 Table 329 */
    { 0x1816902B,   0x2000,  0x1000 },  /* LPC1311/01 - M3 8K Flash 4K SRAM - UM10375 Rev 5 2012 Ch 21.13.11 Table 329 */
    { 0x2C40102B,   0x8000,  0x2000 },  /* LPC1313 - M3 32K Flash 8K SRAM - UM10375 Rev 5 2012 Ch 21.13.11 Table 329 */
    { 0x1830102B,   0x8000,  0x2000 },  /* LPC1313/01 - M3 32K Flash 8K SRAM - UM10375 Rev 5 2012 Ch 21.13.11 Table 329 */
    { 0x3A010523,   0x8000,  0x2000 },  /* LPC1315 - M3 32K Flash 8K SRAM - UM10524 Rev 4 2013 Ch 21.13.11 Table 358 */
    { 0x1A018524,   0xc000,  0x2000 },  /* LPC1316 - M3 48K Flash 8K SRAM - UM10524 Rev 4 2013 Ch 21.13.11 Table 358 */
    { 0x1A020525,  0x10000,  0x2000 },  /* LPC1317 - M3 64K Flash 8K SRAM - UM10524 Rev 4 2013 Ch 21.13.11 Table 358 */
    { 0x3D01402B,   0x4000,  0x1000 },  /* LPC1342 - M3 16K Flash 4K SRAM - UM10375 Rev 5 2012 Ch 21.13.11 Table 329 */
    { 0x3D00002B,   0x8000,  0x2000 },  /* LPC1343 - M3 32K Flash 8K SRAM - UM10375 Rev 5 2012 Ch 21.13.11 Table 329 */
    { 0x3000002B,   0x8000,  0x2000 },  /* LPC1343 - M3 32K Flash 8K SRAM - UM10375 Rev 5 2012 Ch 21.13.11 Table 329 */
    { 0x28010541,   0x8000,  0x2000 },  /* LPC1345 - M3 32K Flash 8K SRAM - UM10524 Rev 4 2013 Ch 21.13.11 Table 358 */
    { 0x08018542,   0xc000,  0x2000 },  /* LPC1346 - M3 48K Flash 8K SRAM - UM10524 Rev 4 2013 Ch 21.13.11 Table 358 */
    { 0x08020543,  0x10000,  0x2000 },  /* LPC1347 - M3 64K Flash 8K SRAM - UM10524 Rev 4 2013 Ch 21.13.11 Table 358 */
    { 0x00001517,  0x10000,  0x3000 },  /* LPC1517 - M3 64K Flash 12K SRAM - UM10736 Rev 1.2 2017 Ch 34.7.11 Table 489 */
    { 0x00001518,  0x20000,  0x5000 },  /* LPC1518 - M3 128K Flash 20K SRAM - UM10736 Rev 1.2 2017 Ch 34.7.11 Table 489 */
    { 0x00001519,  0x40000,  0x9000 },  /* LPC1519 - M3 256K Flash 36K SRAM - UM10736 Rev 1.2 2017 Ch 34.7.11 Table 489 */
    { 0x00001547,  0x10000,  0x3000 },  /* LPC1547 - M3 64K Flash 12K SRAM - UM10736 Rev 1.2 2017 Ch 34.7.11 Table 489 */
    { 0x00001548,  0x20000,  0x5000 },  /* LPC1548 - M3 128K Flash 20K SRAM - UM10736 Rev 1.2 2017 Ch 34.7.11 Table 489 */
    { 0x00001549,  0x40000,  0x9000 },  /* LPC1549 - M3 256K Flash 36K SRAM - UM10736 Rev 1.2 2017 Ch 34.7.11 Table 489 */
    { 0x25001118,   0x8000,  0x2000 },  /* LPC1751 - M3 32K Flash 8K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
    { 0x25001110,   0x8000,  0x2000 },  /* LPC1751 (No CRP) - M3 32K Flash 8K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
    { 0x25001121,  0x10000,  0x4000 },  /* LPC1752 - M3 64K Flash 16K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
    { 0x25011722,  0x20000,  0x8000 },  /* LPC1754 - M3 128K Flash 32K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
    { 0x25011723,  0x40000,  0x8000 },  /* LPC1756 - M3 256K Flash 32K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
    { 0x25013F37,  0x80000, 0x10000 },  /* LPC1758 - M3 512K Flash 64K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
    { 0x25113737,  0x80000, 0x10000 },  /* LPC1759 - M3 512K Flash 64K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
    { 0x26012033,  0x40000, 0x10000 },  /* LPC1763 - M3 256K Flash 64K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
    { 0x26011922,  0x20000,  0x8000 },  /* LPC1764 - M3 128K Flash 32K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
    { 0x26013733,  0x40000, 0x10000 },  /* LPC1765 - M3 256K Flash 64K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
    { 0x26013F33,  0x40000, 0x10000 },  /* LPC1766 - M3 256K Flash 64K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
    { 0x26012837,  0x80000, 0x10000 },  /* LPC1767 - M3 512K Flash 64K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
    { 0x26013F37,  0x80000, 0x10000 },  /* LPC1768 - M3 512K Flash 64K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
    { 0x26113F37,  0x80000, 0x10000 },  /* LPC1769 - M3 512K Flash 64K SRAM - UM10360 Rev 4.1 2016 Ch 32.7.11 Table 584 */
    { 0x27011132,  0x20000,  0x8000 },  /* LPC1774 - M3 128K Flash 32K SRAM - UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
    { 0x27191F43,  0x40000, 0x10000 },  /* LPC1776 - M3 256K Flash 64K SRAM - UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
    { 0x27193747,  0x80000, 0x10000 },  /* LPC1777 - M3 512K Flash 64K SRAM - UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
    { 0x27193F47,  0x80000, 0x10000 },  /* LPC1778 - M3 512K Flash 64K SRAM - UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
    { 0x281D1743,  0x40000, 0x10000 },  /* LPC1785 - M3 256K Flash 64K SRAM - UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
    { 0x281D1F43,  0x40000, 0x10000 },  /* LPC1786 - M3 256K Flash 64K SRAM - UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
    { 0x281D3747,  0x80000, 0x10000 },  /* LPC1787 - M3 512K Flash 64K SRAM - UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
    { 0x281D3F47,  0x80000, 0x10000 },  /* LPC1788 - M3 512K Flash 64K SRAM - UM10470 Rev 4.0 2016 Ch 37.7.11 Table 747 */
    { 0x5284E02B,        0, 0x22000 },  /* LPC18[S]x0 - M3 no Flash 104K~136K SRAM - UM10430 Rev 3.1 2019 Ch 10.4.10 Table 97, note: single "CHIP ID" for a group of MCUs with different memory sizes */
    { 0x6284E02B,        0, 0x22000 },  /* LPC18[S]x0 - M3 no Flash 104K~136K SRAM - UM10430 Rev 3.1 2019 Ch 10.4.10 Table 97, note: single "CHIP ID" for a group of MCUs with different memory sizes */
    { 0x4284E02B, 0x100000, 0x12000 },  /* LPC18[S]xx - M3 512K~1M Flash 72K SRAM - UM10430 Rev 3.1 2019 Ch 10.4.10 Table 97, note: single "CHIP ID" for a group of MCUs with different memory sizes */
    { 0x7284E02B, 0x100000, 0x12000 },  /* LPC18[S]xx - M3 512K~1M Flash 72K SRAM - UM10430 Rev 3.1 2019 Ch 10.4.10 Table 97, note: single "CHIP ID" for a group of MCUs with different memory sizes */
    { 0x47011132,  0x20000,  0xa000 },  /* LPC4074 - M4 128K Flash 40K SRAM - UM10562 Rev 3 2014 Ch 38.7.11 Table 753 */
    { 0x47191F43,  0x40000, 0x14000 },  /* LPC4076 - M4 256K Flash 80K SRAM - UM10562 Rev 3 2014 Ch 38.7.11 Table 753 */
    { 0x47193F47,  0x80000, 0x18000 },  /* LPC4078 - M4 512K Flash 96K SRAM - UM10562 Rev 3 2014 Ch 38.7.11 Table 753 */
    { 0x481D3F47,  0x80000, 0x18000 },  /* LPC4088 - M4 512K Flash 96K SRAM - UM10562 Rev 3 2014 Ch 38.7.11 Table 753 */
    { 0x5906002B,        0, 0x32000 },  /* LPC43[S]x0 - M4/M0 no Flash 136K~200K SRAM - UM10503 Rev 2.3 2017, Ch 11.4.11 Table 106, note: single "CHIP ID" for a group of MCUs with different memory sizes */
    { 0x6906002B,        0, 0x32000 },  /* LPC43[S]x0 - M4/M0 no Flash 136K~200K SRAM - UM10503 Rev 2.3 2017, Ch 11.4.11 Table 106, note: single "CHIP ID" for a group of MCUs with different memory sizes */
    { 0x4906002B, 0x100000, 0x12000 },  /* LPC43[S]xx - M4/M0 512K~1M Flash 72K SRAM - UM10503 Rev 2.3 2017, Ch 11.4.11 Table 106, note: single "CHIP ID" for a group of MCUs with different memory sizes */
    { 0x7906002B, 0x100000, 0x12000 },  /* LPC43[S]xx - M4/M0 512K~1M Flash 72K SRAM - UM10503 Rev 2.3 2017, Ch 11.4.11 Table 106, note: single "CHIP ID" for a group of MCUs with different memory sizes */
    { 0x7F954605,  0x40000, 0x20000 },  /* LPC54605J256 - M4 256K Flash 128K SRAM - UM10912 Rev 2.4 2019 Ch 7.5.99 Table 232 */
    { 0xFFF54605,  0x80000, 0x30000 },  /* LPC54605J512 - M4 512K Flash 192K SRAM - UM10912 Rev 2.4 2019 Ch 7.5.99 Table 232 */
    { 0x7F954606,  0x40000, 0x20000 },  /* LPC54606J256 - M4 256K Flash 128K SRAM - UM10912 Rev 2.4 2019 Ch 7.5.99 Table 232 */
    { 0xFFF54606,  0x80000, 0x30000 },  /* LPC54606J512 - M4 512K Flash 192K SRAM - UM10912 Rev 2.4 2019 Ch 7.5.99 Table 232 */
    { 0x7F954607,  0x40000, 0x20000 },  /* LPC54607J256 - M4 256K Flash 128K SRAM - UM10912 Rev 2.4 2019 Ch 7.5.99 Table 232 */
    { 0xFFF54607,  0x80000, 0x30000 },  /* LPC54607J512 - M4 512K Flash 192K SRAM - UM10912 Rev 2.4 2019 Ch 7.5.99 Table 232 */
    { 0xFFF54608,  0x80000, 0x30000 },  /* LPC54608J512 - M4 512K Flash 192K SRAM - UM10912 Rev 2.4 2019 Ch 7.5.99 Table 232 */
    { 0x7F954616,  0x40000, 0x20000 },  /* LPC54616J256 - M4 256K Flash 128K SRAM - UM10912 Rev 2.4 2019 Ch 7.5.99 Table 232 */
    { 0xFFF54616,  0x80000, 0x30000 },  /* LPC54616J512 - M4 512K Flash 192K SRAM - UM10912 Rev 2.4 2019 Ch 7.5.99 Table 232 */
    { 0xFFF54618,  0x80000, 0x30000 },  /* LPC54618J512 - M4 512K Flash 192K SRAM - UM10912 Rev 2.4 2019 Ch 7.5.99 Table 232 */
    { 0xFFF54628,  0x80000, 0x30000 }   /* LPC54628J512 - M4 512K Flash 192K SRAM - UM10912 Rev 2.4 2019 Ch 7.5.99 Table 232 */
};

static const struct lpc_meminfo_s *lpc_get_meminfo(uint32_t device_id)
{
    for (size_t idx = 0; idx < sizeof(lpc_meminfo) / sizeof(lpc_meminfo[0]); idx++)
        if (lpc_meminfo[idx].device_id == device_id)
            return &lpc_meminfo[idx];
    return NULL;
}

size_t lpc_flash_size(uint32_t device_id, size_t default_size)
{
    const struct lpc_meminfo_s *info = lpc_get_meminfo(device_id);
    return info ? info->flash : default_size;
}

size_t lpc_sram_size(uint32_t device_id, size_t default_size)
{
    const struct lpc_meminfo_s *info = lpc_get_meminfo(device_id);
    return info ? info->sram : default_size;
}

