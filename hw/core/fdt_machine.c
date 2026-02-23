/*
 * Builder for fdt based machines
 *
 * Copyright (c) 2009 Edgar E. Iglesias.
 * Copyright (c) 2009 Michal Simek.
 * Copyright (c) 2011 PetaLogix Qld Pty Ltd.
 * Copyright (c) 2011 Peter A. G. Crosthwaite <peter.crosthwaite@petalogix.com>.
 * Copyright (c) 2025 FZI Forschungszentrum Informatik
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

#include "chardev/char.h"
#include "exec/log.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/fdt_machine.h"
#include "hw/core/fdt_machine_register.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/cpu/cluster.h"
#include "hw/misc/unimp.h"
#include "hw/rp/remote-port-device.h"
#include "hw/rp/remote-port.h"
#include "hw/ssi/ssi.h"
#include "net/net.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qobject/qlist.h"
#include "qom/qom-qobject.h"
#include "accel/accel-cpu.h"
#include "system/address-spaces.h"
#include "system/blockdev.h"
#include "system/memory.h"
#include "system/reset.h"
#include "system/system.h"

#include <libfdt.h>
#include <stdlib.h>

static Object *fdt_create_unimplemented_qdev(FDTMachineNode *node)
{
    uint64_t addr, size;
    DeviceState *dev = NULL;
    if (fdt_machine_get_cont_range(node, &addr, &size, NULL) && addr && size) {
        g_autofree char *name = g_strconcat("dummy-", node->name, NULL);

        dev = qdev_new(TYPE_UNIMPLEMENTED_DEVICE);
        qdev_prop_set_string(dev, "name", name);
        qdev_prop_set_uint64(dev, "size", size);

        FDT_MACHINE_LOG(
            node,
            "FDT: Create dummy device for unsupported memory mapped "
            "peripheral (0x%" PRIx64 " - 0x%" PRIx64 ")\n",
            addr, addr + size - 1);
    }
    return OBJECT(dev);
}

static char *get_prop_basename(const char *str, int *index)
{
    const char *open = strchr(str, '[');
    const char *close;

    *index = -1;
    if (!open)
        return g_strdup(str);

    close = strchr(open, ']');
    if (!close || close[1] != '\0')
        return g_strdup(str);

    char *endptr;
    gint64 value = g_ascii_strtoll(open + 1, &endptr, 10);
    if (endptr != close || value < 0 || value > G_MAXINT)
        return g_strdup(str);

    *index = (int)value;
    return g_strndup(str, open - str);
}


/*
 * Used property types:
 *
 * Non-generic types - Ignore:
 * "BiosAtaTranslation", "BlockdevOnError", "EndianMode", "FloppyDriveType",
 * "GranuleMode", "IOThreadVirtQueueMappingList", "LostTickPolicy", "MigMode",
 * "MultiFDCompression", "OffAutoPCIBAR", "OnOffAuto", "PCIELinkSpeed",
 * "PCIELinkWidth", "S390CpuEntitlement", "VMAppleVirtioBlkVariant",
 * "VirtIOGPUOutputList", "ZeroPageDetection", "display_mode", "busnr"
 *
 * Array only for init - Ignore:
 * "list"
 *
 * (U)int types:
 * "int", "int32", "int64", "int8", "size", "uint16", "uint32", "uint64",
 * "uint8", "usize"
 *
 * Other generic types:
 * "bool", "str", "strList", "link"
 */
static void fdt_set_obj_props(FDTMachineNode *node, Object *obj)
{
    ObjectProperty *prop;
    ObjectPropertyIterator iter;
    object_property_iter_init(&iter, obj);

    while ((prop = object_property_iter_next(&iter))) {
        g_autofree char *fdt_propname = NULL;
        int index;

        if (!prop->set || !strcmp(prop->name, "realized")) {
            continue;
        }

        fdt_propname = get_prop_basename(prop->name, &index);
        /* all other properties have to exits to be valid */
        if (!fdt_machine_find_property(node, fdt_propname, NULL)) {
            FDT_MACHINE_LOG(node, "property '%s' (type %s) not set in fdt\n",
                            prop->name, prop->type);
            continue;
        }

        if (!strcmp(prop->type, "bool")) {
            bool val = fdt_machine_get_bool(node, fdt_propname);
            object_property_set_bool(obj, prop->name, val, &error_fatal);
            FDT_MACHINE_LOG(node, "set property '%s' (type: bool) to %s\n",
                            prop->name, val ? "true" : "false");
            continue;
        }

        if (!strncmp(prop->type, "uint", 4) || !strncmp(prop->type, "int", 3)) {
            uint64_t val =
                fdt_machine_get_uint(node, fdt_propname, &error_fatal);
            object_property_set_int(obj, prop->name, val, &error_fatal);
            FDT_MACHINE_LOG(node, "set property '%s' (type: %s) to 0x%llx\n",
                            prop->name, prop->type, (unsigned long long)val);
            continue;
        }

        if (!strcmp(prop->type, "str")) {
            const char *val =
                fdt_machine_get_string(node, fdt_propname, NULL, &error_fatal);
            object_property_set_str(obj, prop->name, val, &error_fatal);
            FDT_MACHINE_LOG(node, "set property '%s' (type: str) to %s\n",
                            prop->name, val);
            continue;
        }

        if (!strcmp(prop->type, "strList")) {
            g_autoptr(QList) list = qlist_new();
            unsigned int len;
            const char *val =
                fdt_machine_get_string(node, fdt_propname, &len, &error_fatal);
            const char *end = val + len;

            for (; val < end; val += strlen(val) + 1) {
                qlist_append_str(list, val);
            }
            object_property_set_qobject(obj, prop->name, QOBJECT(list),
                                        &error_fatal);
            FDT_MACHINE_LOG(node, "set property '%s' (type: strList)\n",
                            prop->name);
            continue;
        }

        if (!strncmp(prop->type, "link<", 5)) {
            Object *linked_dev;
            uint32_t phandle =
                fdt_machine_get_u32(node, fdt_propname, &error_fatal);
            FDTMachineNode *link = fdt_machine_get_phandle(node, phandle);
            linked_dev = link ? fdt_machine_get_object(link) : NULL;
            if (!linked_dev) {
                error_report("FDT: %s: link to unsupported node (%s)",
                             node->path, prop->name);
                continue;
            }

            object_property_set_link(obj, prop->name, linked_dev, &error_fatal);
            FDT_MACHINE_LOG(node, "set property link '%s' (type: %s)\n",
                            prop->name, prop->type);
            continue;
        }

        FDT_MACHINE_LOG(node, "Unsupported property type %s for supplied %s\n",
                        prop->type, prop->name);
    }
}

static Object *fdt_get_obj_recursive(FDTMachineNode *node)
{
    while (node) {
        Object *p = fdt_machine_get_object(node);
        if (p) {
            return p;
        }
        node = node->parent;
    }
    return NULL;
}

static bool substitute_char(char *s, char a, char b)
{
    bool changed = false;

    while ((s = strchr(s, a))) {
        *s++ = b;
        changed = true;
    }

    return changed;
}

/*
 * Remove version suffixes like:
 * - -v?\d+(.\d)+ (e.g. '-v2.1')
 * - single digits at the end (e.g. 'uart0')
 */
static bool trim_version(char *compat)
{
    static GRegex *regex = NULL;
    g_autoptr(GMatchInfo) match_info = NULL;
    bool success;
    int start_pos, end_pos;

    if (unlikely(regex == NULL)) {
        regex = g_regex_new("((-|_)v?\\d+(\\.\\d+)*)$|((?<!\\d)\\d$)",
                            G_REGEX_OPTIMIZE, 0, NULL);
    }

    if (!g_regex_match(regex, compat, 0, &match_info)) {
        return false;
    }

    success = g_match_info_fetch_pos(match_info, 0, &start_pos, &end_pos);
    assert(success);

    compat[start_pos] = '\0';
    return true;
}

static Object *fdt_create_from_compat(const char *compat)
{
    ObjectClass *class = NULL;
    const char *no_vendor = fdt_machine_trim_vendor(compat);
    bool hasVendor = (no_vendor != compat);
    g_autofree char *full_stop = NULL;
    g_autofree char *no_version = NULL;

    assert(compat);

    /* Try to create the object */
    if ((class = module_object_class_by_name(compat))) {
        goto return_obj;
    }

    /* Try it without vendor string */
    if (hasVendor && (class = module_object_class_by_name(no_vendor))) {
        goto return_obj;
    }

    /* QEMU sometimes uses full stops instead of commas */
    full_stop = g_strdup(compat);
    if (substitute_char(full_stop, ',', '.') &&
        (class = module_object_class_by_name(full_stop))) {
        goto return_obj;
    }

    /* If all fails try everthing without the version string */
    no_version = g_strdup(compat);
    if (trim_version(no_version)) {
        return fdt_create_from_compat(no_version);
    }

    return NULL;

return_obj:
    assert(class);
    return object_new_with_class(class);
}

static void fdt_set_generic_mmap(FDTMachineNode *node, FDTGenericMMap *mmap)
{
    FDTGenericMMapClass *fmc = FDT_GENERIC_MMAP_GET_CLASS(mmap);

    if (fmc->parse_reg) {
        if (!node->regs.r) {
            FDT_MACHINE_LOG(node, " has no registers to set\n");
        } else {
            fmc->parse_reg(mmap, node, &error_fatal);
        }
    }
}

static void dev_attach_nic(FDTMachineNode *node, DeviceState *dev)
{
    Object *obj = OBJECT(dev);
    g_autofree char *netdev_name = object_property_get_str(obj, "netdev", NULL);
    const char *obj_type, *type_dot, *alias;
    const char *nic_name;
    NICInfo *nd;
    const FDTMachineProperty *mac_prop;

    /* no nic or nic is already set */
    if (!netdev_name || *netdev_name != '\0') {
        return;
    }

    obj_type = object_get_typename(obj);
    type_dot = strrchr(obj_type, '.');
    alias = type_dot ? type_dot + 1 : "";
    nd = qemu_find_nic_info(obj_type, false, alias);

    if (!nd) {
        FDT_MACHINE_LOG(node, "No compatibile NIC found\n");
        return;
    }

    nic_name = nd->name ? nd->name : "unnamed";
    FDT_MACHINE_LOG(node, "set NIC %s\n", nic_name);
    qdev_set_nic_properties(dev, nd);

    /* the dt standard defines these byte arrays for mac addresses */

    /* last boot */
    mac_prop = fdt_machine_find_property(node, "mac-address", NULL);
    if (!mac_prop) {
        /* default */
        mac_prop = fdt_machine_find_property(node, "local-mac-address", NULL);
    }

    if (mac_prop && mac_prop->length == 6) {
        const uint8_t *macp = (const uint8_t *)mac_prop->value;
        char macstr[18];
        snprintf(macstr, sizeof(macstr), "%02x:%02x:%02x:%02x:%02x:%02x",
                 macp[0], macp[1], macp[2], macp[3], macp[4], macp[5]);
        object_property_set_str(obj, "mac", macstr, &error_abort);
        FDT_MACHINE_LOG(node, "set MAC to %s\n", macstr);
    }
}


static void dev_attach_unconnected_frontends(FDTMachineNode *node,
                                             DeviceState *dev)
{
    Object *obj = OBJECT(dev);

    dev_attach_nic(node, dev);

    /* Setup SSI CS line index */
    if (object_dynamic_cast(obj, TYPE_SSI_PERIPHERAL)) {
        uint32_t cs_index = 0;
        if (node->regs.size > 0) {
            cs_index = node->regs.r[0].address;
        } else {
            FDT_MACHINE_LOG(node, "WARNING: no cs found for SSI Peripheral\n");
        }
        qdev_prop_set_uint8(dev, "cs", cs_index);
    }

    if (object_property_find(obj, "chardev") &&
        !object_dynamic_cast(obj, TYPE_REMOTE_PORT)) {

        g_autofree char *label = object_property_get_str(obj, "chardev", NULL);
        if (!label || !*label) {
            static unsigned int next_serial_id = 0;
            Chardev *chr = serial_hd(next_serial_id++);
            if (chr) {
                FDT_MACHINE_LOG(node,
                                "no chardev specified, defaulting to '%s'\n",
                                chr->label);
                object_property_set_str(obj, "chardev", chr->label,
                                        &error_abort);
            } else {
                warn_report("%s: no chardev left and none specified",
                            node->path);
            }
        }
    }


    if (object_property_find(obj, "drive")) {
        warn_report("%s: drive property is not yet supported by the "
                    "generic fdt machine, device will run without a "
                    "backing drive", node->path);
    }

    if (object_dynamic_cast(obj, TYPE_REMOTE_PORT_DEVICE)) {
        int i;
        uint32_t adaptor_phandle, chan;
        FDTMachineNode *adaptor_node;
        DeviceState *adaptor;
        char *name;
        Error *errp = NULL;

        for (i = 0;; ++i) {
            adaptor_phandle = fdt_machine_get_u32_array_element(node, "remote-ports", 2 * i, &errp);
            if (errp) {
                error_free(errp);
                break;
            }
            adaptor_node = fdt_machine_get_phandle(node, adaptor_phandle);
            if (!adaptor_node) {
                error_report("%s, Missing remote port phandle %d", node->path, adaptor_phandle);
                exit(1);
            }
            adaptor = DEVICE(fdt_machine_get_object(adaptor_node));
            if (!adaptor) {
                error_report("%s, Missing remote port dev", node->path);
                exit(1);
            }

            name = g_strdup_printf("rp-adaptor%" PRId32, i);
            object_property_set_link(obj, name, OBJECT(adaptor), &error_fatal);
            FDT_MACHINE_LOG(node, "connecting RP to adaptor %s channel %d\n",
                        adaptor_node->path, i);
            g_free(name);

            chan = fdt_machine_get_u32_array_element(node, "remote-ports", 2 * i + 1, &error_fatal);

            name = g_strdup_printf("rp-chan%" PRId32, i);
            object_property_set_int(obj, name, chan, NULL);
            /* Not critical - device has right to not care about channel
             * numbers if its a pure slave (only responses).
             */
            g_free(name);

            name = g_strdup_printf("remote-port-dev%d", chan);
            object_property_set_link(OBJECT(adaptor), name, obj, &error_fatal);
            FDT_MACHINE_LOG(node, "connecting adaptor to RP %s channel %d\n",
                        adaptor_node->path, chan);
            g_free(name);
        }
    }
}

static void dev_attach_bus(FDTMachineNode *node, DeviceState *dev)
{
    DeviceClass *k = DEVICE_CLASS(object_get_class(OBJECT(dev)));
    const char *bus_type = k->bus_type;
    Object *bus;

    if (dev->parent_bus) {
        FDT_MACHINE_LOG(node, "device already attached to a sysbus\n");
        return;
    }
    
    if (!bus_type) {
        FDT_MACHINE_LOG(node, "no bus type - skipping\n");
        return;
    }

    FDT_MACHINE_LOG(node, "search for bus (type: %s)\n", bus_type);

    /* Look for an ancestor that is a bus */
    bus = node->parent ? fdt_machine_get_object(node->parent) : NULL;
    while (bus && !object_dynamic_cast(bus, bus_type)) {
        bus = bus->parent;
    }

    if (object_dynamic_cast(OBJECT(dev), TYPE_SYS_BUS_DEVICE) || !bus) {
        bus = OBJECT(sysbus_get_default());
        FDT_MACHINE_LOG(node, "defaulting to bus '%s'\n",
                        BUS(bus)->name);
    }

    if (!bus) {
        error_report("%s: Couldn't find bus for this devices", node->path);
        exit(1);
    }
    FDT_MACHINE_LOG(node, "attaching to bus '%s'\n", BUS(bus)->name);
    qdev_set_parent_bus(dev, BUS(bus), &error_abort);
}

static void dev_attach_clocks(const FDTMachineNode *node, DeviceState *dev)
{
    /* Clock handling within the device tree.  */
    Error *err = NULL;
    const uint32_t *clock_cells, *clock_end;
    const FDTMachineProperty *clocks =
        fdt_machine_find_property(node, "clocks", NULL);
    FDTMachineNode *clk_node;
    const char *consumer_clk_name;
    unsigned int cell_idx = 0;

    if (!clocks) {
        return;
    }

    if (QLIST_EMPTY(&dev->clocks)) {
        FDT_MACHINE_LOG(node, "Found clock attributes, but device has not "
                              "implemented clocks\n");
        return;
    }


    clock_cells = clocks->value;
    clock_end = clocks->value + clocks->length;

    /* Iterate over all cells of the node `clocks' property */
    for (; clock_cells < clock_end; clock_cells++) {
        uint32_t clock_phandle = fdt32_to_cpu(*clock_cells);
        uint32_t provider_cells_count;
        uint32_t provider_clk_idx;
        const char *provider_clk_name;
        Error *provider_err = NULL;
        Object *provider;
        Clock *provider_clk;
        bool continue_after_provider_resolution = false;

        /*
         * Get the provider node path, from the phandle discovered above.
         */
        clk_node = fdt_machine_get_phandle(node, clock_phandle);
        if (!clk_node) {
            FDT_MACHINE_LOG(
                node, "clock provider with phandle  %" PRIu32 " not found\n",
                clock_phandle);
            continue;
        }

        consumer_clk_name = fdt_machine_get_string_array_element(
            node, "clock-names", cell_idx, &err);

        if (err) {
            FDT_MACHINE_LOG(
                node,
                "missing clock-names cell for clocks cell  index %d - "
                "defaulting to %s\n",
                cell_idx, clk_node->name);
            error_free(err);
            err = NULL;
            consumer_clk_name = clk_node->name;
        }

        if (qdev_get_clock_in(dev, consumer_clk_name) == NULL) {
            FDT_MACHINE_LOG(node, "input clock `%s' not found on device\n",
                            consumer_clk_name);
            /*
             * Don't continue just now. Let it run after #clock-cells
             * resolution on the provider so that we know how much cells we
             * must skip on the consumer.
             */
            continue_after_provider_resolution = true;
        }


        /* Now get the number of cells required for that provider.  */
        provider_cells_count =
            fdt_machine_get_u32(clk_node, "#clock-cells", &provider_err);

        if (provider_err) {
            FDT_MACHINE_LOG(node,
                            "missing #clock-cells for clock provider %s\n ",
                            clk_node->path);
            error_free(provider_err);
            provider_err = NULL;
            continue;
        }

        if (provider_cells_count == 0) {
            provider_clk_idx = 0;
        } else if (provider_cells_count == 1) {
            cell_idx++;
            provider_clk_idx = fdt_machine_get_u32_array_element(
                node, "clocks", cell_idx, &error_fatal);
        } else {
            FDT_MACHINE_LOG(
                node, "unsupported #clock-cells value for clock provider %s\n",
                clk_node->path);
            continue;
        }

        if (continue_after_provider_resolution) {
            continue;
        }

        provider = fdt_machine_get_object(clk_node);
        if (object_dynamic_cast(provider, TYPE_CLOCK)) {
            provider_clk = CLOCK(provider);

            FDT_MACHINE_LOG(
                node, "connecting fixed-clock %s to clock input %s: %s\n ",
                clk_node->path, node->path, consumer_clk_name);
        } else if (object_dynamic_cast(provider, TYPE_DEVICE)) {
            provider_clk_name = fdt_machine_get_string_array_element(
                clk_node, "clock-output-names", provider_clk_idx,
                &provider_err);

            if (provider_err) {
                FDT_MACHINE_LOG(
                    node,
                    "clock index %" PRIu32 " not found for clock "
                    "provider %s. Its `clock-output-names' property is "
                    " either missing or too short\n ",
                    provider_clk_idx, clk_node->path);
                error_free(provider_err);
                provider_err = NULL;
                continue;
            }

            provider_clk =
                qdev_get_clock_out(DEVICE(provider), provider_clk_name);

            if (provider_clk == NULL) {
                FDT_MACHINE_LOG(node,
                                "output clock `%s' not found on device %s\n ",
                                provider_clk_name, clk_node->path);
                continue;
            }

            FDT_MACHINE_LOG(node,
                            "connecting clock output %s:%s to clock "
                            "input %s:%s\n",
                            clk_node->path, provider_clk_name, node->path,
                            consumer_clk_name);
        } else {
            FDT_MACHINE_LOG(node,
                            "clock provider %s is not a device nor a clock\n",
                            clk_node->path);
            continue;
        }

        /* perform the connection */

        FDT_MACHINE_LOG(node, "connected to clock %s\n", consumer_clk_name);
        qdev_connect_clock_in(dev, consumer_clk_name, provider_clk);
    }
}

static Object *fdt_create_generic_qdev(FDTMachineNode *node, const char *compat)
{
    Object *obj = fdt_create_from_compat(compat);

    if (!obj) {
        FDT_MACHINE_LOG(node, "no match found for compat %s\n", compat);
        return NULL;
    }
    FDT_MACHINE_LOG(node, "matched compat %s\n", compat);

    /* before fdt_set_obj_props, so explicit DT properties take precedence */
    if (node->unit_addr != (uint64_t)-1 &&
        object_property_find(obj, "unit-address")) {
        object_property_set_uint(obj, "unit-address", node->unit_addr,
                                 &error_fatal);
        FDT_MACHINE_LOG(node, "set unit-address to 0x%" PRIx64 "\n",
                        node->unit_addr);
    }

    fdt_set_obj_props(node, obj);

    /* Set the default sync-quantum based on the global one. Node properties
     * in the dtb can later override this value.  */
    if (global_sync_quantum) {
        if (object_property_find(obj, "sync-quantum")) {
            object_property_set_int(obj, "sync-quantum", global_sync_quantum,
                                    &error_abort);
        }
    }

    return obj;
}

static void fdt_machine_coroutine_start(FDTMachineNode *node, Coroutine *co)
{
    node->result.co = co;
    qemu_coroutine_enter(co);
}

static void fdt_machine_coroutine_end(FDTMachineNode *node)
{
    node->result.co = NULL;
}

static bool fdt_machine_coroutine_finished(const FDTMachineNode *node)
{
    return (node->result.co == NULL);
}

static void coroutine_fn fdt_create_node(void *arg)
{
    FDTMachineNode *node = arg;
    Object *obj = NULL;
    assert(fdt_machine_get_state(node) == FDT_NODE_UNINITIALIZED);

    for (const char **compats = node->compat; *compats != NULL; ++compats) {
        const char *compat = *compats;

        /* Try registered custom init */
        obj = fdt_register_create_compat(node, compat);
        if (obj)
            break;

        /* Try default QOM init */
        obj = fdt_create_generic_qdev(node, compat);
        if (obj)
            break;
    }

    /* create a dummy node for memory map devices */
    if (!obj) {
        obj = fdt_create_unimplemented_qdev(node);
    }

    /* the user functions can set their own state, if they didn't or it is a
     * generic object set the results */
    if (fdt_machine_get_state(node) == FDT_NODE_UNINITIALIZED) {
        fdt_machine_set_object(node, obj);
        if (object_dynamic_cast(obj, TYPE_DEVICE)) {
            DeviceState *dev = DEVICE(obj);
            dev->id = g_strdup(node->name);
        }
    }

    if (!obj) {
        /* no object was created -> skip the next steps */
        FDT_MACHINE_LOG(node, "Skipping unsupported node\n");
        fdt_machine_set_ready(node);
    } else if (!obj->parent) {
        /* default to parent object to node parent object */
        Object *obj_parent = fdt_get_obj_recursive(node->parent);
        obj_parent = obj_parent ? obj_parent : object_get_root();
        object_property_add_child(obj_parent, node->name, obj);
        object_unref(obj);
    }

    /* Attach optional external resources (devices, clocks, sysbus) if
     * unset and applicable */
    if (object_dynamic_cast(obj, TYPE_DEVICE)) {
        DeviceState *dev = DEVICE(obj);
        dev_attach_unconnected_frontends(node, dev);
        dev_attach_clocks(node, dev);
        dev_attach_bus(node, dev);
    }

    fdt_machine_coroutine_end(node);
    return;
}

static void fdt_connect_intrs(FDTMachineNode *node, Object *obj)
{
    unsigned int i, size = node->intrs.size;

    if (!size) {
        return;
    }

    if (!object_dynamic_cast(obj, TYPE_SYS_BUS_DEVICE)) {
        FDT_MACHINE_LOG(node, "has %d interrupts, but isn't a sysbus device.\n",
                        size);
        return;
    }

    SysBusDevice *dev = SYS_BUS_DEVICE(obj);

    for (i = 0; i < size; i++) {
        if (!sysbus_has_irq(dev, i)) {
            FDT_MACHINE_LOG(node, "irq #%d isn't a sysbus interrupt.\n", i);
            continue;
        }

        if (sysbus_is_irq_connected(dev, i)) {
            FDT_MACHINE_LOG(node, "irq #%d is already connected!\n", i);
            continue;
        }

        FDTMachineIntr *intr = &node->intrs.i[i];
        Object *p = fdt_get_obj_recursive(intr->intr_parent);
        if (!object_dynamic_cast(p, TYPE_FDT_GENERIC_INTC)) {
            FDT_MACHINE_LOG(
                node, "irq #%d is not connected to a generic fdt intc!\n", i);
            continue;
        }
        fdt_machine_wait_for_state(intr->intr_parent, FDT_NODE_READY);

        FDTGenericIntcClass *pc = FDT_GENERIC_INTC_GET_CLASS(p);
        assert(pc->get_irq);
        qemu_irq irq = pc->get_irq(FDT_GENERIC_INTC(p), intr, &error_fatal);

        sysbus_connect_irq(dev, i, irq);
        FDT_MACHINE_LOG(node, "connected irq #%d to %s!\n", i,
                        intr->intr_parent->name);
    }
}

static void coroutine_fn fdt_realize_node(void *arg)
{
    FDTMachineNode *node = arg;
    if (fdt_machine_get_state(node) >= FDT_NODE_REALIZED) {
        fdt_machine_coroutine_end(node);
        return;
    }

    fdt_machine_set_realized(node);

    fdt_machine_coroutine_end(node);
}

static void coroutine_fn fdt_finalize_node(void *arg)
{
    FDTMachineNode *node = arg;
    Object *obj;

    if (fdt_machine_get_state(node) >= FDT_NODE_READY) {
        fdt_machine_coroutine_end(node);
        return;
    }
    obj = fdt_machine_get_object(node);

    fdt_connect_intrs(node, obj);

    /* Register mmap based devices, e.g., sysbus devices */
    if (object_dynamic_cast(obj, TYPE_FDT_GENERIC_MMAP)) {
        fdt_set_generic_mmap(node, FDT_GENERIC_MMAP(obj));
    }

    fdt_machine_set_ready(node);
    fdt_machine_coroutine_end(node);
}

static void fdt_coroutine_foreach(CoroutineEntry func, FDTMachineNode *node)
{
    FDTMachineNode *child = node->children;
    FDTMachineNode *end = child + node->child_count;

    fdt_machine_coroutine_start(node, qemu_coroutine_create(func, node));
    for (; child < end; ++child) {
        fdt_coroutine_foreach(func, child);
    }
}

static void fdt_init_check_queues(FDTMachineNode *node)
{
    FDTMachineNode *child = node->children;
    FDTMachineNode *end = child + node->child_count;

    if (!fdt_machine_coroutine_finished(node)) {
        error_report("FDT: %s did not finish step!", node->path);
        exit(1);
    }

    for (; child < end; ++child) {
        fdt_init_check_queues(child);
    }
}

static void fdt_init_step(CoroutineEntry func, FDTMachineNode *node)
{
    fdt_coroutine_foreach(func, node);
    fdt_init_check_queues(node);
}

static unsigned int fdt_count_cores(FDTMachineNode *node)
{
    FDTMachineNode *child = node->children;
    FDTMachineNode *end = child + node->child_count;
    unsigned int c =
        (object_dynamic_cast(fdt_machine_get_object(node), TYPE_CPU) == NULL) ?
            0 :
            1;

    for (; child < end; ++child) {
        c += fdt_count_cores(child);
    }

    return c;
}

FDTMachineInfo *fdt_machine_create(void *fdt)
{
    FDTMachineInfo *fdti = fdt_machine_parse(fdt);
    FDTMachineNode *root = fdti ? fdti->root : NULL;

    if (!root) {
        fprintf(stderr, "FDT: ERROR: cannot get root node from device tree\n");
        return NULL;
    }

    memory_region_transaction_begin();
    qemu_log_mask(LOG_FDT, "FDT: Create node objects\n");
    fdt_init_step(fdt_create_node, root);
    qemu_log_mask(LOG_FDT, "FDT: Realize node objects\n");
    fdt_init_step(fdt_realize_node, root);
    qemu_log_mask(LOG_FDT, "FDT: Finialize node objects\n");
    fdt_init_step(fdt_finalize_node, root);
    memory_region_transaction_commit();

    unsigned int cores = fdt_count_cores(root);
    current_machine->smp.cores = cores;
    current_machine->smp.cpus = cores;
    current_machine->smp.max_cpus = cores;

    bdrv_drain_all();
    qemu_log_mask(LOG_FDT, "FDT: Device tree scan complete\n");
    return fdti;
}

static const TypeInfo fdt_generic_intc_info = {
    .name = TYPE_FDT_GENERIC_INTC,
    .parent = TYPE_INTERFACE,
    .class_size = sizeof(FDTGenericIntcClass),
};

static const TypeInfo fdt_generic_mmap_info = {
    .name = TYPE_FDT_GENERIC_MMAP,
    .parent = TYPE_INTERFACE,
    .class_size = sizeof(FDTGenericMMapClass),
};

static void fdt_generic_intc_register_types(void)
{
    type_register_static(&fdt_generic_intc_info);
    type_register_static(&fdt_generic_mmap_info);
}

type_init(fdt_generic_intc_register_types)
