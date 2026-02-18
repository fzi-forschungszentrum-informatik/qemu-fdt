#include "qemu/osdep.h"

#include "hw/core/fdt_machine.h"
#include "hw/core/fdt_machine_info.h"
#include "hw/core/qdev.h"
#include "qemu/error-report.h"
#include "qom/object.h"

#include <assert.h>
#include <glib.h>
#include <libfdt.h>
#include <string.h>

static void parse_name(FDTMachineNode *node);
static void parse_properties(const void *fdt, FDTMachineNode *node);
static void parse_regs(FDTMachineNode *node);
static void parse_ranges(FDTMachineNode *node);
static void parse_compat(const void *fdt, FDTMachineNode *node);

static void parse_node(FDTMachineInfo *dt, FDTMachineNode *parent,
                       FDTMachineNode *node, int offset)
{
    const void *fdt = dt->fdt;
    int child_offset, name_len = 0;

    if (parent == NULL) {
        node->name = "$fdt_root";
    } else {
        node->name = fdt_get_name(fdt, offset, &name_len);
        if (!node->name) {
            error_report("fdt: cannot get name for node at offset %d: %s",
                        offset, fdt_strerror(name_len));
            exit(1);
        }
    }
    node->fdt_machine_offset = offset;
    node->phandle = fdt_get_phandle(fdt, offset);
    node->phandles = &dt->phandles;
    node->parent = parent;
    node->children = NULL;
    node->props = NULL;
    node->result.state = FDT_NODE_UNINITIALIZED;
    qemu_co_queue_init(&node->result.waitq);

    parse_name(node);
    parse_properties(fdt, node);
    parse_regs(node);
    parse_ranges(node);
    parse_compat(fdt, node);

    node->child_count = 0;
    fdt_for_each_subnode(child_offset, fdt, offset)
    {
        node->child_count++;
    }

    if (node->child_count > 0) {
        node->children = g_malloc0(sizeof(FDTMachineNode) * node->child_count);
        unsigned int i = 0;
        fdt_for_each_subnode(child_offset, fdt, offset)
        {
            parse_node(dt, node, &node->children[i++], child_offset);
        }
    }
}

static void parse_intrs(FDTMachineNode *node);

static void fdt_machine_late_parse_node(FDTMachineNode *node)
{
    unsigned int i;
    parse_intrs(node);

    for (i = 0; i < node->child_count; ++i) {
        fdt_machine_late_parse_node(&node->children[i]);
    }
}

static uint32_t fdt_machine_get_max_phandle(FDTMachineNode *node)
{
    uint32_t max = node->phandle;
    unsigned int i;

    for (i = 0; i < node->child_count; ++i) {
        uint32_t child = fdt_machine_get_max_phandle(&node->children[i]);

        max = (child > max) ? child : max;
    }
    return max;
}

static void fdt_machine_set_phandle(FDTMachineHandles *handles,
                                    FDTMachineNode *node)
{
    unsigned int i;

    if (node->phandle > 0) {
        assert(node->phandle < handles->size);
        assert(handles->nodes[node->phandle] == NULL);
        handles->nodes[node->phandle] = node;
    }

    for (i = 0; i < node->child_count; ++i) {
        fdt_machine_set_phandle(handles, &node->children[i]);
    }
}

FDTMachineInfo *fdt_machine_parse(const void *fdt)
{
    FDTMachineInfo *dt;

    if (!fdt) {
        return NULL;
    }

    if (fdt_check_header(fdt) != 0) {
        return NULL;
    }

    dt = g_malloc0(sizeof(FDTMachineInfo));

    dt->fdt = fdt;
    dt->root = g_malloc0(sizeof(FDTMachineNode));
    parse_node(dt, NULL, dt->root, 0);

    dt->phandles.size = fdt_machine_get_max_phandle(dt->root) + 1;
    dt->phandles.nodes =
        g_malloc0(sizeof(FDTMachineNode *) * dt->phandles.size);
    fdt_machine_set_phandle(&dt->phandles, dt->root);

    fdt_machine_late_parse_node(dt->root);

    return dt;
}

static void fdt_machine_free_node(FDTMachineNode *node)
{
    unsigned int i;

    for (i = 0; i < node->child_count; i++) {
        fdt_machine_free_node(&node->children[i]);
    }

    object_unref(node->result.obj);
    g_free(node->props);
    g_free(node->children);
    g_free(node->regs.r);
    g_free(node->ranges.r);
    g_free(node->intrs.i);
    g_free(node->short_name);
    g_free(node->path);
    g_free(node->compat);
}

void fdt_machine_free(FDTMachineInfo *dt)
{
    if (!dt) {
        return;
    }

    fdt_machine_free_node(dt->root);
    g_free(dt->root);
    g_free(dt->phandles.nodes);
    g_free(dt);
}

const FDTMachineProperty *fdt_machine_find_property(const FDTMachineNode *node,
                                                    const char *name,
                                                    Error **errp)
{
    unsigned int i;

    if (!node || !name) {
        error_setg(errp, "%s: Invalid request", __func__);
        return NULL;
    }

    for (i = 0; i < node->prop_count; i++) {
        if (!strcmp(node->props[i].name, name)) {
            return &node->props[i];
        }

        if (!strcmp(fdt_machine_trim_vendor(node->props[i].name), name)) {
            return &node->props[i];
        }
    }

    error_setg(errp, "%s: Node %s doesn't contain %s", __func__, node->path,
               name);
    return NULL;
}

static void parse_name(FDTMachineNode *node)
{
    const char *at = strchr(node->name, '@');

    if (at) {
        size_t short_name_len = at - node->name;
        node->short_name = g_strndup(node->name, short_name_len);
        char *name_end;
        node->unit_addr = g_ascii_strtoull(at + 1, &name_end, 16);

        if (*name_end != '\0') {
            error_report("%s: Invalid unit address", node->name);
            exit(1);
        }
    } else {
        node->short_name = g_strdup(node->name);
        node->unit_addr = -1;
    }

    if (!node->parent) {
        node->path = g_strdup("/");
    } else {
        if (strcmp(node->parent->path, "/") == 0) {
            node->path = g_strdup_printf("/%s", node->name);
        } else {
            node->path =
                g_strdup_printf("%s/%s", node->parent->path, node->name);
        }
    }
}

static void parse_properties(const void *fdt, FDTMachineNode *node)
{
    int prop_offset;
    unsigned int i = 0;

    node->prop_count = 0;
    node->props = NULL;

    fdt_for_each_property_offset(prop_offset, fdt, node->fdt_machine_offset)
    {
        node->prop_count++;
    }

    if (node->prop_count == 0) {
        return;
    }

    node->props = g_malloc0(sizeof(FDTMachineProperty) * node->prop_count);

    fdt_for_each_property_offset(prop_offset, fdt, node->fdt_machine_offset)
    {
        int len;
        const struct fdt_property *prop =
            fdt_get_property_by_offset(fdt, prop_offset, &len);

        node->props[i].fdt_machine_offset = prop_offset;
        node->props[i].name = fdt_string(fdt, fdt32_to_cpu(prop->nameoff));
        node->props[i].value = prop->data;
        node->props[i].length = (uint32_t)fdt32_to_cpu(prop->len);
        i++;
    }
}

static void parse_regs(FDTMachineNode *node)
{
    const FDTMachineProperty *reg_prop =
        fdt_machine_find_property(node, "reg", NULL);
    unsigned int address_cells = 1;
    unsigned int size_cells = 1;
    unsigned int reg_count;
    const fdt32_t *val;
    unsigned int i;

    if (!reg_prop) {
        return;
    }

    if (node->parent) {
        address_cells =
            fdt_machine_get_u32_default(node->parent, "#address-cells", 1);
        size_cells =
            fdt_machine_get_u32_default(node->parent, "#size-cells", 1);
    }

    if ((address_cells + size_cells == 0) || address_cells > 2 ||
        size_cells > 2) {
        error_report(
            "%s: Invalid reg sizes (#address-cells %d, #size-cells %d)",
            node->path, address_cells, size_cells);
        exit(1);
    }

    if (reg_prop->length % (4 * (address_cells + size_cells)) != 0) {
        error_report("%s: Invalid reg length (length %d, #address-cells %d, "
                     "#size-cells %d)",
                     node->path, reg_prop->length, address_cells, size_cells);
        exit(1);
    }

    reg_count =
        reg_prop->length / sizeof(fdt32_t) / (address_cells + size_cells);

    if (!reg_count) {
        return;
    }

    node->regs.size = reg_count;
    node->regs.r = g_new0(FDTMachineReg, reg_count);

    val = reg_prop->value;
    for (i = 0; i < reg_count; ++i) {
        unsigned int j;
        FDTMachineReg *reg = &node->regs.r[i];

        for (j = 0; j < address_cells; ++j) {
            reg->address <<= 32;
            reg->address |= fdt32_to_cpu(*val++);
        }

        for (j = 0; j < size_cells; ++j) {
            reg->size <<= 32;
            reg->size |= fdt32_to_cpu(*val++);
        }
    }
}


static void parse_ranges(FDTMachineNode *node)
{
    const FDTMachineProperty *ranges_prop =
        fdt_machine_find_property(node, "ranges", NULL);
    unsigned int parent_addr_cells = 1;
    unsigned int child_addr_cells;
    unsigned int size_cells;
    unsigned int range_size;
    unsigned int range_count;
    const fdt32_t *val;
    unsigned int i;

    if (!ranges_prop) {
        return;
    }

    if (node->parent) {
        parent_addr_cells =
            fdt_machine_get_u32_default(node->parent, "#address-cells", 1);
    }

    child_addr_cells = fdt_machine_get_u32_default(node, "#address-cells", 1);
    size_cells = fdt_machine_get_u32_default(node, "#size-cells", 1);

    range_size = parent_addr_cells + child_addr_cells + size_cells;

    if (parent_addr_cells == 0 || parent_addr_cells > 2 ||
        child_addr_cells == 0 || child_addr_cells > 2 || size_cells == 0 ||
        size_cells > 2) {
        error_report(
            "%s: Invalid range sizes (#address-cells %d %d, #size-cells %d)",
            node->path, parent_addr_cells, child_addr_cells, size_cells);
        exit(1);
    }

    if (ranges_prop->length % (4 * range_size) != 0) {
        error_report("%s: Invalid reg length (length %d, #address-cells %d %d, "
                     "#size-cells %d)",
                     node->path, ranges_prop->length, parent_addr_cells,
                     child_addr_cells, size_cells);
        exit(1);
    }

    range_count = ranges_prop->length / sizeof(fdt32_t) / (range_size);
    if (!range_count) {
        return;
    }

    node->ranges.size = range_count;
    node->ranges.r = g_new0(FDTMachineRange, range_count);

    val = ranges_prop->value;
    for (i = 0; i < range_count; ++i) {
        unsigned int j;
        FDTMachineRange *range = &node->ranges.r[i];

        for (j = 0; j < parent_addr_cells; ++j) {
            range->parent_addr <<= 32;
            range->parent_addr |= fdt32_to_cpu(*val++);
        }

        for (j = 0; j < parent_addr_cells; ++j) {
            range->child_addr <<= 32;
            range->child_addr |= fdt32_to_cpu(*val++);
        }

        for (j = 0; j < size_cells; ++j) {
            range->size <<= 32;
            range->size |= fdt32_to_cpu(*val++);
        }
    }
}

static void parse_intrs(FDTMachineNode *node)
{
    Error *err = NULL;
    const FDTMachineProperty *intrs =
        fdt_machine_find_property(node, "interrupts-extended", NULL);
    uint32_t parent_phandle;
    FDTMachineNode *parent;
    FDTMachineIntr *intr;
    const uint32_t *cellp, *cellp_end;
    uint32_t cell_size;
    unsigned int i, cell_index;

    if (intrs) {
        if (intrs->length % 4 != 0) {
            error_report("%s: invalid interrupts-extended length %d",
                         node->path, intrs->length);
            exit(1);
        }

        cellp = intrs->value;
        cellp_end = intrs->value + intrs->length;
        node->intrs.size = 0;

        while (cellp < cellp_end) {
            parent_phandle = fdt32_to_cpu(*cellp++);
            parent = fdt_machine_get_phandle(node, parent_phandle);
            if (!parent) {
                error_report("%s: invalid interrupts parent %d", node->path,
                             parent_phandle);
                exit(1);
            }

            cell_size =
                fdt_machine_get_u32_default(parent, "#interrupt-cells", 1);
            for (i = 0; i < cell_size; ++i) {
                cellp++;
            }
            if (cell_size > FDT_MAX_NCELLS) {
                error_report("%s: #interrupt-cells %d bigger than max value %d",
                             node->path, cell_size, FDT_MAX_NCELLS);
                exit(1);
            }
            node->intrs.size++;
        }

        if (node->intrs.size > 0) {
            cellp = intrs->value;
            node->intrs.i =
                g_malloc0(sizeof(FDTMachineIntr) * node->intrs.size);
            i = 0;
            while (cellp < cellp_end) {
                parent_phandle = fdt32_to_cpu(*cellp++);
                parent = fdt_machine_get_phandle(node, parent_phandle);
                assert(parent);
                cell_size =
                    fdt_machine_get_u32_default(parent, "#interrupt-cells", 1);

                intr = &node->intrs.i[i++];
                intr->intr_parent = parent;
                intr->size = cell_size;
                for (i = 0; i < cell_size; ++i) {
                    assert(cellp < cellp_end);
                    intr->cells[i] = fdt32_to_cpu(*cellp++);
                }
            }
        }
        return;
    }

    intrs = fdt_machine_find_property(node, "interrupts", NULL);
    if (!intrs) {
        return;
    }

    parent_phandle = fdt_machine_get_u32(node, "interrupt-parent", &err);
    if (err) {
        /* Spec: default to node parent */
        error_free(err);
        parent = node->parent;
    } else {
        parent = fdt_machine_get_phandle(node, parent_phandle);
    }
    assert(parent);

    cellp = intrs->value;
    cellp_end = intrs->value + intrs->length;
    cell_size = fdt_machine_get_u32_default(parent, "#interrupt-cells", 1);

    if (intrs->length % (cell_size * sizeof(uint32_t)) != 0) {
        error_report("%s: invalid interrupt length %d", node->path,
                     intrs->length);
        exit(1);
    }

    node->intrs.size = intrs->length / (cell_size * sizeof(uint32_t));
    cell_index = 0;
    if (node->intrs.size > 0) {
        node->intrs.i = g_malloc0(sizeof(FDTMachineIntr) * node->intrs.size);
        while (cellp < cellp_end) {
            intr = &node->intrs.i[cell_index++];
            intr->intr_parent = parent;
            intr->size = cell_size;
            for (i = 0; i < cell_size; ++i) {
                assert(cellp < cellp_end);
                intr->cells[i] = fdt32_to_cpu(*cellp++);
            }
        }
    }
    assert(cell_index == node->intrs.size);
}

static void parse_compat(const void *fdt, FDTMachineNode *node)
{
    unsigned int compat_len = 0;
    unsigned int count = 0;
    unsigned int i;
    int compat_count = 0;
    const char *compat =
        fdt_machine_get_string(node, "compatible", &compat_len, NULL);
    const char *extra_compat = NULL;

    if (compat) {
        compat_count =
            fdt_stringlist_count(fdt, node->fdt_machine_offset, "compatible");
        assert(compat_count >= 0);
    }

    /* Some device type infos are not in the compatible, append them to the end
     * of the compat list. */
    if (!strcmp(node->short_name, "cpu") ||
        !strcmp(node->short_name, "memory")) {
        extra_compat = fdt_machine_get_string(node, "device_type", NULL, NULL);
    } else if (!node->parent) {
        /* Special case for the root node - if we don't have a useable compat
         * string we default to a generic bus */
        extra_compat = "simple-bus";
    }

    count = compat_count + (extra_compat ? 1 : 0) + 1;

    if (!count) {
        return;
    }

    node->compat = g_new0(const char *, count + 1);

    if (compat) {
        const char *q = compat;
        for (i = 0; i < compat_count; i++) {
            node->compat[i] = q;
            q += strlen(q) + 1;
        }
    }

    /* add device_type at the end of compat */
    if (extra_compat) {
        node->compat[count - 2] = extra_compat;
    }

    /* last compat fallback node name, e.g. for cpus */
    node->compat[count - 1] = node->short_name;

    for (i = 0; i < count; ++i) {
        /* 'cpu' is an abstract type in QEMU.
         * Replace it with a generic type that can be defined by each
         * architecture. */
        if (!strcmp(node->compat[i], "cpu")) {
            node->compat[i] = TYPE_FDT_GENERIC_CPU_TYPE;
        }
    }
    node->compat[count] = NULL;

    return;
}

const char *fdt_machine_get_string(const FDTMachineNode *node, const char *name,
                                   unsigned int *len, Error **errp)
{
    const FDTMachineProperty *prop =
        fdt_machine_find_property(node, name, errp);

    if (prop) {
        const char *res = prop->value;
        if (res[prop->length] == '\0') {
            if (len) {
                *len = prop->length;
            }
            return res;
        }
        error_setg(errp, "%s: Property %s in node %s isn't a terminated string",
                   __func__, name, node->path);
    }
    return NULL;
}

const char *fdt_machine_get_string_array_element(const FDTMachineNode *node,
                                                 const char *name,
                                                 unsigned int index,
                                                 Error **errp)
{
    unsigned int len;
    const char *strArray = fdt_machine_get_string(node, name, &len, errp);
    const char *end;

    if (!strArray) {
        return NULL;
    }

    end = strArray + len;

    while (index--) {
        size_t l = strnlen(strArray, end - strArray);
        if (strArray + l + 1 >= end) {
            error_setg(errp, "Index out of range for property '%s'", name);
            return NULL;
        }

        strArray += l + 1;
    }
    return strArray;
}

bool fdt_machine_get_bool(const FDTMachineNode *node, const char *name)
{
    const FDTMachineProperty *prop =
        fdt_machine_find_property(node, name, NULL);

    return (prop != NULL);
}

uint32_t fdt_machine_get_u32_default(const FDTMachineNode *node,
                                     const char *name, uint32_t dflt)
{
    const FDTMachineProperty *prop =
        fdt_machine_find_property(node, name, NULL);

    if (prop) {
        if (prop->length == 4) {
            return fdt32_to_cpu(*(fdt32_t *)prop->value);
        }
    }

    return dflt;
}

uint32_t fdt_machine_get_u32(const FDTMachineNode *node, const char *name,
                             Error **errp)
{
    const FDTMachineProperty *prop =
        fdt_machine_find_property(node, name, errp);

    if (prop) {
        if (prop->length == 4) {
            return fdt32_to_cpu(*(fdt32_t *)prop->value);
        }

        error_setg(errp, "%s: Property %s in node %s isn't a u32 (length %d)",
                   __func__, name, node->path, prop->length);
    }

    return 0;
}

uint32_t fdt_machine_get_u32_array_element(const FDTMachineNode *node,
                                           const char *name, unsigned int index,
                                           Error **errp)
{
    const FDTMachineProperty *prop =
        fdt_machine_find_property(node, name, errp);

    if (prop) {
        if ((prop->length % 4 == 0) &&
            ((index * sizeof(fdt32_t) < prop->length))) {
            return fdt32_to_cpu(
                *(fdt32_t *)(prop->value + index * sizeof(fdt32_t)));
        }

        error_setg(errp,
                   "%s: Property %s in node %s isn't a u32 array (length %d)",
                   __func__, name, node->path, prop->length);
    }

    return 0;
}

uint64_t fdt_machine_get_uint(const FDTMachineNode *node, const char *name,
                              Error **errp)
{
    const FDTMachineProperty *prop =
        fdt_machine_find_property(node, name, errp);

    if (prop) {
        switch (prop->length) {
        case 1:
            return *(uint8_t *)prop->value;
        case 2:
            return fdt16_to_cpu(*(fdt16_t *)prop->value);
        case 4:
            return fdt32_to_cpu(*(fdt32_t *)prop->value);
        case 8:
            return fdt64_to_cpu(*(fdt64_t *)prop->value);
        }

        error_setg(errp,
                   "%s: Property %s in node %s isn't a valid size (length %d)",
                   __func__, name, node->path, prop->length);
    }

    return 0;
}

FDTMachineNode *fdt_machine_get_phandle(const FDTMachineNode *node,
                                        uint32_t phandle)
{
    return (phandle < node->phandles->size) ? node->phandles->nodes[phandle] :
                                              NULL;
}

FDTMachineResultState fdt_machine_get_state(const FDTMachineNode *node)
{
    return node->result.state;
}

Object *fdt_machine_wait_for_state(FDTMachineNode *node,
                                   FDTMachineResultState state)
{
    while (node->result.state < state) {
        qemu_co_queue_wait(&node->result.waitq, NULL);
    }

    return node->result.obj;
}

Object *fdt_machine_get_object(FDTMachineNode *node)
{
    return fdt_machine_wait_for_state(node, FDT_NODE_CREATED);
}

void fdt_machine_set_object(FDTMachineNode *node, Object *obj)
{
    node->result.obj = obj;
    object_ref(obj);
    node->result.state = FDT_NODE_CREATED;
    qemu_co_queue_restart_all(&node->result.waitq);
}

void fdt_machine_set_realized(FDTMachineNode *node)
{
    node->result.state = FDT_NODE_REALIZED;
    if (object_dynamic_cast(node->result.obj, TYPE_DEVICE)) {
        object_property_set_bool(OBJECT(node->result.obj), "realized", true,
                                 &error_abort);
    }
    qemu_co_queue_restart_all(&node->result.waitq);
}

void fdt_machine_set_ready(FDTMachineNode *node)
{
    node->result.state = FDT_NODE_READY;
    qemu_co_queue_restart_all(&node->result.waitq);
}

bool fdt_machine_get_cont_range(const FDTMachineNode *node, uint64_t *addr,
                                uint64_t *size, Error **errp)
{
    uint64_t base_addr = ~0ull;
    uint64_t total_size = 0;
    uint64_t max_addr = 0;
    int i;

    for (i = 0; i < node->regs.size; ++i) {
        const FDTMachineReg *reg = &node->regs.r[i];
        base_addr = MIN(base_addr, reg->address);
        max_addr = MAX(max_addr, reg->address + reg->size);
        total_size += reg->size;
    }

    if (total_size != max_addr - base_addr) {
        error_setg(errp,
                   "FDT memory parse does not support empty, discontiguous, or "
                   "overlapping memory regions");
        return false;
    }

    *addr = base_addr;
    *size = total_size;
    return true;
}
