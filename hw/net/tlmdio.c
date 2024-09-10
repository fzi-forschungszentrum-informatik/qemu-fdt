/*
 * FZI MDIO controller emulation
 *
 * Copyright (c) 2024 FZI Forschungszentrum Informatik
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/net/tlmdio.h"
#include "net/net.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/queue.h"

#define FZI_MDIO(obj) OBJECT_CHECK(MdioState, (obj), TYPE_FZI_MDIO)

// static inline void mdio_pulse_irq(MdioState *s)
// {
//     if (s->control & MDIO_CTRL_ENABLE) {
//         assert(s->irq[0] != NULL);
//         qemu_irq_raise(s->irq[0]);
//     }
// }

static void resetDummys(MdioState *s)
{
  uint16_t zcu102_mdio_regs[] = {
      0x1540, 0x01E8, 0x0174, 0x0C00, 0x01A0, 0x4020, 0x0004, 0x0000,
      0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x8000,
      0x0001, 0x0000, 0x0003, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
      0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000};
  uint16_t artyT100_mdio_regs[] = {
      0x3100, 0x786D, 0x2000, 0x5C90, 0x01E1, 0xC1E1, 0x000D, 0x2801,
      0x0000, 0x0000, 0x4815, 0x0000, 0x2E00, 0x0000, 0x00FF, 0x0000,
      0x0100, 0x0001, 0x0000, 0x8001, 0x3100, 0x786D, 0x2000, 0x5C90,
      0x01E1, 0xC1E1, 0x000D, 0x0000, 0x0000, 0x0000, 0x4615, 0x0000};

  memcpy(s->dummyRegs[0], zcu102_mdio_regs, sizeof(zcu102_mdio_regs));
  memcpy(s->dummyRegs[1], artyT100_mdio_regs, sizeof(artyT100_mdio_regs));
}

static uint16_t mdio_read_req(MdioState *s, uint8_t phy, uint8_t addr)
{
    if (phy > 1)
        return 0xffff;
    return s->dummyRegs[phy][addr];
};

static void mdio_write_req(MdioState *s, uint8_t phy, uint8_t addr,
                           uint16_t val)
{
    if (phy > 1)
        return;
    s->dummyRegs[phy][addr] = val;
};

static void mdio_req(MdioState *s, uint32_t *frame)
{
    if (*frame & RIFMDIO_SLOT_STAT_ACT) {
        uint8_t op = (*frame & MDIO_DATA_OP) >> 28;
        uint8_t pa5 = (*frame & MDIO_DATA_PA5) >> 23;
        uint8_t ra5 = (*frame & MDIO_DATA_RA5) >> 18;
        uint16_t data = (*frame & MDIO_DATA_DATA) >> 16;

        switch (op) {
        case 0b10:;
            uint16_t ret = mdio_read_req(s, pa5, ra5);
            *frame = (*frame & 0xFFFF0000) | ret;
            *frame = (*frame & ~RIFMDIO_SLOT_STAT_ACT) | RIFMDIO_SLOT_STAT_OK;
            break;
        case 0b01:
            mdio_write_req(s, pa5, ra5, data);
            *frame = (*frame & ~RIFMDIO_SLOT_STAT_ACT) | RIFMDIO_SLOT_STAT_OK;
            break;
        default:
            *frame |= RIFMDIO_SLOT_STAT_ERR;
        }
    }
};

static uint64_t mdio_read(void *opaque, hwaddr addr, unsigned int size)
{
    uint32_t r = 0;
    MdioState *s = opaque;

    switch (addr) {
    case MDIO_PERIPHIDENT: // R 32
        r = s->periphident;
        break;
    case MDIO_CONTROL: // R/RW 9
        r = s->control &
            (MDIO_CTRL_ENABLE | MDIO_CTRL_FREERUNNING | MDIO_CTRL_PREAMBLESKIP |
             MDIO_CTRL_RESERVED | MDIO_CTRL_SLOTS);
        break;
    case MDIO_DATA_1: // R/RW 32
        r = s->data_1;
        break;
    case MDIO_DATA_2: // R/RW 32
        r = s->data_2;
        break;
    default:
        break;
    }

    return r;
}

static void mdio_write(void *opaque, hwaddr addr, uint64_t val64,
                       unsigned int size)
{
    MdioState *s = opaque;
    uint32_t value = val64;

    switch (addr) {
    case MDIO_CONTROL: // R/RW 9
        s->control = value & (MDIO_CTRL_ENABLE | MDIO_CTRL_FREERUNNING |
                              MDIO_CTRL_PREAMBLESKIP);
        break;
    case MDIO_DATA_1: // R/RW 32
        s->data_1 = value & ~MDIO_DATA_RESERVED;
        mdio_req(s, &s->data_1);
        break;
    case MDIO_DATA_2: // R/RW 32
        s->data_2 = value & ~MDIO_DATA_RESERVED;
        mdio_req(s, &s->data_2);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps mdio_ops = { .read = mdio_read,
                                          .write = mdio_write,
                                          .endianness = DEVICE_NATIVE_ENDIAN,
                                          .valid = { .min_access_size = 4,
                                                     .max_access_size = 4 } };

static void fzi_mdio_reset(DeviceState *dev)
{
    MdioState *s = FZI_MDIO(dev);

    s->periphident = 0x4D44494F; // MDIO IN ASCII
    s->control = 0;
    s->data_1 = 0;
    s->data_2 = 0;

    resetDummys(s);
}

static void fzi_mdio_realize(DeviceState *dev, Error **errp)
{
    MdioState *s = FZI_MDIO(dev);
    qdev_init_gpio_out(dev, s->irq, 1);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[0]);
}

static void fzi_mdio_init(Object *obj)
{
    MdioState *s = FZI_MDIO(obj);
    memory_region_init_io(&s->mmio, obj, &mdio_ops, s, TYPE_FZI_MDIO,
                          MDIO_REG_MAX);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);

    s->periphident = 0x4D44494F; // MDIO IN ASCII
    s->control = 0;
    s->data_1 = 0;
    s->data_2 = 0;

    resetDummys(s);
}

static void fzi_mdio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, fzi_mdio_reset);

    dc->realize = fzi_mdio_realize;
}

static const TypeInfo fzi_mdio_info = {
    .name = TYPE_FZI_MDIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MdioState),
    .instance_init = fzi_mdio_init,
    .class_init = fzi_mdio_class_init,
};

static void fzi_mdio_register_types(void)
{
    type_register_static(&fzi_mdio_info);
}

type_init(fzi_mdio_register_types)

    /*
     * Create MDIO device.
     */
    MdioState *mdio_create(MemoryRegion *address_space, hwaddr base,
                           qemu_irq irq)
{
    DeviceState *dev = qdev_new(TYPE_FZI_MDIO);
    MdioState *s = (MdioState *)dev;
    s->irq[0] = irq;

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    return s;
}
