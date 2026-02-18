/*
 * Tables of FDT device models and their init functions. Keyed by compatibility
 * strings, device instance names.
 *
 * Copyright (c) 2010 PetaLogix Qld Pty Ltd.
 * Copyright (c) 2010 Peter A. G. Crosthwaite <peter.crosthwaite@petalogix.com>.
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
#include "hw/core/fdt_machine_register.h"
#include "hw/core/qdev-properties.h"
#include "hw/cpu/cluster.h"
#include "migration/vmstate.h"
#include "qemu/coroutine.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "system/reset.h"

#include "qapi/error.h"
#include <libfdt.h>

#define FDT_GENERIC_MAX_PATTERN_LEN 1024

typedef struct TableListNode {
    struct TableListNode *next;
    char key[FDT_GENERIC_MAX_PATTERN_LEN];
    FDTInitFn fdt_init;
    void *opaque;
} TableListNode;

/* add a node to the table specified by *head_p */

static void add_to_table(FDTInitFn fdt_init, const char *key, void *opaque,
                         TableListNode **head_p)
{
    TableListNode *nn = g_malloc(sizeof(*nn));

    assert(strlen(key) < sizeof(nn->key));
    nn->next = *head_p;
    pstrcpy(nn->key, sizeof(nn->key), key);
    nn->fdt_init = fdt_init;
    nn->opaque = opaque;
    *head_p = nn;
}

/* search a table for a key string and call the fdt init function if found.
 */

static Object *fdt_init_search_table(FDTMachineNode *node, const char *key,
                                     TableListNode **head)
{
    TableListNode *iter;

    for (iter = *head; iter != NULL; iter = iter->next) {
        if (!strcmp(key, iter->key)) {
            assert(iter->fdt_init);
            FDT_MACHINE_LOG(node, "matched compat %s\n", key);
            return iter->fdt_init(node, iter->opaque);
        }
    }

    return NULL;
}

static TableListNode *compat_list_head;

void fdt_register_add_to_compat(FDTInitFn fdt_init, const char *compat,
                                void *opaque)
{
    assert(fdt_init);
    add_to_table(fdt_init, compat, opaque, &compat_list_head);
}

Object *fdt_register_create_compat(FDTMachineNode *node, const char *compat)
{
    return fdt_init_search_table(node, compat, &compat_list_head);
}

static void dump_table(TableListNode *head)
{
    TableListNode *iter;

    for (iter = head; iter != NULL; iter = iter->next) {
        qemu_log("key : %s, opaque data %p\n", iter->key, iter->opaque);
    }
}

void fdt_register_dump_table(void)
{
    qemu_log("FDT COMPATIBILITY TABLE:\n");
    dump_table(compat_list_head);
}
