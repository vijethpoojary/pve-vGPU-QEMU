/*
 * device.h - vijGPU KMDF device context and declarations
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <wdmguid.h>
#include "vijgpu.h"

/* Device interface GUID for user-mode access
 * {A1B2C3D4-E5F6-7890-ABCD-EF1234567890} */
DEFINE_GUID(GUID_DEVINTERFACE_VIJGPU,
    0xa1b2c3d4, 0xe5f6, 0x7890,
    0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90);

/* -----------------------------------------------------------------------
 * Command queue ring (driver-side, allocated as contiguous DMA memory)
 * --------------------------------------------------------------------- */
typedef struct _VIJGPU_RING {
    PVIJGPU_COMMAND     Entries;        /* pointer to ring buffer (virtual) */
    PHYSICAL_ADDRESS    PhysicalAddr;   /* physical address for device */
    SIZE_T              AllocSize;      /* total allocation size in bytes */
    ULONG               EntryCount;     /* number of entries */
    ULONG               Head;           /* software head (mirrors device) */
    ULONG               Tail;           /* software tail */
    ULONGLONG           NextSequence;   /* monotonically increasing */
    ULONGLONG           LastCompletedSeq; /* last confirmed by interrupt */
    ULONG               LastComplStatus;  /* last COMPL_STATUS value */
    KEVENT              CompletionEvent; /* signaled on interrupt */
} VIJGPU_RING, *PVIJGPU_RING;

/* -----------------------------------------------------------------------
 * Per-device context
 * --------------------------------------------------------------------- */
typedef struct _DEVICE_CONTEXT {
    /* Mapped BAR pointers */
    PVOID               Bar0;           /* control registers (4 KiB) */
    PVOID               Bar1;           /* virtual VRAM */
    PVOID               Bar2;           /* command queue / doorbell */

    /* VRAM properties (read from device at startup) */
    ULONGLONG           VramSize;       /* bytes */

    /* Command ring */
    VIJGPU_RING         Ring;

    /* Interrupt object */
    WDFINTERRUPT        Interrupt;

    /* WDF memory objects for BAR mappings */
    WDFCMRESLIST        ResourcesTranslated;

    /* Spinlock protecting ring head/tail updates */
    WDFSPINLOCK         RingLock;

} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

/* -----------------------------------------------------------------------
 * Inline register accessors
 * Always 32-bit little-endian.  The device is little-endian; x86 Windows
 * is also little-endian, so no byte-swap is needed.
 * --------------------------------------------------------------------- */
static __forceinline ULONG
VijGpuReadReg32(PVOID Base, ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)((PUCHAR)Base + Offset));
}

static __forceinline VOID
VijGpuWriteReg32(PVOID Base, ULONG Offset, ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)Base + Offset), Value);
}

/* -----------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------- */

/* driver.c */
DRIVER_INITIALIZE       DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD VijGpuEvtDeviceAdd;

/* device.c */
NTSTATUS VijGpuCreateDevice(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit);
EVT_WDF_DEVICE_PREPARE_HARDWARE VijGpuEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE VijGpuEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY         VijGpuEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT          VijGpuEvtDeviceD0Exit;

NTSTATUS VijGpuInitDevice(PDEVICE_CONTEXT Ctx);
VOID     VijGpuResetDevice(PDEVICE_CONTEXT Ctx);

/* queue.c */
NTSTATUS VijGpuQueueInit(PDEVICE_CONTEXT Ctx);
VOID     VijGpuQueueFree(PDEVICE_CONTEXT Ctx);
NTSTATUS VijGpuSubmitCommand(PDEVICE_CONTEXT Ctx,
                             ULONG Opcode,
                             ULONGLONG Arg0,
                             ULONGLONG Arg1,
                             ULONGLONG Arg2,
                             PULONGLONG OutSequence);
NTSTATUS VijGpuWaitCompletion(PDEVICE_CONTEXT Ctx,
                               ULONGLONG Sequence,
                               ULONG TimeoutMs);

/* interrupt.c */
NTSTATUS VijGpuInterruptCreate(WDFDEVICE Device, PDEVICE_CONTEXT Ctx);
EVT_WDF_INTERRUPT_ISR  VijGpuInterruptIsr;
EVT_WDF_INTERRUPT_DPC  VijGpuInterruptDpc;

/* vram.c */
NTSTATUS VijGpuVramInit(PDEVICE_CONTEXT Ctx);
VOID     VijGpuVramRelease(PDEVICE_CONTEXT Ctx);

/* ioctl.c */
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL VijGpuEvtIoDeviceControl;
NTSTATUS VijGpuIoQueueCreate(WDFDEVICE Device);
