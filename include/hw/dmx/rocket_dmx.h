/*
 * FZI RocketDMX controller emulation
 *
 * Copyright (c) 2024 FZI Forschungszentrum Informatik
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DMX_H
#define HW_DMX_H

#include "qemu/osdep.h"

#include "hw/core/sysbus.h"
#include "qemu/queue.h"
#include "qemu/timer.h"

#include "ui/console.h"
#include "ui/qemu-pixman.h"

#include "hw/display/dmx.h"

enum {
    DMX_IF_DATA_START = 0x000, //
    DMX_IF_DATA_END = 0x800, // RW
    DMX_IF_FRAME_CTRL =
        0x800, // RW 32 (bit 25: one to clear, bit 28: one to set, bit 29-30:R)
    DMX_IF_MAX = 0x900
};

enum {
    DMX_IF_START_BYTE0_MASK = 0x000000FF, // RW
    DMX_IF_START_BYTE0_SHIFT = 0,
    DMX_IF_START_BYTE1_MASK = 0x0000FF00, // RW
    DMX_IF_START_BYTE1_SHIFT = 8,
    DMX_IF_LIMIT_MASK = 0x01FF0000, // RW
    DMX_IF_LIMIT_SHIFT = 16,
    DMX_IF_IF_MASK = 0x02000000, // RW (ONE_TO_CLEAR)
    DMX_IF_IF_SHIFT = 25,
    DMX_IF_IE_MASK = 0x04000000, // RW
    DMX_IF_IE_SHIFT = 26,
    DMX_IF_DB_MASK = 0x08000000, // RW
    DMX_IF_DB_SHIFT = 27,
    DMX_IF_PF_PEND_MASK = 0x10000000, // RW (ONE_TO_SET)
    DMX_IF_PF_PEND_SHIFT = 28,
    DMX_IF_CUR_PAGE_MASK = 0x20000000, // R
    DMX_IF_CUR_PAGE_SHIFT = 29,
    DMX_IF_TRANS_BZ_MASK = 0x40000000, // R
    DMX_IF_TRANS_BZ_SHIFT = 30,
    DMX_IF_TRANS_EN_MASK = 0x80000000, // RW
    DMX_IF_TRANS_EN_SHIFT = 31,

    DMX_WRITE_ONE_TO_CLEAR_MASK = DMX_IF_IF_MASK,
    DMX_WRITE_ONE_TO_SET_MASK = DMX_IF_PF_PEND_MASK,
    DMX_WRITE_MASK = DMX_IF_START_BYTE0_MASK | DMX_IF_START_BYTE1_MASK |
                     DMX_IF_LIMIT_MASK | DMX_IF_IE_MASK | DMX_IF_DB_MASK |
                     DMX_IF_TRANS_EN_MASK
};

#define TYPE_FZI_DMX "fzi.dmx-controller"

#define DMX_PAGES 2
#define DMX_PAGE_SIZE 512

typedef struct DMXState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    qemu_irq irq;
    MemoryRegion mmio;

    uint8_t dmxPage[DMX_PAGES][DMX_PAGE_SIZE];
    uint32_t ctrl;

    QEMUTimer *timer;
    DeviceState *display;
} DMXState;

DMXState *dmx_if_create(MemoryRegion *address_space, hwaddr base, qemu_irq irq);
#endif
