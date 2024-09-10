/*
 * FZI hardware identification device emulation
 *
 * Copyright (c) 2024 FZI Forschungszentrum Informatik
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>

#include "qemu/osdep.h"
#include "hw/core/cpu.h"

#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/misc/fzi_hwident.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/queue.h"


#define FZI_HWIDENT(obj) OBJECT_CHECK(fziHwIdentState, (obj), TYPE_FZI_HWIDENT)


static uint64_t hwident_read(void *opaque, hwaddr addr, unsigned int size)
{
    fziHwIdentState *s = opaque;

    uint64_t retVal = UINT64_MAX;

    // qemu_log("FZI_HWIDENT ACCESS. ADDR: 0x%" PRIx64 " SIZE: %d\n", addr,
    // size);
    hwaddr access = addr & ~0x3;
    hwaddr shift = addr & 0x3;

    switch (access) {
    case FZIHWIDENT_MAGIC_OFFSET:
        retVal = htobe32(fzihwident_magic);
        break;
    case FZIHWIDENT_BOARDID_OFFSET:
        if (s->boardId) {
            uint32_t id = 0;
            size_t len = MIN(strlen(s->boardId), sizeof(id));
            memcpy(&id, s->boardId, len);
            retVal = id;
        }
        break;
    case FZIHWIDENT_SHA_OFFSET:
        if (s->dirtyBuild) {
            retVal = UINT32_MAX;
        } else {
            retVal = htobe32(s->shortSha);
        }
        break;
    case FZIHWIDENT_TIMESTAMP_OFFSET_L:
        retVal = (htobe64(s->timestamp) & UINT32_MAX);
        break;
    case FZIHWIDENT_TIMESTAMP_OFFSET_H:
        retVal = (htobe64(s->timestamp) >> 32);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "fzi-hwident: invalid read addr=0x%" PRIx64 " size=%d\n",
                      addr, size);
    }

    retVal = (retVal >> (shift * 8)) & ((1UL << (size * 8)) - 1);
    return retVal;
}

static void hwident_write(void *opaque, hwaddr addr, uint64_t val64,
                          unsigned int size)
{
    (void)opaque;
    (void)addr;
    (void)val64;
    return;
}

static const MemoryRegionOps hwident_ops = {
    .read = hwident_read,
    .write = hwident_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = false }
};

static void fzi_hwident_reset(DeviceState *dev)
{
    (void)dev;
    return;
}

static void fzi_hwident_realize(DeviceState *dev, Error **errp)
{
    (void)dev;
    (void)errp;
    return;
}

static void fzi_hwident_init(Object *obj)
{
    fziHwIdentState *s = FZI_HWIDENT(obj);
    memory_region_init_io(&s->mmio, obj, &hwident_ops, s, TYPE_FZI_HWIDENT,
                          4096);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const Property fzi_hwident_properties[] = {
    DEFINE_PROP_STRING("board-ident", fziHwIdentState, boardId),
    DEFINE_PROP_UINT32("build-short-sha", fziHwIdentState, shortSha, 0),
    DEFINE_PROP_UINT64("timestamp", fziHwIdentState, timestamp, 0),
    DEFINE_PROP_BOOL("dirty-build", fziHwIdentState, dirtyBuild, false)
};

static void fzi_hwident_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, fzi_hwident_reset);

    dc->realize = fzi_hwident_realize;
    device_class_set_props(dc, fzi_hwident_properties);
}

static const TypeInfo fzi_hwident_info = {
    .name = TYPE_FZI_HWIDENT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(fziHwIdentState),
    .instance_init = fzi_hwident_init,
    .class_init = fzi_hwident_class_init,
};

static void fzi_hwident_register_types(void)
{
    type_register_static(&fzi_hwident_info);
}

type_init(fzi_hwident_register_types);

/*
 * Create HwIdent device.
 */
fziHwIdentState *fziHwIdent_create(MemoryRegion *address_space, hwaddr base)
{
    DeviceState *dev = qdev_new(TYPE_FZI_HWIDENT);
    fziHwIdentState *s = (fziHwIdentState *)dev;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    return s;
}
