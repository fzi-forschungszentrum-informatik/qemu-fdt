/*
 * SiFive CLINT (Core Local Interruptor) interface
 * FZI: This is more or less a copy of the RISC-V ACLINT, but in one device
 *
 * Copyright (c) 2016-2017 Sagar Karandikar, sagark@eecs.berkeley.edu
 * Copyright (c) 2017 SiFive, Inc.
 * Copyright (c) 2021 Western Digital Corporation or its affiliates.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_SIFIVE_CLINT_H
#define HW_SIFIVE_CLINT_H

#include "hw/core/sysbus.h"

#define TYPE_SIFIVE_CLINT "sifive.clint0"

#define SIFIVE_CLINT(obj) \
    OBJECT_CHECK(SifiveClintState, (obj), TYPE_SIFIVE_CLINT)

typedef struct SifiveClintState {
    /*< private >*/
    SysBusDevice parent_obj;
    uint64_t time_delta;
    uint64_t *timecmp;
    QEMUTimer **timers;

    /*< public >*/
    /* MTIMER */
    MemoryRegion mmio;
    uint32_t hartid_base;
    uint32_t num_harts;
    uint32_t timecmp_base;
    uint32_t time_base;
    uint32_t aperture_size;
    uint32_t timebase_freq;
    /* SWI */
    uint32_t sswi;
    /* MTIMER + SWI*/
    qemu_irq *irqs; // hart * mtime + hart * soft
} SifiveClintState;

DeviceState *sifive_clint_create(hwaddr addr, hwaddr size,
    uint32_t hartid_base, uint32_t num_harts,
    uint32_t timecmp_base, uint32_t time_base, uint32_t timebase_freq,
    bool provide_rdtime, bool sswi);

enum {
    SIFIVE_CLINT_DEFAULT_MTIMECMP      = 0x0,
    SIFIVE_CLINT_DEFAULT_MTIME         = 0x7ff8,
    SIFIVE_CLINT_DEFAULT_MTIMER_SIZE   = 0x8000,
    SIFIVE_CLINT_DEFAULT_TIMEBASE_FREQ = 1000000,
    SIFIVE_CLINT_MAX_HARTS             = 4095,
    SIFIVE_CLINT_SWI_SIZE              = 0x4000
};

#endif
