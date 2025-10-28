/*
 * FZI simulation test-exit device emulation
 *
 * Copyright (c) 2025 FZI Forschungszentrum Informatik
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SIM_TEST_H
#define HW_SIM_TEST_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_SIM_TEST "fzi.simtestregs"

typedef struct SimTestState SimTestState;
DECLARE_INSTANCE_CHECKER(SimTestState, SIM_TEST, TYPE_SIM_TEST)

struct SimTestState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;

    uint16_t magicReg;
    uint16_t exitCodeReg;
};

enum { SIM_TEST_ACTIVE = 0xcafe, SIM_TEST_STOP = 0xdead };

DeviceState *sim_test_create(hwaddr addr);

#endif
