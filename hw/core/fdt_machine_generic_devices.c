#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "system/address-spaces.h"

#include "hw/core/clock.h"
#include "hw/core/fdt_machine_register.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev.h"
#include "hw/cpu/cluster.h"
#include "system/memory.h"

/* Clocks are objects, but not devices, so they don't support properties. We
 * build them here instead. */
static Object *fixed_clock_init(FDTMachineNode *node, void *opaque)
{
    uint32_t freq;
    Error *err = NULL;
    Clock *clk;
    Object *parent;

    freq = fdt_machine_get_uint(node, "clock-frequency", &err);
    if (err) {
        FDT_MACHINE_LOG(
            node, "missing `clock-frequency' property, defaulting to 0\n");
        freq = 0;
    }

    parent = fdt_machine_get_object(node->parent);
    if (parent == NULL) {
        parent = qdev_get_machine();
    }

    clk = clock_new(parent, node->name);
    clock_set_hz(clk, freq);

    FDT_MACHINE_LOG(node, "created fixed-clock with %" PRIu32 " Hz frequency\n",
                    freq);

    return OBJECT(clk);
}

static uint32_t get_new_cluster_id(void)
{
    static uint32_t id;
    return id++;
}

static Object *cpu_cluster_init(FDTMachineNode *node, void *opaque)
{
    Object *cl;
    uint32_t id = get_new_cluster_id();
    Object *parent = fdt_machine_get_object(node->parent);
    if (parent == NULL) {
        parent = qdev_get_machine();
    }

    cl = object_new(TYPE_CPU_CLUSTER);
    object_property_add_child(parent, node->name, cl);
    object_unref(cl);
    qdev_prop_set_uint32(DEVICE(cl), "cluster-id", id);

    FDT_MACHINE_LOG(node, "created cluster with id %" PRIu32 "\n", id);

    return cl;
}

fdt_register_compatibility(fixed_clock_init, "fixed-clock");
fdt_register_compatibility(cpu_cluster_init, "cpus");
