/*
 * FZI hardware identification device emulation
 *
 * Copyright (c) 2024 FZI Forschungszentrum Informatik
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_FZIHWIDENT_H
#define HW_FZIHWIDENT_H

#include <stdint.h>
#include <stdbool.h>

#define FZIHWIDENT_MAGIC_OFFSET       0x00
static const uint32_t fzihwident_magic = {0x4E4E3139};

#define FZIHWIDENT_BOARDID_OFFSET     0x08
#define FZIHWIDENT_BOARDID_LEN        4

#define FZIHWIDENT_SHA_OFFSET         0x10
#define FZIHWIDENT_SHA_LEN            4

#define FZIHWIDENT_TIMESTAMP_OFFSET    0x20
#define FZIHWIDENT_TIMESTAMP_LEN       8
#define FZIHWIDENT_TIMESTAMP_OFFSET_L  0x20
#define FZIHWIDENT_TIMESTAMP_OFFSET_H  0x24

#define TYPE_FZI_HWIDENT "fzi.hwidentregs"

typedef struct fziHwIdentState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    CPUState *cpu;

    MemoryRegion mmio;

    char *boardId;
    uint32_t shortSha;
    uint64_t timestamp;
    bool dirtyBuild;

} fziHwIdentState;

fziHwIdentState *fziHwIdent_create(MemoryRegion *address_space, hwaddr base);

#endif /* HW_FZIHWIDENT_H */
