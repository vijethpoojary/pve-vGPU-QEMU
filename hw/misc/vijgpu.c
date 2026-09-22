/*
 * vijGPU - Virtual GPU PCI device for QEMU
 *
 * Copyright (c) 2026 vijGPU contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * BAR0 register map (4 KiB, 32-bit accesses):
 *   0x00  VIJGPU_REG_ID      RO  0x56494A47 ("VIJG")
 *   0x04  VIJGPU_REG_VER     RO  0x00000001
 *   0x08  VIJGPU_REG_CTRL    RW  control register (guest-visible state)
 *   0x0c+ reserved           RO  0x00000000
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qemu/module.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "qom/object.h"

#define TYPE_VIJGPU_DEVICE      "vijgpu"

OBJECT_DECLARE_SIMPLE_TYPE(VijGPUState, VIJGPU_DEVICE)

/* PCI identifiers */
#define VIJGPU_VENDOR_ID        0x1234
#define VIJGPU_DEVICE_ID        0x1200
#define VIJGPU_REVISION         0x01

/* BAR0 layout */
#define VIJGPU_BAR0_SIZE        (4 * KiB)

/* Register offsets within BAR0 */
#define VIJGPU_REG_ID           0x00    /* RO: device signature "VIJG" */
#define VIJGPU_REG_VER          0x04    /* RO: device version 1 */
#define VIJGPU_REG_CTRL         0x08    /* RW: control register */

/* Fixed register values */
#define VIJGPU_ID_VALUE         0x56494A47U  /* "VIJG" little-endian */
#define VIJGPU_VER_VALUE        0x00000001U

struct VijGPUState {
    /* PCIDevice must be the first member */
    PCIDevice pdev;

    MemoryRegion bar0;

    /* Device state */
    uint32_t ctrl;
};

/* ------------------------------------------------------------------
 * BAR0 MMIO read
 * ------------------------------------------------------------------ */

static uint64_t vijgpu_bar0_read(void *opaque, hwaddr addr, unsigned size)
{
    VijGPUState *s = opaque;

    switch (addr) {
    case VIJGPU_REG_ID:
        return VIJGPU_ID_VALUE;
    case VIJGPU_REG_VER:
        return VIJGPU_VER_VALUE;
    case VIJGPU_REG_CTRL:
        return s->ctrl;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "vijgpu: read from reserved offset 0x%"HWADDR_PRIx"\n",
                      addr);
        return 0;
    }
}

/* ------------------------------------------------------------------
 * BAR0 MMIO write
 * ------------------------------------------------------------------ */

static void vijgpu_bar0_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    VijGPUState *s = opaque;

    switch (addr) {
    case VIJGPU_REG_CTRL:
        s->ctrl = (uint32_t)val;
        break;
    case VIJGPU_REG_ID:
    case VIJGPU_REG_VER:
        /* read-only registers: ignore writes silently */
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "vijgpu: write to reserved offset 0x%"HWADDR_PRIx
                      " value 0x%"PRIx64"\n", addr, val);
        break;
    }
}

static const MemoryRegionOps vijgpu_bar0_ops = {
    .read  = vijgpu_bar0_read,
    .write = vijgpu_bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* ------------------------------------------------------------------
 * PCI realize / unrealize
 * ------------------------------------------------------------------ */

static void vijgpu_realize(PCIDevice *pdev, Error **errp)
{
    VijGPUState *s = VIJGPU_DEVICE(pdev);

    memory_region_init_io(&s->bar0, OBJECT(s), &vijgpu_bar0_ops, s,
                          "vijgpu-bar0", VIJGPU_BAR0_SIZE);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);
}

/* ------------------------------------------------------------------
 * Class and type registration
 * ------------------------------------------------------------------ */

static void vijgpu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);

    pc->realize   = vijgpu_realize;
    pc->vendor_id = VIJGPU_VENDOR_ID;
    pc->device_id = VIJGPU_DEVICE_ID;
    pc->revision  = VIJGPU_REVISION;
    pc->class_id  = PCI_CLASS_DISPLAY_OTHER;

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->desc = "vijGPU virtual GPU";
}

static const TypeInfo vijgpu_info = {
    .name          = TYPE_VIJGPU_DEVICE,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(VijGPUState),
    .class_init    = vijgpu_class_init,
    .interfaces    = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void vijgpu_register_types(void)
{
    type_register_static(&vijgpu_info);
}

type_init(vijgpu_register_types);
