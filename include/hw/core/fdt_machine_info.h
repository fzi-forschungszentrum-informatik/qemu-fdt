#ifndef FDT_MACHINE_INFO_H
#define FDT_MACHINE_INFO_H

#include "qemu/osdep.h"

#include "qapi/error.h"
#include "qemu/coroutine.h"
#include <libfdt.h>
#include <stddef.h>
#include <stdint.h>

struct FDTMachineNode;
typedef struct FDTMachineNode FDTMachineNode;

typedef struct FDTMachineProperty {
    const char *name;
    const void *value;
    unsigned int length;
    int fdt_machine_offset;
} FDTMachineProperty;

typedef struct FDTMachineReg {
    uint64_t address;
    uint64_t size;
} FDTMachineReg;

typedef struct FDTMachineRegs {
    unsigned int size;
    FDTMachineReg *r;
} FDTMachineRegs;

typedef struct FDTMachineRange {
    uint64_t child_addr;
    uint64_t parent_addr;
    uint64_t size;
} FDTMachineRange;

typedef struct FDTMachineRanges {
    unsigned int size;
    FDTMachineRange *r;
} FDTMachineRanges;

typedef struct FDTMachineIntr {
    FDTMachineNode *intr_parent;
    unsigned int size;
    uint32_t cells[FDT_MAX_NCELLS];
} FDTMachineIntr;

typedef struct FDTMachineIntrs {
    unsigned int size;
    FDTMachineIntr *i;
} FDTMachineIntrs;

typedef enum FDTMachineResultState {
    FDT_NODE_UNINITIALIZED = 0U,
    FDT_NODE_CREATED = 1U,
    FDT_NODE_REALIZED = 2U,
    FDT_NODE_READY = 3U,
} FDTMachineResultState;

typedef struct FDTMachineResult {
    FDTMachineResultState state;
    Object *obj;
    Coroutine *co;
    CoQueue waitq;
} FDTMachineResult;

typedef struct FDTMachineHandles {
    unsigned int size;
    FDTMachineNode **nodes;
} FDTMachineHandles;

typedef struct FDTMachineNode {
    const char *name;
    char *short_name;
    uint64_t unit_addr;
    char *path;
    uint32_t phandle;
    int fdt_machine_offset;

    unsigned int prop_count;
    FDTMachineProperty *props;

    unsigned int child_count;
    FDTMachineNode *children;
    
    FDTMachineNode *parent;

    const FDTMachineHandles *phandles;

    FDTMachineRegs regs;
    FDTMachineRanges ranges;
    FDTMachineIntrs intrs;

    FDTMachineResult result;
    const char **compat;
} FDTMachineNode;

typedef struct FDTMachineInfo {
    const void *fdt;
    FDTMachineNode *root;
    FDTMachineHandles phandles;
} FDTMachineInfo;


/* Property functions */
const char *fdt_machine_get_string(const FDTMachineNode *node, const char *name,
                            unsigned int *len, Error **errp);
const char *fdt_machine_get_string_array_element(const FDTMachineNode *node,
                                          const char *name, unsigned int index,
                                          Error **errp);
bool fdt_machine_get_bool(const FDTMachineNode *node, const char *name);
uint32_t fdt_machine_get_u32(const FDTMachineNode *node, const char *name, Error **errp);
uint32_t fdt_machine_get_u32_array_element(const FDTMachineNode *node, const char *name,
                                    unsigned int index, Error **errp);
uint32_t fdt_machine_get_u32_default(const FDTMachineNode *node, const char *name,
                              uint32_t dflt);
uint64_t fdt_machine_get_uint(const FDTMachineNode *node, const char *name, Error **errp);
FDTMachineNode *fdt_machine_get_phandle(const FDTMachineNode *node, uint32_t phandle);
const FDTMachineProperty *fdt_machine_find_property(const FDTMachineNode *node,
                    const char *name, Error **errp);
static inline const char *fdt_machine_trim_vendor(const char *s)
{
    const char *p = strchr(s, ',');
    return p ? p + 1 : s;
}

/* State functions */
Object *fdt_machine_get_object(FDTMachineNode *node);
FDTMachineResultState fdt_machine_get_state(const FDTMachineNode *node);
Object *fdt_machine_wait_for_state(FDTMachineNode *node, FDTMachineResultState state);

void fdt_machine_set_object(FDTMachineNode *node, Object *obj);
void fdt_machine_set_realized(FDTMachineNode *node);
void fdt_machine_set_ready(FDTMachineNode *node);

/* Helper for regs */
bool fdt_machine_get_cont_range(const FDTMachineNode *node, uint64_t *addr, uint64_t *size,
                         Error **errp);


/* Move to machine: */
/* Parse and free */
FDTMachineInfo *fdt_machine_parse(const void *fdt);
void fdt_machine_free(FDTMachineInfo *fdti);

#endif /* FDT_MACHINE_INFO_H */
