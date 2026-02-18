#ifndef FDT_MACHINE_H
#define FDT_MACHINE_H

#include "qemu/help-texts.h"
#include "qom/object.h"
#include "system/memory.h"
#include "hw/core/fdt_machine_info.h"

/* create a fdt_generic machine. The client is responsible for
 * destroying the returned FDTMachineInfo (using fdt_machine_free)
 */
FDTMachineInfo *fdt_machine_create(void *fdt);


#define TYPE_FDT_GENERIC_CPU_TYPE "fdt_generic_cpu"

#define TYPE_FDT_GENERIC_INTC "fdt-generic-intc"

#define FDT_GENERIC_INTC_CLASS(klass) \
    OBJECT_CLASS_CHECK(FDTGenericIntcClass, (klass), TYPE_FDT_GENERIC_INTC)
#define FDT_GENERIC_INTC_GET_CLASS(obj) \
    OBJECT_GET_CLASS(FDTGenericIntcClass, (obj), TYPE_FDT_GENERIC_INTC)
#define FDT_GENERIC_INTC(obj) \
    INTERFACE_CHECK(FDTGenericIntc, (obj), TYPE_FDT_GENERIC_INTC)

typedef struct FDTGenericIntc {
    /*< private >*/
    Object parent_obj;
} FDTGenericIntc;

typedef struct FDTGenericIntcClass {
    /*< private >*/
    InterfaceClass parent_class;

    /*< public >*/
    qemu_irq (*get_irq)(FDTGenericIntc *obj, FDTMachineIntr *intr,
                        Error **errp);
} FDTGenericIntcClass;


#define TYPE_FDT_GENERIC_MMAP "fdt-generic-mmap"

#define FDT_GENERIC_MMAP_CLASS(klass) \
    OBJECT_CLASS_CHECK(FDTGenericMMapClass, (klass), TYPE_FDT_GENERIC_MMAP)
#define FDT_GENERIC_MMAP_GET_CLASS(obj) \
    OBJECT_GET_CLASS(FDTGenericMMapClass, (obj), TYPE_FDT_GENERIC_MMAP)
#define FDT_GENERIC_MMAP(obj) \
    INTERFACE_CHECK(FDTGenericMMap, (obj), TYPE_FDT_GENERIC_MMAP)

typedef struct FDTGenericMMap {
    /*< private >*/
    Object parent_obj;
} FDTGenericMMap;

typedef struct FDTGenericMMapClass {
    /*< private >*/
    InterfaceClass parent_class;

    /*< public >*/
    void (*parse_reg)(FDTGenericMMap *obj, const FDTMachineNode *node,
                      Error **errp);
} FDTGenericMMapClass;

#endif /* FDT_MACHINE_H */
