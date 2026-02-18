/*
 * Remote-port stubs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/qdev.h"
#include "hw/rp/remote-port.h"

bool rp_device_add(const QDict *opts, DeviceState *dev, Error **errp)
{
    return true;
}
