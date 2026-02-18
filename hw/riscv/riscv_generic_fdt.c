/*
 * Small Device-tree driven RISC-V machine creator.
 *
 * Copyright (c) 2022 Advanced Micro Devices.
 * Written by Edgar E. Iglesias.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"

#include "cpu.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/fdt_machine.h"
#include "hw/core/generic-loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/riscv/boot.h"
#include "hw/riscv/machines-qom.h"
#include "qapi/error.h"
#include "system/device_tree.h"
#include "system/reset.h"
#include "qemu/error-report.h"

static void riscv_fdt_load_file(const char *file)
{
    DeviceState *loader = qdev_new(TYPE_GENERIC_LOADER);

    qdev_prop_set_string(loader, "file", file);
    qdev_realize_and_unref(loader, NULL, &error_fatal);
}

static void riscv_fdt_init(MachineState *machine)
{
    FDTMachineInfo *fdti;
    void *fdt = NULL;
    int fdt_size;

    if (!machine->dtb && !machine->hw_dtb) {
        error_report("No hw-dtb found");
        exit(1);
    }

    if (!machine->hw_dtb) {
        machine->hw_dtb = machine->dtb;
    }

    fdt = load_device_tree(machine->hw_dtb, &fdt_size);
    if (!fdt) {
        error_report("Error: Unable to load Hardware Device Tree %s",
                      machine->hw_dtb);
        exit(1);
    }

    /* Instantiate peripherals from the FDT. */
    fdti = fdt_machine_create(fdt);
    if (!fdti) {
        exit(1);
    }
    fdt_machine_free(fdti);

    if (machine->firmware &&
        strcmp(machine->firmware, "default") &&
        strcmp(machine->firmware, "none")) {
        riscv_fdt_load_file(machine->firmware);
    }

    if (machine->kernel_filename) {
        riscv_fdt_load_file(machine->kernel_filename);
    }

    return;
}

static void riscv_fdt_machine_reset(MachineState *machine, ResetType type)
{
    CPUState *cpu;

    qemu_devices_reset(type);
    CPU_FOREACH(cpu) {
        cpu_reset(cpu);
    }
}

static void riscv_fdt_machine_init(MachineClass *mc)
{
    mc->desc = "RISC-V flat device tree driven machine model";
    mc->init = riscv_fdt_init;
    mc->reset = riscv_fdt_machine_reset;
    mc->min_cpus = 1;
    mc->max_cpus = 64;
    mc->default_cpu_type = TYPE_RISCV_CPU_BASE;
    mc->default_cpus = 64;
}

DEFINE_MACHINE_RISCV32_64("riscv-fdt", riscv_fdt_machine_init)

static void fdt_generic_cpu_init(Object *obj)
{
    object_property_add_alias(obj, "unit-address", obj, "mhartid");
}

static const TypeInfo fdt_qom_aliases[] = {
        { .name = TYPE_FDT_GENERIC_CPU_TYPE, .parent = TYPE_RISCV_CPU_BASE,
          .instance_init = fdt_generic_cpu_init },
};

static void fdt_generic_register_types(void)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(fdt_qom_aliases); ++i) {
        type_register_static(&fdt_qom_aliases[i]);
    }
}

type_init(fdt_generic_register_types)
