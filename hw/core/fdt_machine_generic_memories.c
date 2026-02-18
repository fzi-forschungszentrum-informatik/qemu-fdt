#include "qemu/osdep.h"

#include "hw/core/clock.h"
#include "hw/core/fdt_machine_register.h"
#include "hw/core/qdev.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "system/address-spaces.h"
#include "system/memory.h"

/* MemoryRegions do not support the dynamic setting of many properties.
 * We build them here instead. */
typedef bool (*MemoryRegionInitFn)(MemoryRegion *mr, Object *owner,
                                   const char *name, uint64_t size,
                                   Error **errp);

static Object *memory_region_node_init(FDTMachineNode *node,
                                       MemoryRegionInitFn init_fn,
                                       const char *type_str)
{
    MemoryRegion *first = NULL;
    MemoryRegion *mem_parent = MEMORY_REGION(object_dynamic_cast(
        fdt_machine_get_object(node->parent), TYPE_MEMORY_REGION));
    unsigned int i;

    if (!mem_parent) {
        FDT_MACHINE_LOG(node, "no parent bus - connecting to root\n");
        mem_parent = get_system_memory();
    }

    if (!node->regs.size) {
        error_report("%s: %s without reg", node->path, type_str);
        exit(1);
    }

    for (i = 0; i < node->regs.size; i++) {
        const FDTMachineReg *reg = &node->regs.r[i];
        MemoryRegion *mem = g_new(MemoryRegion, 1);
        g_autofree char *name =
            i ? g_strdup_printf("%s-%u", node->name, i) : g_strdup(node->name);

        init_fn(mem, NULL, name, reg->size, &error_fatal);
        memory_region_add_subregion(mem_parent, reg->address, mem);

        FDT_MACHINE_LOG(node,
                        "created %s %s [0x%" PRIx64 " - 0x%" PRIx64
                        "] - parent '%s'\n",
                        type_str, mem->name, reg->address,
                        reg->address + reg->size - 1, mem_parent->name);
        if (!first) {
            first = mem;
        }
    }

    return OBJECT(first);
}

static Object *ram_init(FDTMachineNode *node, void *opaque)
{
    return memory_region_node_init(node, memory_region_init_ram, "ram");
}

static Object *rom_init(FDTMachineNode *node, void *opaque)
{
    return memory_region_node_init(node, memory_region_init_rom, "rom");
}

fdt_register_compatibility(ram_init, "memory");
fdt_register_compatibility(rom_init, "rom");

static void get_simple_bus_range(const FDTMachineNode *node, uint64_t *min,
                                 uint64_t *max)
{
    FDTMachineNode *child = node->children;
    FDTMachineNode *end_child = child + node->child_count;

    for (; child < end_child; ++child) {
        uint64_t start, size;
        if (fdt_machine_get_cont_range(child, &start, &size, NULL) && size) {
            uint64_t end = start + size - 1;
            *min = MIN(start, *min);
            *max = MAX(end, *max);
        }

        /* If the child is bus, go deeper */
        for (const char **compats = child->compat; *compats != NULL; ++compats) {
            const char *compat = *compats;
            if (!strcmp(compat, "simple-bus")) {
                get_simple_bus_range(child, min, max);
                break;
            }
        }
    }
}

/* For simple buses we just put them into a common memory region */
static Object *simple_bus_init(FDTMachineNode *node, void *opaque)
{
    MemoryRegion *mr = g_new(MemoryRegion, 1);
    MemoryRegion *mr_parent =
        node->parent ?
            MEMORY_REGION(object_dynamic_cast(
                fdt_machine_get_object(node->parent), TYPE_MEMORY_REGION)) :
            NULL;

    if (!mr_parent) {
        FDT_MACHINE_LOG(node, "no parent bus - connecting to root\n");
        mr_parent = get_system_memory();
    }

    if (node->ranges.size) {
        if (node->ranges.size != 1) {
            error_report("%s: Currently only max range is supported %d)",
                         node->path, node->ranges.size);
            exit(1);
        }

        FDTMachineRange *range = &node->ranges.r[0];

        MemoryRegion *mr_src = g_new(MemoryRegion, 1);
        g_autofree char *name = g_strconcat(node->name, "-src", NULL);
        memory_region_init(mr_src, NULL, name, range->child_addr + range->size);

        memory_region_init_alias(mr, NULL, node->name, mr_src,
                                 range->child_addr, range->size);
        memory_region_add_subregion(mr_parent, range->parent_addr, mr);

        /* children map into mr_src, subregions of an alias are not rendered */
        FDT_MACHINE_LOG(node, "create simple bus\n");
        return OBJECT(mr_src);
    } else {
        uint64_t start = UINT64_MAX, end = 0;
        get_simple_bus_range(node, &start, &end);

        /* no mapped children: empty container */
        memory_region_init(mr, NULL, node->name, start > end ? 0 : end + 0x1);
        memory_region_add_subregion(mr_parent, 0x0, mr);
    }

    FDT_MACHINE_LOG(node, "create simple bus\n");
    return OBJECT(mr);
}

fdt_register_compatibility(simple_bus_init, "simple-bus");
