/*
 * vijGPU - Software-only Virtual GPU PCI device for QEMU
 *
 * Copyright (c) 2026 vijGPU contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * -----------------------------------------------------------------------
 * ARCHITECTURE
 * -----------------------------------------------------------------------
 *
 * PCI layout:
 *   BAR0  4 KiB   MMIO control/status registers  (read/write via MMIO)
 *   BAR1  256 MiB Virtual VRAM                   (guest-mapped RAM)
 *   BAR2  4 KiB   Command queue registers         (doorbell + ring ctrl)
 *
 * Guest driver workflow:
 *   1. Map BAR0 → read identity/version, write CTRL/RESET.
 *   2. Map BAR1 → direct read/write of virtual VRAM.
 *   3. Map BAR2 → set up command ring (guest-allocated DMA buffer).
 *   4. Write commands into the ring, advance TAIL, ring doorbell.
 *   5. QEMU processes commands synchronously on MMIO write.
 *   6. QEMU updates COMPLETION_STATUS, COMPLETION_SEQ, IRQ_STATUS.
 *   7. Guest reads IRQ_STATUS / COMPLETION_STATUS, clears interrupt.
 *
 * -----------------------------------------------------------------------
 * BAR0 REGISTER MAP  (all registers 32-bit, little-endian)
 * -----------------------------------------------------------------------
 *
 * Identity / version
 *   0x00  DEVICE_ID       RO  0x56494A47  ("VIJG")
 *   0x04  VERSION         RO  0x00020000  (major=2, minor=0)
 *
 * Device status / control
 *   0x08  STATUS          RO  device status flags (see VIJGPU_STATUS_*)
 *   0x0C  CTRL            RW  control register    (see VIJGPU_CTRL_*)
 *
 * Virtual VRAM information
 *   0x10  VRAM_SIZE_LO    RO  lower 32 bits of VRAM size in bytes
 *   0x14  VRAM_SIZE_HI    RO  upper 32 bits of VRAM size in bytes
 *
 * Command queue
 *   0x20  CQ_ADDR_LO      RW  lower 32 bits of guest command ring PA
 *   0x24  CQ_ADDR_HI      RW  upper 32 bits of guest command ring PA
 *   0x28  CQ_SIZE         RW  number of entries in the ring (must be >=1)
 *   0x2C  CQ_HEAD         RO  head index (device-maintained)
 *   0x30  CQ_TAIL         RW  tail index (guest writes to submit commands)
 *                             Writing CQ_TAIL triggers command processing.
 *
 * Completion
 *   0x34  COMPL_STATUS    RO  status of last completed command
 *                             (see VIJGPU_COMPL_* values)
 *   0x38  COMPL_SEQ_LO    RO  sequence number of last completed command
 *                             (lower 32 bits)
 *   0x3C  COMPL_SEQ_HI    RO  sequence number (upper 32 bits)
 *
 * Interrupts
 *   0x40  IRQ_STATUS      RO/W1C  pending interrupt flags
 *   0x44  IRQ_MASK        RW      interrupt enable mask (1=enabled)
 *
 * Reset
 *   0x48  RESET           WO  write VIJGPU_RESET_KEY to perform device reset
 *
 * -----------------------------------------------------------------------
 * BAR2 REGISTER MAP  (all registers 32-bit, little-endian)
 * Mirrors the BAR0 command-queue and interrupt registers for convenience.
 * -----------------------------------------------------------------------
 *
 *   0x00  DOORBELL        WO  write any value to reprocess pending commands
 *   0x04  CQ_SIZE         RW  same as BAR0 0x28
 *   0x08  CQ_HEAD         RO  same as BAR0 0x2C
 *   0x0C  CQ_TAIL         RW  same as BAR0 0x30 (writing triggers processing)
 *   0x10  IRQ_STATUS      RO/W1C  same as BAR0 0x40
 *   0x14  IRQ_MASK        RW  same as BAR0 0x44
 *
 * -----------------------------------------------------------------------
 * COMMAND FORMAT  (fixed 40 bytes per entry)
 * -----------------------------------------------------------------------
 *
 *  Offset  Size  Field
 *    0      4    opcode    (uint32_t, see VIJGPU_CMD_*)
 *    4      4    flags     (uint32_t, reserved, must be 0)
 *    8      8    arg0      (uint64_t, command-specific)
 *   16      8    arg1      (uint64_t, command-specific)
 *   24      8    arg2      (uint64_t, command-specific)
 *   32      8    sequence  (uint64_t, caller-assigned, echoed in completion)
 *
 * Commands:
 *   NOP  (0)          — does nothing, completes successfully.
 *
 *   WRITE_REG (1)     — write a writable BAR0 register.
 *                       arg0 = BAR0 offset (must be writable)
 *                       arg1 = 32-bit value to write
 *                       arg2 = unused
 *
 *   READ_REG (2)      — read a BAR0 register.
 *                       arg0 = BAR0 offset
 *                       arg1 = unused
 *                       arg2 = unused
 *                       result stored in COMPL_SEQ (lower 32 bits)
 *
 *   FILL_VRAM (3)     — fill a region of virtual VRAM.
 *                       arg0 = byte offset into VRAM
 *                       arg1 = byte length
 *                       arg2 = fill value (only low 8 bits used)
 *
 *   COPY_VRAM (4)     — copy within virtual VRAM.
 *                       arg0 = source offset
 *                       arg1 = destination offset
 *                       arg2 = byte length
 *
 * -----------------------------------------------------------------------
 * COMPLETION STATUS values  (COMPL_STATUS register)
 * -----------------------------------------------------------------------
 *   0  SUCCESS
 *   1  INVALID_COMMAND   (unknown opcode)
 *   2  INVALID_ARGUMENT  (bad register offset or other bad arg)
 *   3  OUT_OF_BOUNDS     (VRAM offset+length exceeds VRAM size)
 *   4  QUEUE_ERROR       (queue not configured or ring wrap problem)
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qemu/module.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "hw/core/qdev-properties.h"
#include "qom/object.h"
#include "qapi/error.h"
#include "system/memory.h"

/* -----------------------------------------------------------------------
 * Type / QOM declarations
 * --------------------------------------------------------------------- */

#define TYPE_VIJGPU_DEVICE  "vijgpu"
OBJECT_DECLARE_SIMPLE_TYPE(VijGPUState, VIJGPU_DEVICE)

/* -----------------------------------------------------------------------
 * PCI identity — DO NOT CHANGE
 * --------------------------------------------------------------------- */

#define VIJGPU_VENDOR_ID    0x1234
#define VIJGPU_DEVICE_ID    0x1200
#define VIJGPU_REVISION     0x01

/* -----------------------------------------------------------------------
 * BAR sizes
 * --------------------------------------------------------------------- */

#define VIJGPU_BAR0_SIZE    (4 * KiB)   /* control registers */
#define VIJGPU_BAR2_SIZE    (4 * KiB)   /* command queue registers */

/* Default VRAM size; guest sees this as BAR1 */
#define VIJGPU_VRAM_DEFAULT (256 * MiB)
#define VIJGPU_VRAM_MIN     (4 * MiB)
#define VIJGPU_VRAM_MAX     (2048ULL * MiB)

/* -----------------------------------------------------------------------
 * BAR0 register offsets
 * --------------------------------------------------------------------- */

#define R_DEVICE_ID         0x00   /* RO */
#define R_VERSION           0x04   /* RO */
#define R_STATUS            0x08   /* RO */
#define R_CTRL              0x0C   /* RW */
#define R_VRAM_SIZE_LO      0x10   /* RO */
#define R_VRAM_SIZE_HI      0x14   /* RO */
/* 0x18, 0x1C: reserved */
#define R_CQ_ADDR_LO        0x20   /* RW */
#define R_CQ_ADDR_HI        0x24   /* RW */
#define R_CQ_SIZE           0x28   /* RW */
#define R_CQ_HEAD           0x2C   /* RO */
#define R_CQ_TAIL           0x30   /* RW - write triggers processing */
#define R_COMPL_STATUS      0x34   /* RO */
#define R_COMPL_SEQ_LO      0x38   /* RO */
#define R_COMPL_SEQ_HI      0x3C   /* RO */
#define R_IRQ_STATUS        0x40   /* RO / W1C */
#define R_IRQ_MASK          0x44   /* RW */
#define R_RESET             0x48   /* WO */

/* BAR2 register offsets */
#define R2_DOORBELL         0x00   /* WO */
#define R2_CQ_SIZE          0x04   /* RW (mirrors R_CQ_SIZE) */
#define R2_CQ_HEAD          0x08   /* RO (mirrors R_CQ_HEAD) */
#define R2_CQ_TAIL          0x0C   /* RW (mirrors R_CQ_TAIL) */
#define R2_IRQ_STATUS       0x10   /* RO/W1C (mirrors R_IRQ_STATUS) */
#define R2_IRQ_MASK         0x14   /* RW (mirrors R_IRQ_MASK) */

/* -----------------------------------------------------------------------
 * Register / field values
 * --------------------------------------------------------------------- */

#define VIJGPU_DEVICE_ID_VALUE  0x56494A47U  /* "VIJG" */
#define VIJGPU_VERSION_VALUE    0x00020000U  /* 2.0 */
#define VIJGPU_RESET_KEY        0xDEADBEEFU

/* STATUS flags */
#define VIJGPU_STATUS_READY     (1U << 0)   /* device is ready */
#define VIJGPU_STATUS_BUSY      (1U << 1)   /* processing a command */
#define VIJGPU_STATUS_ERROR     (1U << 2)   /* last operation errored */

/* CTRL flags */
#define VIJGPU_CTRL_IRQ_ENABLE  (1U << 0)   /* global IRQ enable */

/* IRQ_STATUS / IRQ_MASK bits */
#define VIJGPU_IRQ_CMD_DONE     (1U << 0)   /* command(s) completed */

/* Completion status codes */
#define VIJGPU_COMPL_SUCCESS        0U
#define VIJGPU_COMPL_INVALID_CMD    1U
#define VIJGPU_COMPL_INVALID_ARG    2U
#define VIJGPU_COMPL_OUT_OF_BOUNDS  3U
#define VIJGPU_COMPL_QUEUE_ERROR    4U

/* -----------------------------------------------------------------------
 * Command structure  (40 bytes, little-endian)
 * --------------------------------------------------------------------- */

#define VIJGPU_CMD_NOP          0U
#define VIJGPU_CMD_WRITE_REG    1U
#define VIJGPU_CMD_READ_REG     2U
#define VIJGPU_CMD_FILL_VRAM    3U
#define VIJGPU_CMD_COPY_VRAM    4U

#define VIJGPU_CMD_SIZE         40U   /* bytes per command entry */

/* Maximum ring size we accept from the guest */
#define VIJGPU_CQ_MAX_ENTRIES   4096U

/* Maximum bytes we process per doorbell ring to prevent infinite loops */
#define VIJGPU_CQ_MAX_PROCESS   1024U

/* -----------------------------------------------------------------------
 * Device state
 * --------------------------------------------------------------------- */

struct VijGPUState {
    /* PCIDevice MUST be first */
    PCIDevice       pdev;

    /* BARs */
    MemoryRegion    bar0;       /* control registers */
    MemoryRegion    bar1_vram;  /* virtual VRAM (RAM-backed) */
    MemoryRegion    bar2;       /* command queue registers */

    /* VRAM backing — host RAM, no physical GPU */
    uint64_t        vram_size;  /* configurable, default 256 MiB */

    /* Command queue configuration (written by guest) */
    uint64_t        cq_addr;    /* guest physical address of ring buffer */
    uint32_t        cq_size;    /* number of entries */
    uint32_t        cq_head;    /* device-owned head index */
    uint32_t        cq_tail;    /* last tail written by guest */

    /* Device control/status */
    uint32_t        ctrl;
    uint32_t        status;

    /* Completion state */
    uint32_t        compl_status;
    uint64_t        compl_seq;

    /* Interrupts */
    uint32_t        irq_status;
    uint32_t        irq_mask;
};

/* -----------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------- */

static void vijgpu_process_commands(VijGPUState *s);
static void vijgpu_raise_irq(VijGPUState *s, uint32_t mask);
static void vijgpu_reset_device(VijGPUState *s);

/* -----------------------------------------------------------------------
 * Interrupt helpers
 * --------------------------------------------------------------------- */

static void vijgpu_raise_irq(VijGPUState *s, uint32_t mask)
{
    s->irq_status |= mask;
    if (s->irq_status & s->irq_mask) {
        if (msi_enabled(&s->pdev)) {
            msi_notify(&s->pdev, 0);
        } else {
            pci_set_irq(&s->pdev, 1);
        }
    }
}

static void vijgpu_lower_irq(VijGPUState *s)
{
    if (!msi_enabled(&s->pdev)) {
        pci_set_irq(&s->pdev, 0);
    }
}

static void vijgpu_update_irq(VijGPUState *s)
{
    if (s->irq_status & s->irq_mask) {
        if (msi_enabled(&s->pdev)) {
            msi_notify(&s->pdev, 0);
        } else {
            pci_set_irq(&s->pdev, 1);
        }
    } else {
        vijgpu_lower_irq(s);
    }
}

/* -----------------------------------------------------------------------
 * Command processing
 * --------------------------------------------------------------------- */

/*
 * Execute a single command.  Updates compl_status and compl_seq.
 * Returns true if processing should continue, false on a fatal queue error.
 */
static bool vijgpu_execute_command(VijGPUState *s,
                                   uint32_t opcode,
                                   uint32_t flags,
                                   uint64_t arg0,
                                   uint64_t arg1,
                                   uint64_t arg2,
                                   uint64_t sequence)
{
    (void)flags; /* reserved, ignored */

    s->compl_seq = sequence;

    switch (opcode) {

    case VIJGPU_CMD_NOP:
        s->compl_status = VIJGPU_COMPL_SUCCESS;
        break;

    case VIJGPU_CMD_WRITE_REG: {
        /*
         * arg0 = BAR0 register offset
         * arg1 = 32-bit value
         * Only writable registers are permitted.
         */
        uint32_t offset = (uint32_t)arg0;
        uint32_t value  = (uint32_t)arg1;

        switch (offset) {
        case R_CTRL:
            s->ctrl = value;
            s->compl_status = VIJGPU_COMPL_SUCCESS;
            break;
        case R_CQ_ADDR_LO:
            s->cq_addr = (s->cq_addr & 0xFFFFFFFF00000000ULL) | value;
            s->compl_status = VIJGPU_COMPL_SUCCESS;
            break;
        case R_CQ_ADDR_HI:
            s->cq_addr = (s->cq_addr & 0x00000000FFFFFFFFULL) |
                         ((uint64_t)value << 32);
            s->compl_status = VIJGPU_COMPL_SUCCESS;
            break;
        case R_CQ_SIZE:
            if (value == 0 || value > VIJGPU_CQ_MAX_ENTRIES) {
                s->compl_status = VIJGPU_COMPL_INVALID_ARG;
            } else {
                s->cq_size = value;
                s->compl_status = VIJGPU_COMPL_SUCCESS;
            }
            break;
        case R_IRQ_MASK:
            s->irq_mask = value;
            vijgpu_update_irq(s);
            s->compl_status = VIJGPU_COMPL_SUCCESS;
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "vijgpu: WRITE_REG to non-writable offset 0x%x\n",
                          offset);
            s->compl_status = VIJGPU_COMPL_INVALID_ARG;
            break;
        }
        break;
    }

    case VIJGPU_CMD_READ_REG: {
        /*
         * arg0 = BAR0 register offset
         * Result placed in compl_seq (lower 32 bits).
         */
        uint32_t offset = (uint32_t)arg0;
        uint32_t result = 0;

        switch (offset) {
        case R_DEVICE_ID:        result = VIJGPU_DEVICE_ID_VALUE; break;
        case R_VERSION:          result = VIJGPU_VERSION_VALUE;   break;
        case R_STATUS:           result = s->status;              break;
        case R_CTRL:             result = s->ctrl;                break;
        case R_VRAM_SIZE_LO:     result = (uint32_t)(s->vram_size);          break;
        case R_VRAM_SIZE_HI:     result = (uint32_t)(s->vram_size >> 32);    break;
        case R_CQ_ADDR_LO:       result = (uint32_t)(s->cq_addr);            break;
        case R_CQ_ADDR_HI:       result = (uint32_t)(s->cq_addr >> 32);      break;
        case R_CQ_SIZE:          result = s->cq_size;             break;
        case R_CQ_HEAD:          result = s->cq_head;             break;
        case R_CQ_TAIL:          result = s->cq_tail;             break;
        case R_COMPL_STATUS:     result = s->compl_status;        break;
        case R_COMPL_SEQ_LO:     result = (uint32_t)(s->compl_seq);          break;
        case R_COMPL_SEQ_HI:     result = (uint32_t)(s->compl_seq >> 32);    break;
        case R_IRQ_STATUS:       result = s->irq_status;          break;
        case R_IRQ_MASK:         result = s->irq_mask;            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "vijgpu: READ_REG from unknown offset 0x%x\n",
                          offset);
            s->compl_status = VIJGPU_COMPL_INVALID_ARG;
            return true;
        }
        /* Return result in compl_seq lower 32 bits */
        s->compl_seq    = (uint64_t)result;
        s->compl_status = VIJGPU_COMPL_SUCCESS;
        break;
    }

    case VIJGPU_CMD_FILL_VRAM: {
        /*
         * arg0 = offset into VRAM (bytes)
         * arg1 = length (bytes)
         * arg2 = fill byte value (low 8 bits)
         */
        uint64_t offset = arg0;
        uint64_t length = arg1;
        uint8_t  fill   = (uint8_t)arg2;
        uint8_t *vram;

        /* Strict overflow and bounds check */
        if (length == 0) {
            s->compl_status = VIJGPU_COMPL_SUCCESS;
            break;
        }
        if (offset >= s->vram_size ||
            length > s->vram_size ||
            offset > s->vram_size - length) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "vijgpu: FILL_VRAM out of bounds "
                          "(offset=0x%"PRIx64" len=0x%"PRIx64" vram=0x%"PRIx64")\n",
                          offset, length, s->vram_size);
            s->compl_status = VIJGPU_COMPL_OUT_OF_BOUNDS;
            break;
        }

        vram = memory_region_get_ram_ptr(&s->bar1_vram);
        memset(vram + offset, fill, (size_t)length);
        s->compl_status = VIJGPU_COMPL_SUCCESS;
        break;
    }

    case VIJGPU_CMD_COPY_VRAM: {
        /*
         * arg0 = source offset (bytes)
         * arg1 = destination offset (bytes)
         * arg2 = length (bytes)
         */
        uint64_t src = arg0;
        uint64_t dst = arg1;
        uint64_t len = arg2;
        uint8_t *vram;

        if (len == 0) {
            s->compl_status = VIJGPU_COMPL_SUCCESS;
            break;
        }

        /* Check source range */
        if (src >= s->vram_size ||
            len > s->vram_size ||
            src > s->vram_size - len) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "vijgpu: COPY_VRAM src out of bounds\n");
            s->compl_status = VIJGPU_COMPL_OUT_OF_BOUNDS;
            break;
        }
        /* Check destination range */
        if (dst >= s->vram_size ||
            len > s->vram_size ||
            dst > s->vram_size - len) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "vijgpu: COPY_VRAM dst out of bounds\n");
            s->compl_status = VIJGPU_COMPL_OUT_OF_BOUNDS;
            break;
        }

        vram = memory_region_get_ram_ptr(&s->bar1_vram);
        memmove(vram + dst, vram + src, (size_t)len);
        s->compl_status = VIJGPU_COMPL_SUCCESS;
        break;
    }

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "vijgpu: unknown command opcode 0x%x\n", opcode);
        s->compl_status = VIJGPU_COMPL_INVALID_CMD;
        break;
    }

    return true;
}

/*
 * vijgpu_process_commands — drain the command ring from head to tail.
 *
 * Called whenever the guest writes to CQ_TAIL (either BAR0 or BAR2).
 * Each command entry is read from guest memory using pci_dma_read so
 * that we use the correct DMA address space and endianness.
 *
 * We read the raw bytes and parse them manually to handle endianness.
 */
static void vijgpu_process_commands(VijGPUState *s)
{
    uint32_t tail;
    uint32_t processed = 0;
    bool     did_work = false;

    /* Validate queue configuration */
    if (s->cq_size == 0 || s->cq_size > VIJGPU_CQ_MAX_ENTRIES ||
        s->cq_addr == 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "vijgpu: command queue not configured\n");
        s->compl_status = VIJGPU_COMPL_QUEUE_ERROR;
        return;
    }

    tail = s->cq_tail;

    /* Drain from head up to (but not including) tail */
    while (s->cq_head != tail && processed < VIJGPU_CQ_MAX_PROCESS) {
        uint8_t  raw[VIJGPU_CMD_SIZE];
        uint32_t opcode, flags;
        uint64_t arg0, arg1, arg2, sequence;
        uint64_t entry_pa;

        /* Guest physical address of this entry */
        entry_pa = s->cq_addr +
                   (uint64_t)(s->cq_head % s->cq_size) * VIJGPU_CMD_SIZE;

        /* Read the command from guest memory.  Failure is non-fatal. */
        if (pci_dma_read(&s->pdev, entry_pa, raw, VIJGPU_CMD_SIZE) != 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "vijgpu: pci_dma_read failed for command "
                          "at 0x%"PRIx64"\n", entry_pa);
            s->compl_status = VIJGPU_COMPL_QUEUE_ERROR;
            s->cq_head = tail; /* skip to avoid retrying bad entry */
            break;
        }

        /* Parse little-endian command fields */
        opcode   = le32_to_cpu(*(uint32_t *)(raw +  0));
        flags    = le32_to_cpu(*(uint32_t *)(raw +  4));
        arg0     = le64_to_cpu(*(uint64_t *)(raw +  8));
        arg1     = le64_to_cpu(*(uint64_t *)(raw + 16));
        arg2     = le64_to_cpu(*(uint64_t *)(raw + 24));
        sequence = le64_to_cpu(*(uint64_t *)(raw + 32));

        /* Advance head before executing (so reset during exec is safe) */
        s->cq_head = (s->cq_head + 1) % s->cq_size;
        processed++;
        did_work = true;

        if (!vijgpu_execute_command(s, opcode, flags,
                                    arg0, arg1, arg2, sequence)) {
            break;
        }
    }

    if (did_work) {
        vijgpu_raise_irq(s, VIJGPU_IRQ_CMD_DONE);
    }
}

/* -----------------------------------------------------------------------
 * Device reset
 * --------------------------------------------------------------------- */

static void vijgpu_reset_device(VijGPUState *s)
{
    /* Lower any pending interrupt before clearing state */
    vijgpu_lower_irq(s);

    /* Command queue */
    s->cq_addr   = 0;
    s->cq_size   = 0;
    s->cq_head   = 0;
    s->cq_tail   = 0;

    /* Control / status */
    s->ctrl      = 0;
    s->status    = VIJGPU_STATUS_READY;

    /* Completion */
    s->compl_status = VIJGPU_COMPL_SUCCESS;
    s->compl_seq    = 0;

    /* Interrupts */
    s->irq_status = 0;
    s->irq_mask   = 0;

    /*
     * VRAM is NOT zeroed on reset.  The guest driver is responsible for
     * initializing any VRAM contents it cares about.  This avoids a
     * potentially expensive memset on large VRAM regions at reset time.
     */
}

/* -----------------------------------------------------------------------
 * BAR0 MMIO operations
 * --------------------------------------------------------------------- */

static uint64_t vijgpu_bar0_read(void *opaque, hwaddr addr, unsigned size)
{
    VijGPUState *s = opaque;

    switch (addr) {
    case R_DEVICE_ID:       return VIJGPU_DEVICE_ID_VALUE;
    case R_VERSION:         return VIJGPU_VERSION_VALUE;
    case R_STATUS:          return s->status;
    case R_CTRL:            return s->ctrl;
    case R_VRAM_SIZE_LO:    return (uint32_t)(s->vram_size);
    case R_VRAM_SIZE_HI:    return (uint32_t)(s->vram_size >> 32);
    case R_CQ_ADDR_LO:      return (uint32_t)(s->cq_addr);
    case R_CQ_ADDR_HI:      return (uint32_t)(s->cq_addr >> 32);
    case R_CQ_SIZE:         return s->cq_size;
    case R_CQ_HEAD:         return s->cq_head;
    case R_CQ_TAIL:         return s->cq_tail;
    case R_COMPL_STATUS:    return s->compl_status;
    case R_COMPL_SEQ_LO:    return (uint32_t)(s->compl_seq);
    case R_COMPL_SEQ_HI:    return (uint32_t)(s->compl_seq >> 32);
    case R_IRQ_STATUS:      return s->irq_status;
    case R_IRQ_MASK:        return s->irq_mask;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "vijgpu: BAR0 read from unknown offset 0x%"HWADDR_PRIx"\n",
                      addr);
        return 0;
    }
}

static void vijgpu_bar0_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    VijGPUState *s = opaque;
    uint32_t v = (uint32_t)val;

    switch (addr) {

    /* Read-only registers — silently ignore */
    case R_DEVICE_ID:
    case R_VERSION:
    case R_STATUS:
    case R_VRAM_SIZE_LO:
    case R_VRAM_SIZE_HI:
    case R_CQ_HEAD:
    case R_COMPL_STATUS:
    case R_COMPL_SEQ_LO:
    case R_COMPL_SEQ_HI:
        break;

    case R_CTRL:
        s->ctrl = v;
        break;

    case R_CQ_ADDR_LO:
        s->cq_addr = (s->cq_addr & 0xFFFFFFFF00000000ULL) | v;
        break;

    case R_CQ_ADDR_HI:
        s->cq_addr = (s->cq_addr & 0x00000000FFFFFFFFULL) |
                     ((uint64_t)v << 32);
        break;

    case R_CQ_SIZE:
        if (v == 0 || v > VIJGPU_CQ_MAX_ENTRIES) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "vijgpu: invalid CQ_SIZE %u\n", v);
        } else {
            s->cq_size = v;
        }
        break;

    case R_CQ_TAIL:
        s->cq_tail = v % (s->cq_size ? s->cq_size : 1);
        vijgpu_process_commands(s);
        break;

    case R_IRQ_STATUS:
        /* W1C: guest clears bits by writing 1 */
        s->irq_status &= ~v;
        vijgpu_update_irq(s);
        break;

    case R_IRQ_MASK:
        s->irq_mask = v;
        vijgpu_update_irq(s);
        break;

    case R_RESET:
        if (v == VIJGPU_RESET_KEY) {
            vijgpu_reset_device(s);
        }
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "vijgpu: BAR0 write to unknown offset 0x%"HWADDR_PRIx
                      " value 0x%x\n", addr, v);
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

/* -----------------------------------------------------------------------
 * BAR2 MMIO operations (command queue / doorbell mirror)
 * --------------------------------------------------------------------- */

static uint64_t vijgpu_bar2_read(void *opaque, hwaddr addr, unsigned size)
{
    VijGPUState *s = opaque;

    switch (addr) {
    case R2_CQ_SIZE:    return s->cq_size;
    case R2_CQ_HEAD:    return s->cq_head;
    case R2_CQ_TAIL:    return s->cq_tail;
    case R2_IRQ_STATUS: return s->irq_status;
    case R2_IRQ_MASK:   return s->irq_mask;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "vijgpu: BAR2 read from unknown offset 0x%"HWADDR_PRIx"\n",
                      addr);
        return 0;
    }
}

static void vijgpu_bar2_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    VijGPUState *s = opaque;
    uint32_t v = (uint32_t)val;

    switch (addr) {

    case R2_DOORBELL:
        /* Any write triggers re-processing of pending commands */
        vijgpu_process_commands(s);
        break;

    case R2_CQ_SIZE:
        if (v == 0 || v > VIJGPU_CQ_MAX_ENTRIES) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "vijgpu: invalid CQ_SIZE %u via BAR2\n", v);
        } else {
            s->cq_size = v;
        }
        break;

    case R2_CQ_TAIL:
        s->cq_tail = v % (s->cq_size ? s->cq_size : 1);
        vijgpu_process_commands(s);
        break;

    case R2_IRQ_STATUS:
        s->irq_status &= ~v;
        vijgpu_update_irq(s);
        break;

    case R2_IRQ_MASK:
        s->irq_mask = v;
        vijgpu_update_irq(s);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "vijgpu: BAR2 write to unknown offset 0x%"HWADDR_PRIx
                      " value 0x%x\n", addr, v);
        break;
    }
}

static const MemoryRegionOps vijgpu_bar2_ops = {
    .read  = vijgpu_bar2_read,
    .write = vijgpu_bar2_write,
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

/* -----------------------------------------------------------------------
 * QOM reset hook
 * --------------------------------------------------------------------- */

static void vijgpu_reset(DeviceState *dev)
{
    VijGPUState *s = VIJGPU_DEVICE(dev);
    vijgpu_reset_device(s);
}

/* -----------------------------------------------------------------------
 * PCI realize / unrealize
 * --------------------------------------------------------------------- */

static void vijgpu_realize(PCIDevice *pdev, Error **errp)
{
    VijGPUState *s = VIJGPU_DEVICE(pdev);

    /* Validate VRAM size */
    if (s->vram_size < VIJGPU_VRAM_MIN || s->vram_size > VIJGPU_VRAM_MAX) {
        error_setg(errp,
                   "vijgpu: vram_size 0x%"PRIx64" out of range "
                   "[0x%x, 0x%"PRIx64"]",
                   s->vram_size, VIJGPU_VRAM_MIN, VIJGPU_VRAM_MAX);
        return;
    }

    /* VRAM size must be a power of two for clean BAR alignment */
    s->vram_size = pow2ceil(s->vram_size);

    /* Enable INTx + MSI (1 vector).  Fallback to INTx if MSI unavailable. */
    pci_config_set_interrupt_pin(pdev->config, 1);
    msi_init(pdev, 0, 1, true, false, NULL); /* ignore error — INTx fallback */

    /* BAR0: control registers (MMIO) */
    memory_region_init_io(&s->bar0, OBJECT(s), &vijgpu_bar0_ops, s,
                          "vijgpu-regs", VIJGPU_BAR0_SIZE);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);

    /*
     * BAR1: virtual VRAM backed by host RAM.
     * PCI_BASE_ADDRESS_MEM_PREFETCH marks it as prefetchable so the guest
     * OS can map it with write-combining, which is appropriate for VRAM.
     */
    if (!memory_region_init_ram(&s->bar1_vram, OBJECT(s),
                                "vijgpu-vram", s->vram_size, errp)) {
        return;
    }
    pci_register_bar(pdev, 1,
                     PCI_BASE_ADDRESS_MEM_PREFETCH |
                     PCI_BASE_ADDRESS_SPACE_MEMORY,
                     &s->bar1_vram);

    /* BAR2: command queue / doorbell registers (MMIO) */
    memory_region_init_io(&s->bar2, OBJECT(s), &vijgpu_bar2_ops, s,
                          "vijgpu-cmdq", VIJGPU_BAR2_SIZE);
    pci_register_bar(pdev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar2);

    /* Initialize device state */
    vijgpu_reset_device(s);
}

static void vijgpu_uninit(PCIDevice *pdev)
{
    VijGPUState *s = VIJGPU_DEVICE(pdev);
    msi_uninit(pdev);
    vijgpu_lower_irq(s);
}

/* -----------------------------------------------------------------------
 * Device properties
 * --------------------------------------------------------------------- */

static const Property vijgpu_properties[] = {
    DEFINE_PROP_UINT64("vram_size", VijGPUState, vram_size, VIJGPU_VRAM_DEFAULT),
};

/* -----------------------------------------------------------------------
 * Class and type registration
 * --------------------------------------------------------------------- */

static void vijgpu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass   *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);

    pc->realize   = vijgpu_realize;
    pc->exit      = vijgpu_uninit;
    pc->vendor_id = VIJGPU_VENDOR_ID;
    pc->device_id = VIJGPU_DEVICE_ID;
    pc->revision  = VIJGPU_REVISION;
    pc->class_id  = PCI_CLASS_DISPLAY_OTHER;

    device_class_set_legacy_reset(dc, vijgpu_reset);
    device_class_set_props(dc, vijgpu_properties);
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
