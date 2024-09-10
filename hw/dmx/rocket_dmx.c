/*
 * FZI RocketDMX controller emulation
 *
 * Copyright (c) 2024 FZI Forschungszentrum Informatik
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "hw/dmx/rocket_dmx.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"

#define FZI_DMX_OBJ(obj) OBJECT_CHECK(DMXState, (obj), TYPE_FZI_DMX)


static inline uint8_t getPage(uint32_t ctrl)
{
    return ((ctrl & DMX_IF_CUR_PAGE_MASK) >> DMX_IF_CUR_PAGE_SHIFT);
}

static inline void dmx_display_update(DMXState *s)
{
    uint8_t page = getPage(s->ctrl);
    uint16_t frameLen =
        ((s->ctrl & DMX_IF_LIMIT_MASK) >> DMX_IF_LIMIT_SHIFT) + 1;
    uint8_t *dataPtr = s->dmxPage[page];

    if (s->display)
        dmx_display_set_fb(s->display, frameLen, dataPtr);
}


static inline void dmx_trans_ev_timer(DMXState *s)
{
    if ((s->ctrl & DMX_IF_DB_MASK) && (s->ctrl & DMX_IF_PF_PEND_MASK)) {
        s->ctrl &= ~DMX_IF_PF_PEND_MASK;
        s->ctrl ^= DMX_IF_CUR_PAGE_MASK;
        if (s->display)
            dmx_display_set_dirty(s->display);
    }

    if ((s->ctrl & DMX_IF_TRANS_EN_MASK) && s->timer != NULL) {
        s->ctrl |= DMX_IF_TRANS_BZ_MASK;
        uint16_t frameLen =
            ((s->ctrl & DMX_IF_LIMIT_MASK) >> DMX_IF_LIMIT_SHIFT) +
            2; // Start byte + 1 indexed Frame
        uint32_t frameTransTime =
            96 + 12 + frameLen * 44; // T_Break + T_Mark + #Bytes * T_Byte
        int64_t next = qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + frameTransTime;
        timer_mod(s->timer, next);
    } else {
        s->ctrl &= ~DMX_IF_TRANS_BZ_MASK;
    }

    dmx_display_update(s);
}

static inline void dmx_raise_irq(DMXState *s)
{
    s->ctrl |= DMX_IF_IF_MASK;
    if (s->ctrl & DMX_IF_IE_MASK) {
        assert(s->irq != NULL);
        qemu_irq_raise(s->irq);
    }
}

static inline void dmx_tx_finished_cb(void *opaque)
{
    DMXState *s = (DMXState *)opaque;
    dmx_trans_ev_timer(s);
    dmx_raise_irq(s);
}


static uint64_t dmx_read(void *opaque, hwaddr addr, unsigned int size)
{
    uint64_t r = 0;
    DMXState *s = opaque;
    (void)s;

    if (addr < DMX_IF_DATA_END) {
        switch (size) {
        case 1:
            r = (uint8_t)s->dmxPage[getPage(s->ctrl)][addr >> 2];
            break;
        case 2:
            r = (uint16_t)s->dmxPage[getPage(s->ctrl)][addr >> 2];
            break;
        case 4:
            r = s->dmxPage[getPage(s->ctrl)][addr >> 2];
            break;
        default:
            error_report("dmx: invalid register read: %08x (%d bytes)",
                         (uint32_t)addr, size);
        }
    } else if (addr == DMX_IF_FRAME_CTRL) {
        if (size == 4) {
            r = s->ctrl;
        } else {
            error_report("dmx: invalid register read: %08x (%d bytes)",
                         (uint32_t)addr, size);
        }
    } else {
        error_report("dmx: invalid register read: %08x (%d bytes)",
                     (uint32_t)addr, size);
    }

    // qemu_log("%s addr=" HWADDR_FMT_plx  " val=%lx\n", __func__, addr, r);
    return r;
}

static void dmx_write(void *opaque, hwaddr addr, uint64_t val64,
                      unsigned int size)
{
    // qemu_log("%s addr=" HWADDR_FMT_plx  " val=%lx\n", __func__, addr, val64);

    DMXState *s = opaque;
    if (size == 4) {
        if (addr < DMX_IF_DATA_END) {
            if (size == 4) {
                s->dmxPage[getPage(s->ctrl)][addr >> 2] = val64;
                if (!(s->ctrl & DMX_IF_DB_MASK) &&
                    (s->ctrl & DMX_IF_TRANS_EN_MASK)) {
                    if (s->display)
                        dmx_display_set_dirty(s->display);
                }
            }
        } else if (addr == DMX_IF_FRAME_CTRL) {
            if (size == 4) {
                uint32_t writeVal32 = val64 & DMX_WRITE_MASK;
                uint32_t oneToCleanMask = val64 & DMX_WRITE_ONE_TO_CLEAR_MASK;
                uint32_t oneToSetMask = val64 & DMX_WRITE_ONE_TO_SET_MASK;
                uint32_t onlyReadMask =
                    ~(DMX_WRITE_MASK | oneToCleanMask | oneToSetMask);

                s->ctrl = (s->ctrl & onlyReadMask) | writeVal32 | oneToSetMask;

                if (s->ctrl & DMX_IF_TRANS_EN_MASK) {
                    if (s->timer == NULL) {
                        if (s->display)
                            dmx_display_set_dirty(s->display);
                        s->ctrl |= DMX_IF_TRANS_BZ_MASK;
                        s->timer = timer_new_us(QEMU_CLOCK_VIRTUAL,
                                                &dmx_tx_finished_cb, s);
                        dmx_trans_ev_timer(s);
                    }
                } else {
                    if (s->display)
                        dmx_display_reset(s->display);

                    s->timer = NULL;
                }
            }
        } else {
            error_report("dmx: invalid register write: %08x=%08lx (%d bytes)",
                         (uint32_t)addr, val64, size);
        }
    } else {
        /* writes must be 4 byte words */
        error_report("dmx: invalid register write: %08x=%08lx (%d bytes)",
                     (uint32_t)addr, val64, size);
    }
}

static const MemoryRegionOps dmx_ops = { .read = dmx_read,
                                         .write = dmx_write,
                                         .endianness = DEVICE_NATIVE_ENDIAN,
                                         .valid = { .min_access_size = 4,
                                                    .max_access_size = 4 } };

static void fzi_dmx_reset(DeviceState *dev)
{
    DMXState *s = FZI_DMX_OBJ(dev);
    if (s->display)
        dmx_display_reset(s->display);
    s->ctrl = 511 << DMX_IF_LIMIT_SHIFT; // All other equal 0
}


static void fzi_dmx_realize(DeviceState *dev, Error **errp)
{
    DMXState *s = FZI_DMX_OBJ(dev);
    qdev_init_gpio_out(dev, &s->irq, 1);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    s->timer = NULL;

    /* TODO: Do it correctly */
    s->display = dmx_display_create(1, 1);
}

static void fzi_dmx_init(Object *obj)
{
    DMXState *s = FZI_DMX_OBJ(obj);

    memory_region_init_io(&s->mmio, obj, &dmx_ops, s, TYPE_FZI_DMX, DMX_IF_MAX);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void fzi_dmx_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, fzi_dmx_reset);

    dc->realize = fzi_dmx_realize;
}


static const TypeInfo fzi_dmx_info = {
    .name = TYPE_FZI_DMX,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DMXState),
    .instance_init = fzi_dmx_init,
    .class_init = fzi_dmx_class_init,
};

static void fzi_dmx_register_types(void)
{
    type_register_static(&fzi_dmx_info);
}

type_init(fzi_dmx_register_types)

    /*
     * Create Rocket NIC device.
     */
    DMXState *dmx_if_create(MemoryRegion *address_space, hwaddr base,
                            qemu_irq irq)
{
    DeviceState *dev = qdev_new(TYPE_FZI_DMX);
    DMXState *s = (DMXState *)dev;

    s->irq = irq;
    s->timer = NULL;

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);

    return s;
}
