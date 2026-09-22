/*
 * vijgpu.h - Hardware ABI definitions for vijGPU virtual GPU
 *
 * This header defines the exact hardware interface between the QEMU
 * vijGPU device and the Windows kernel driver.  It must stay in sync
 * with hw/misc/vijgpu.c in the QEMU source tree.
 *
 * DO NOT change values in this file without updating vijgpu.c as well.
 */

#pragma once

#include <ntddk.h>

/* -----------------------------------------------------------------------
 * PCI identity
 * --------------------------------------------------------------------- */
#define VIJGPU_PCI_VENDOR_ID    0x1234
#define VIJGPU_PCI_DEVICE_ID    0x1200
#define VIJGPU_PCI_REVISION     0x01

/* -----------------------------------------------------------------------
 * BAR indices
 * --------------------------------------------------------------------- */
#define VIJGPU_BAR0_INDEX       0   /* Control/status registers  (4 KiB) */
#define VIJGPU_BAR1_INDEX       1   /* Virtual VRAM              (default 256 MiB) */
#define VIJGPU_BAR2_INDEX       2   /* Command queue / doorbell  (4 KiB) */

#define VIJGPU_BAR0_SIZE        0x1000
#define VIJGPU_BAR2_SIZE        0x1000
#define VIJGPU_VRAM_DEFAULT     (256ULL * 1024 * 1024)

/* -----------------------------------------------------------------------
 * BAR0 register offsets  (32-bit registers, little-endian)
 * --------------------------------------------------------------------- */
#define VIJGPU_R_DEVICE_ID      0x00    /* RO: 0x56494A47 ("VIJG") */
#define VIJGPU_R_VERSION        0x04    /* RO: 0x00020000 */
#define VIJGPU_R_STATUS         0x08    /* RO: device status flags */
#define VIJGPU_R_CTRL           0x0C    /* RW: control flags */
#define VIJGPU_R_VRAM_SIZE_LO   0x10    /* RO: VRAM size low 32 bits */
#define VIJGPU_R_VRAM_SIZE_HI   0x14    /* RO: VRAM size high 32 bits */
#define VIJGPU_R_CQ_ADDR_LO     0x20    /* RW: command ring PA low */
#define VIJGPU_R_CQ_ADDR_HI     0x24    /* RW: command ring PA high */
#define VIJGPU_R_CQ_SIZE        0x28    /* RW: ring entry count */
#define VIJGPU_R_CQ_HEAD        0x2C    /* RO: device head index */
#define VIJGPU_R_CQ_TAIL        0x30    /* RW: tail (triggers processing) */
#define VIJGPU_R_COMPL_STATUS   0x34    /* RO: last completion status */
#define VIJGPU_R_COMPL_SEQ_LO   0x38    /* RO: last completed sequence low */
#define VIJGPU_R_COMPL_SEQ_HI   0x3C    /* RO: last completed sequence high */
#define VIJGPU_R_IRQ_STATUS     0x40    /* RO/W1C: pending interrupt flags */
#define VIJGPU_R_IRQ_MASK       0x44    /* RW: interrupt enable mask */
#define VIJGPU_R_RESET          0x48    /* WO: write VIJGPU_RESET_KEY to reset */

/* -----------------------------------------------------------------------
 * BAR2 register offsets
 * --------------------------------------------------------------------- */
#define VIJGPU_R2_DOORBELL      0x00    /* WO: ring doorbell */
#define VIJGPU_R2_CQ_SIZE       0x04    /* RW: mirrors BAR0 CQ_SIZE */
#define VIJGPU_R2_CQ_HEAD       0x08    /* RO: mirrors BAR0 CQ_HEAD */
#define VIJGPU_R2_CQ_TAIL       0x0C    /* RW: mirrors BAR0 CQ_TAIL */
#define VIJGPU_R2_IRQ_STATUS    0x10    /* RO/W1C: mirrors BAR0 IRQ_STATUS */
#define VIJGPU_R2_IRQ_MASK      0x14    /* RW: mirrors BAR0 IRQ_MASK */

/* -----------------------------------------------------------------------
 * Register values and flags
 * --------------------------------------------------------------------- */
#define VIJGPU_DEVICE_ID_VALUE  0x56494A47UL
#define VIJGPU_VERSION_VALUE    0x00020000UL
#define VIJGPU_RESET_KEY        0xDEADBEEFUL

#define VIJGPU_STATUS_READY     (1UL << 0)
#define VIJGPU_STATUS_BUSY      (1UL << 1)
#define VIJGPU_STATUS_ERROR     (1UL << 2)

#define VIJGPU_CTRL_IRQ_ENABLE  (1UL << 0)

#define VIJGPU_IRQ_CMD_DONE     (1UL << 0)

#define VIJGPU_COMPL_SUCCESS        0UL
#define VIJGPU_COMPL_INVALID_CMD    1UL
#define VIJGPU_COMPL_INVALID_ARG    2UL
#define VIJGPU_COMPL_OUT_OF_BOUNDS  3UL
#define VIJGPU_COMPL_QUEUE_ERROR    4UL

/* -----------------------------------------------------------------------
 * Command opcodes
 * --------------------------------------------------------------------- */
#define VIJGPU_CMD_NOP          0UL
#define VIJGPU_CMD_WRITE_REG    1UL
#define VIJGPU_CMD_READ_REG     2UL
#define VIJGPU_CMD_FILL_VRAM    3UL
#define VIJGPU_CMD_COPY_VRAM    4UL

/* -----------------------------------------------------------------------
 * Command entry (40 bytes, little-endian)
 * --------------------------------------------------------------------- */
#define VIJGPU_CMD_SIZE     40
#define VIJGPU_CQ_ENTRIES   64
#define VIJGPU_CQ_MAX       4096

#pragma pack(push, 1)
typedef struct _VIJGPU_COMMAND {
    ULONG       Opcode;
    ULONG       Flags;      /* reserved, must be 0 */
    ULONGLONG   Arg0;
    ULONGLONG   Arg1;
    ULONGLONG   Arg2;
    ULONGLONG   Sequence;
} VIJGPU_COMMAND, *PVIJGPU_COMMAND;
#pragma pack(pop)

C_ASSERT(sizeof(VIJGPU_COMMAND) == VIJGPU_CMD_SIZE);
