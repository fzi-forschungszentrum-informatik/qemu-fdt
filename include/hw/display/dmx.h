/*
 * FZI DMX display emulation
 *
 * Copyright (c) 2024 FZI Forschungszentrum Informatik
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef INCLUDE_HW_RISCV_DMXDISPLAY_H_
#define INCLUDE_HW_RISCV_DMXDISPLAY_H_

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "ui/console.h"
#include "ui/qemu-pixman.h"

#define DISPLAY_HEIGHT 512
#define DISPLAY_WIDTH 512
#define DISPLAY_CHANNEL 3
#define MAX_DMX_DATA 512

#define TYPE_FZI_DMXDISPLAY "fzi.dmx-display"

typedef struct DmxDisplayState {
    /*< private >*/
    SysBusDevice parent_obj;

    uint8_t fbDirty;
    uint8_t frameBuffer[DISPLAY_HEIGHT][DISPLAY_WIDTH][4];
    uint8_t dataBuffer[MAX_DMX_DATA];

    uint16_t height;
    uint16_t width;

    QemuConsole *con;
    DisplaySurface *ds;
} DmxDisplayState;

DeviceState *dmx_display_create(uint16_t width, uint16_t height);

void dmx_display_reset(DeviceState *s);
void dmx_display_set_dirty(DeviceState *dev);
void dmx_display_set_fb(DeviceState *s, uint16_t frameLen, uint8_t *dataPtr);

#endif /* INCLUDE_HW_RISCV_DMXDISPLAY_H_ */
