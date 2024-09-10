/*
 * FZI DMX display emulation
 *
 * Copyright (c) 2024 FZI Forschungszentrum Informatik
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/display/dmx.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include <math.h>

// #define PRINT_TO_CONSOLE

#define FZI_DMXDISPLAY_OBJ(obj) \
    OBJECT_CHECK(DmxDisplayState, (obj), TYPE_FZI_DMXDISPLAY)

uint8_t PALETTE[16][3] = {
    { 0x00, 0x00, 0x00 }, // 0
    { 0xFF, 0x00, 0x00 }, // 1
    { 0xFF, 0x2A, 0x00 }, // 2
    { 0xFF, 0x55, 0x00 }, // 3
    { 0xFF, 0x80, 0x00 }, // 4
    { 0xFF, 0xAA, 0x00 }, // 5
    { 0xFF, 0xD5, 0x00 }, // 6
    { 0xFF, 0xFF, 0x00 }, // 7
    { 0xD4, 0xFF, 0x00 }, // 8
    { 0xAA, 0xFF, 0x00 }, // 9
    { 0x80, 0xFF, 0x00 }, // 10
    { 0x55, 0xFF, 0x00 }, // 11
    { 0x2B, 0xFF, 0x00 }, // 12
    { 0x00, 0xFF, 0x00 }, // 13
    { 0xFF, 0x00, 0x00 }, // 14
    { 0xFF, 0xFF, 0x00 }, // 15
};

#ifndef PRINT_TO_CONSOLE
static void dmx_graphical_update(DmxDisplayState *s)
{
    if (s->ds == NULL) {
        /* video mode switch */
        s->ds = qemu_create_displaysurface_from(
            DISPLAY_WIDTH, DISPLAY_HEIGHT, PIXMAN_r8g8b8a8, DISPLAY_WIDTH * 4,
            s->frameBuffer[0][0]);
        qemu_console_set_surface(s->con, s->ds);
    }
    qemu_console_update_full(s->con);
}

static bool dmx_display_update(void *opaque)
{
    DmxDisplayState *s = FZI_DMXDISPLAY_OBJ(opaque);

    if (s->fbDirty) {
        s->fbDirty = 0;

        float facH = DISPLAY_HEIGHT / s->height;
        float facW = DISPLAY_WIDTH / s->width;

        uint8_t pos = 0;
        uint8_t *dataPtr = s->dataBuffer;
        for (int y = 0; y < s->height; ++y) {
            for (int x = 0; x < s->width; ++x) {
                uint8_t c = 0;
                if (pos == 0) {
                    c = *dataPtr >> 0x4;
                } else {
                    c = *dataPtr & 0x0f;
                    ++dataPtr;
                }
                pos = !pos;

                uint8_t r = PALETTE[c][2];
                uint8_t g = PALETTE[c][1];
                uint8_t b = PALETTE[c][0];
                if (c < 14) {
                    r /= 1.4;
                    g /= 1.4;
                    b /= 1.4;
                }

                uint16_t startX = (s->width - x - 1) * facW;
                uint16_t endX = (s->width - x) * facW;
                uint16_t startY = y * facH;
                uint16_t endY = (y + 1) * facH;

                for (int yD = startY; yD < endY; ++yD) {
                    for (int xD = startX; xD < endX; ++xD) {
                        s->frameBuffer[yD][xD][0] = r;
                        s->frameBuffer[yD][xD][1] = g;
                        s->frameBuffer[yD][xD][2] = b;
                        s->frameBuffer[yD][xD][3] = 0xff;
                    }
                }
            }
        }

        dmx_graphical_update(s);
        return true;
    }

    return false;
}

#else

#define BLOCK100 "\u2588"
#define BLOCK75 "\u2593"
#define BLOCK50 "\u2592"
#define BLOCK25 "\u2591"
#define BLOCK0 " "
#define START_RGB "\x1b[38;2;%u;%u;%um"
#define END_COLOR "\x1b[0m"

static void dmx_display_update(void *opaque)
{
    DmxDisplayState *s = FZI_DMXDISPLAY_OBJ(opaque);

    const char *CLEAR_SCREEN_ANSI = "\e[1;1H\e[2J";
    int res = write(STDOUT_FILENO, CLEAR_SCREEN_ANSI, 12);
    if (res < 0) {
        printf("UNSUPPORTED DMX DISPLAY\n");
        return;
    }

    printf("\u2554");
    for (int x = 0; x < s->width; ++x) {
        printf("\u2550");
    }
    printf("\u2557\n");

    int pos = 0;
    uint8_t *dataPtr = s->dataBuffer;
    for (int x = 0; x < s->width; ++x) {
        printf("\u2551");
        for (int y = 0; y < s->height; ++y) {
            uint8_t c = 0;
            if (pos == 0) {
                c = *dataPtr >> 0x4;
            } else {
                c = *dataPtr & 0x0f;
                ++dataPtr;
            }
            pos = !pos;

            uint8_t r = PALETTE[c][2];
            uint8_t g = PALETTE[c][1];
            uint8_t b = PALETTE[c][0];
            if (c < 14) {
                r /= 1.4;
                g /= 1.4;
                b /= 1.4;
            }
            uint8_t max = r > g ? (r > b ? r : b) : (g > b ? g : b);
            uint8_t decre = 256 / 5;

            if (max > (256 - decre)) {
                printf(START_RGB BLOCK100 END_COLOR, r, g, b);
            } else if (max > (256 - 2 * decre)) {
                printf(START_RGB BLOCK75 END_COLOR, r, g, b);
            } else if (max > (256 - 3 * decre)) {
                printf(START_RGB BLOCK50 END_COLOR, r, g, b);
            } else if (max > (256 - 4 * decre)) {
                printf(START_RGB BLOCK25 END_COLOR, r, g, b);
            } else {
                printf(START_RGB BLOCK0 END_COLOR, r, g, b);
            }
        }
        printf("\u2551\n");
    }

    printf("\u255A");
    for (int x = 0; x < s->width; ++x) {
        printf("\u2550");
    }
}
#endif

void dmx_display_set_dirty(DeviceState *ds)
{
    DmxDisplayState *s = FZI_DMXDISPLAY_OBJ(ds);
    s->fbDirty = 1;
}

void dmx_display_set_fb(DeviceState *ds, uint16_t frameLen, uint8_t *dataPtr)
{
    DmxDisplayState *s = FZI_DMXDISPLAY_OBJ(ds);
    frameLen = MIN(frameLen, sizeof(s->dataBuffer));
    uint8_t height = sqrt(frameLen * 2);
    uint8_t width = height;

    s->height = height;
    s->width = width;
    memcpy(&s->dataBuffer, dataPtr, frameLen);
    dmx_display_update(s);
}

static void dmx_display_invalidate(void *opaque)
{
    DmxDisplayState *s = FZI_DMXDISPLAY_OBJ(opaque);
    memset(s->frameBuffer[0][0], 0, sizeof(s->frameBuffer));
    memset(s->dataBuffer, 0, sizeof(s->dataBuffer));
    s->height = 1;
    s->width = 1;

    dmx_display_set_dirty((DeviceState *)opaque);
    dmx_display_update((DeviceState *)opaque);
}


static const GraphicHwOps dmx_display_gfx_ops = {
    .gfx_update = dmx_display_update,
    .invalidate = dmx_display_invalidate,
};

void dmx_display_reset(DeviceState *ds)
{
    dmx_display_invalidate(ds);
}

static void dmx_display_init(Object *obj)
{
}

static void dmx_display_realize(DeviceState *dev, Error **errp)
{
    DmxDisplayState *s = FZI_DMXDISPLAY_OBJ(dev);
    s->con = qemu_graphic_console_create(dev, 0, &dmx_display_gfx_ops, s);
    qemu_console_resize(s->con, s->width, s->height);
}

static const Property dmx_display_properties[] = {
    DEFINE_PROP_UINT16("height", DmxDisplayState, height, 30),
    DEFINE_PROP_UINT16("width", DmxDisplayState, width, 30),
    DEFINE_PROP_UINT8("fbDirty", DmxDisplayState, fbDirty, 1),
};

static void dmx_display_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    device_class_set_legacy_reset(dc, dmx_display_reset);
    dc->realize = dmx_display_realize;
    device_class_set_props(dc, dmx_display_properties);
}

static const TypeInfo dmx_display_info = {
    .name = TYPE_FZI_DMXDISPLAY,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DmxDisplayState),
    .instance_init = dmx_display_init,
    .class_init = dmx_display_class_init,
};

static void dmx_display_register_types(void)
{
    type_register_static(&dmx_display_info);
}

type_init(dmx_display_register_types)

    DeviceState *dmx_display_create(uint16_t width, uint16_t height)
{
    DeviceState *dev = qdev_new(TYPE_FZI_DMXDISPLAY);
    qdev_prop_set_uint16(dev, "width", width);
    qdev_prop_set_uint16(dev, "height", height);
    qdev_prop_set_uint8(dev, "fbDirty", 1);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    return dev;
}
