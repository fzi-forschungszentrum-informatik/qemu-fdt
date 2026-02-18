/*
 * Tables of FDT device models and their init functions. Keyed by compatibility
 * strings, device instance names.
 */

#ifndef FDT_MACHINE_REGISTER_H
#define FDT_MACHINE_REGISTER_H

#include "qemu/help-texts.h"
#include "hw/core/irq.h"
#include "system/device_tree.h"
#include "qemu/coroutine.h"
#include "hw/core/fdt_machine_info.h"

#define FDT_MACHINE_LOG(node, ...)                        \
    do {                                            \
        qemu_log_mask(LOG_FDT, "%s", node->path);   \
        qemu_log_mask(LOG_FDT, ": %s: ", __func__); \
        qemu_log_mask(LOG_FDT, ##__VA_ARGS__);      \
    } while (0)

    
typedef Object *(*FDTInitFn)(FDTMachineNode *, void *);

/* associate a FDTInitFn with a FDT compatibility */

void fdt_register_add_to_compat(FDTInitFn, const char *, void *);

/* try and find a device model for a particular compatibility. If found,
 * the FDTInitFn associated with the compat will be called and 0 will
 * be returned. Returns non-zero on not found or error
 */
Object *fdt_register_create_compat(FDTMachineNode *node, const char *compat);

void fdt_register_dump_table(void);

/* statically register a FDTInitFn as being associate with a compatibility */
#define fdt_register_compatibility_opaque(function, compat, opaque) \
static void __attribute__((constructor)) \
glue(glue(function, __COUNTER__), _register_imp)(void) { \
    fdt_register_add_to_compat(function, compat, opaque); \
}

#define fdt_register_compatibility(function, compat) \
fdt_register_compatibility_opaque(function, compat, NULL)

#endif /* FDT_MACHINE_REGISTER_H */
