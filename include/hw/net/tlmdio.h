/*
 * FZI MDIO controller emulation
 *
 * Copyright (c) 2024 FZI Forschungszentrum Informatik
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TLMDIO_H
#define HW_TLMDIO_H

#include "net/net.h"
#include <stdint.h>

enum {
    MDIO_PERIPHIDENT = 0x000, // R 32
    MDIO_CONTROL = 0x008, // R/RW 9
    MDIO_DATA_1 = 0x100, // R/RW 32
    MDIO_DATA_2 = 0x104, // R/RW 32
    MDIO_REG_MAX = 0x1000
};

enum {
    MDIO_CTRL_ENABLE = 0x001, // RW
    MDIO_CTRL_FREERUNNING = 0x002, // RW NOT RELEVANT IN QEMU
    MDIO_CTRL_PREAMBLESKIP = 0x004, // RW NOT RELEVANT IN QEMU
    MDIO_CTRL_RESERVED = 0x008, // R  NOT USED
    MDIO_CTRL_SLOTS =
        0x1F0, // R  MULTIPLE ISSUE FOR R/W -> CURRENTLY ONLY SLOT0

    MDIO_DATA_DATA = 0x0000FFFF, // RW
    MDIO_DATA_RESERVED = 0x00030000, // R
    MDIO_DATA_RA5 = 0x007C0000, // RW
    MDIO_DATA_PA5 = 0x0F800000, // RW
    MDIO_DATA_OP = 0x30000000, // RW
    MDIO_DATA_STATUS = 0xC0000000, // RW
};

enum {
    RIFMDIO_SLOT_STAT_MASK = 0xC0000000,
    RIFMDIO_SLOT_STAT_IDLE = 0x00000000,

    RIFMDIO_SLOT_WRITE = 0x10000000,
    RIFMDIO_SLOT_READ = 0x20000000,

    RIFMDIO_SLOT_STAT_ACT = 0x40000000,
    RIFMDIO_SLOT_STAT_OK = 0x80000000,
    RIFMDIO_SLOT_STAT_ERR = 0xC0000000
};

#define TYPE_FZI_MDIO "fzi.tlmdio"

typedef struct MdioState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    qemu_irq irq[1];
    MemoryRegion mmio;

    uint32_t periphident;
    uint32_t control;
    uint32_t data_1;
    uint32_t data_2;

    uint16_t dummyRegs[2][32];
} MdioState;

MdioState *mdio_create(MemoryRegion *address_space, hwaddr base, qemu_irq irq);
#endif
