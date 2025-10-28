/*
 * FZI simulation test-exit device emulation
 *
 * Copyright (c) 2025 FZI Forschungszentrum Informatik
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/core/sysbus.h"
#include "hw/misc/sim_test.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/runstate.h"

static uint64_t sim_test_read(void *opaque, hwaddr addr, unsigned int size)
{
    SimTestState *s = SIM_TEST(opaque);

    if (addr == 0) {
        return s->magicReg;
    } else if (addr == 8) {
        return s->exitCodeReg;
    }

    qemu_log_mask(LOG_GUEST_ERROR, "%s: read: addr=0x%x (%u)\n", __func__,
                  (int)addr, size);
    return 0;
}

static void sim_test_write(void *opaque, hwaddr addr, uint64_t val64,
                           unsigned int size)
{
    SimTestState *s = SIM_TEST(opaque);
    uint16_t val16 = val64;

    if (addr == 0) {
        s->magicReg = val16;
        if (s->magicReg == SIM_TEST_STOP) {
            if (s->exitCodeReg)
                qemu_log("QEMU TEST FAILED: 0x%04x (U16 EXIT CODE)\n",
                         s->exitCodeReg);
            else
                qemu_log("QEMU TEST SUCCESS\n");

            uint8_t unixExitCode = s->exitCodeReg;
            if (s->exitCodeReg && !unixExitCode) {
                unixExitCode = s->exitCodeReg >> 8;
            }
            qemu_system_shutdown_request_with_code(
                SHUTDOWN_CAUSE_GUEST_SHUTDOWN, unixExitCode);
        }
        return;

    } else if (addr == 8) {
        s->exitCodeReg = val16;
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: write: addr=0x%x val=0x%016" PRIx64 " (%u)\n", __func__,
                  (int)addr, val64, size);
}

static const MemoryRegionOps sim_test_ops = {
    .read = sim_test_read,
    .write = sim_test_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 2, .max_access_size = 8 }
};

static void sim_test_init(Object *obj)
{
    SimTestState *s = SIM_TEST(obj);
    memory_region_init_io(&s->mmio, obj, &sim_test_ops, s, TYPE_SIM_TEST,
                          0x4000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);

    s->magicReg = 0;
    s->exitCodeReg = 0xffffU;
}

static const TypeInfo sim_test_info = {
    .name = TYPE_SIM_TEST,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SimTestState),
    .instance_init = sim_test_init,
};

static void sim_test_register_types(void)
{
    type_register_static(&sim_test_info);
}

DeviceState *sim_test_create(hwaddr addr)
{
    DeviceState *dev = qdev_new(TYPE_SIM_TEST);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, addr);
    return dev;
}

type_init(sim_test_register_types);
