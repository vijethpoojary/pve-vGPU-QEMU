/*
 * device.c - vijGPU KMDF device lifecycle
 *
 * Handles PnP events: PrepareHardware, ReleaseHardware, D0Entry, D0Exit.
 * Maps BAR0/BAR1/BAR2, reads device identity, initializes the command
 * queue and interrupt.
 */

#include "device.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, VijGpuCreateDevice)
#pragma alloc_text(PAGE, VijGpuEvtDevicePrepareHardware)
#pragma alloc_text(PAGE, VijGpuEvtDeviceReleaseHardware)
#endif

/* -----------------------------------------------------------------------
 * VijGpuCreateDevice
 * Register all WDF callbacks and create the device object.
 * --------------------------------------------------------------------- */
NTSTATUS
VijGpuCreateDevice(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS                        status;
    WDF_PNPPOWER_EVENT_CALLBACKS    pnpPowerCallbacks;
    WDF_OBJECT_ATTRIBUTES           deviceAttributes;
    WDFDEVICE                       device;
    PDEVICE_CONTEXT                 ctx;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(Driver);

    /* Set up PnP / power callbacks */
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = VijGpuEvtDevicePrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = VijGpuEvtDeviceReleaseHardware;
    pnpPowerCallbacks.EvtDeviceD0Entry         = VijGpuEvtDeviceD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit          = VijGpuEvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    /* Allocate per-device context */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "vijgpu: WdfDeviceCreate failed 0x%x\n", status));
        return status;
    }

    ctx = GetDeviceContext(device);
    RtlZeroMemory(ctx, sizeof(*ctx));

    /* Create ring spinlock */
    status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &ctx->RingLock);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "vijgpu: WdfSpinLockCreate failed 0x%x\n", status));
        return status;
    }

    /* Create interrupt object */
    status = VijGpuInterruptCreate(device, ctx);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "vijgpu: VijGpuInterruptCreate failed 0x%x\n", status));
        return status;
    }

    /* Create IOCTL queue */
    status = VijGpuIoQueueCreate(device);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "vijgpu: VijGpuIoQueueCreate failed 0x%x\n", status));
        return status;
    }

    /* Expose a device interface so user-mode can open the device */
    status = WdfDeviceCreateDeviceInterface(device,
                 &GUID_DEVINTERFACE_VIJGPU, NULL);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "vijgpu: WdfDeviceCreateDeviceInterface failed 0x%x\n",
                   status));
        return status;
    }

    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * VijGpuEvtDevicePrepareHardware
 * Map BARs, validate identity, initialize command queue.
 * --------------------------------------------------------------------- */
NTSTATUS
VijGpuEvtDevicePrepareHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    NTSTATUS            status = STATUS_SUCCESS;
    PDEVICE_CONTEXT     ctx = GetDeviceContext(Device);
    ULONG               i;
    ULONG               barCount = 0;
    ULONG               deviceId, version, statusReg;

    PAGED_CODE();
    UNREFERENCED_PARAMETER(ResourcesRaw);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "vijgpu: PrepareHardware\n"));

    /* Walk the translated resource list and map the three memory BARs */
    for (i = 0; i < WdfCmResourceListGetCount(ResourcesTranslated); i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR res =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, i);

        if (res->Type != CmResourceTypeMemory) {
            continue;
        }

        switch (barCount) {
        case 0: /* BAR0 — control registers */
            ctx->Bar0 = MmMapIoSpaceEx(
                            res->u.Memory.Start,
                            VIJGPU_BAR0_SIZE,
                            PAGE_READWRITE | PAGE_NOCACHE);
            if (ctx->Bar0 == NULL) {
                KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                           "vijgpu: failed to map BAR0\n"));
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                       "vijgpu: BAR0 mapped at %p (PA 0x%llx)\n",
                       ctx->Bar0, res->u.Memory.Start.QuadPart));
            break;

        case 1: /* BAR1 — virtual VRAM */
            /*
             * Map the full VRAM BAR.  We read the actual size from the
             * device after BAR0 is mapped.  Use the resource length here.
             */
            ctx->Bar1 = MmMapIoSpaceEx(
                            res->u.Memory.Start,
                            res->u.Memory.Length,
                            PAGE_READWRITE | PAGE_WRITECOMBINE);
            if (ctx->Bar1 == NULL) {
                KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                           "vijgpu: failed to map BAR1 (VRAM)\n"));
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                       "vijgpu: BAR1 VRAM mapped at %p "
                       "(PA 0x%llx, len 0x%x)\n",
                       ctx->Bar1,
                       res->u.Memory.Start.QuadPart,
                       res->u.Memory.Length));
            break;

        case 2: /* BAR2 — command queue / doorbell */
            ctx->Bar2 = MmMapIoSpaceEx(
                            res->u.Memory.Start,
                            VIJGPU_BAR2_SIZE,
                            PAGE_READWRITE | PAGE_NOCACHE);
            if (ctx->Bar2 == NULL) {
                KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                           "vijgpu: failed to map BAR2\n"));
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                       "vijgpu: BAR2 mapped at %p (PA 0x%llx)\n",
                       ctx->Bar2, res->u.Memory.Start.QuadPart));
            break;

        default:
            break;
        }

        barCount++;
        if (barCount >= 3) {
            break;
        }
    }

    if (barCount < 3) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "vijgpu: only found %u BARs, expected 3\n", barCount));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* Validate device identity */
    deviceId  = VijGpuReadReg32(ctx->Bar0, VIJGPU_R_DEVICE_ID);
    version   = VijGpuReadReg32(ctx->Bar0, VIJGPU_R_VERSION);
    statusReg = VijGpuReadReg32(ctx->Bar0, VIJGPU_R_STATUS);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "vijgpu: DEVICE_ID=0x%08x VERSION=0x%08x STATUS=0x%08x\n",
               deviceId, version, statusReg));

    if (deviceId != VIJGPU_DEVICE_ID_VALUE) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "vijgpu: unexpected DEVICE_ID 0x%08x (expected 0x%08x)\n",
                   deviceId, VIJGPU_DEVICE_ID_VALUE));
        return STATUS_DEVICE_DATA_ERROR;
    }

    /* Read VRAM size from device */
    {
        ULONG lo = VijGpuReadReg32(ctx->Bar0, VIJGPU_R_VRAM_SIZE_LO);
        ULONG hi = VijGpuReadReg32(ctx->Bar0, VIJGPU_R_VRAM_SIZE_HI);
        ctx->VramSize = ((ULONGLONG)hi << 32) | lo;
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                   "vijgpu: VRAM size = 0x%llx bytes (%llu MiB)\n",
                   ctx->VramSize, ctx->VramSize / (1024 * 1024)));
    }

    if (ctx->VramSize == 0) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "vijgpu: VRAM size is 0\n"));
        return STATUS_DEVICE_DATA_ERROR;
    }

    /* Initialize command queue */
    status = VijGpuQueueInit(ctx);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "vijgpu: VijGpuQueueInit failed 0x%x\n", status));
        return status;
    }

    /* Initialize device: enable interrupts */
    status = VijGpuInitDevice(ctx);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "vijgpu: VijGpuInitDevice failed 0x%x\n", status));
        return status;
    }

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "vijgpu: PrepareHardware complete\n"));
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * VijGpuEvtDeviceReleaseHardware
 * Unmap BARs and free DMA ring.
 * --------------------------------------------------------------------- */
NTSTATUS
VijGpuEvtDeviceReleaseHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);
    PAGED_CODE();
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "vijgpu: ReleaseHardware\n"));

    /* Disable interrupts before unmapping */
    if (ctx->Bar0) {
        VijGpuWriteReg32(ctx->Bar0, VIJGPU_R_IRQ_MASK, 0);
    }

    VijGpuQueueFree(ctx);

    if (ctx->Bar2) {
        MmUnmapIoSpace(ctx->Bar2, VIJGPU_BAR2_SIZE);
        ctx->Bar2 = NULL;
    }
    if (ctx->Bar1 && ctx->VramSize > 0) {
        MmUnmapIoSpace(ctx->Bar1, (SIZE_T)ctx->VramSize);
        ctx->Bar1 = NULL;
    }
    if (ctx->Bar0) {
        MmUnmapIoSpace(ctx->Bar0, VIJGPU_BAR0_SIZE);
        ctx->Bar0 = NULL;
    }

    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * VijGpuEvtDeviceD0Entry - device is entering D0 (powered)
 * --------------------------------------------------------------------- */
NTSTATUS
VijGpuEvtDeviceD0Entry(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);
    UNREFERENCED_PARAMETER(PreviousState);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "vijgpu: D0Entry\n"));

    /* Re-enable interrupt mask after power-on */
    if (ctx->Bar0) {
        VijGpuWriteReg32(ctx->Bar0, VIJGPU_R_IRQ_MASK, VIJGPU_IRQ_CMD_DONE);
    }

    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * VijGpuEvtDeviceD0Exit - device is leaving D0
 * --------------------------------------------------------------------- */
NTSTATUS
VijGpuEvtDeviceD0Exit(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);
    UNREFERENCED_PARAMETER(TargetState);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "vijgpu: D0Exit\n"));

    if (ctx->Bar0) {
        VijGpuWriteReg32(ctx->Bar0, VIJGPU_R_IRQ_MASK, 0);
    }

    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * VijGpuInitDevice - enable interrupts and verify device is ready
 * --------------------------------------------------------------------- */
NTSTATUS
VijGpuInitDevice(PDEVICE_CONTEXT Ctx)
{
    ULONG status;

    /* Enable CMD_DONE interrupt */
    VijGpuWriteReg32(Ctx->Bar0, VIJGPU_R_IRQ_MASK, VIJGPU_IRQ_CMD_DONE);

    /* Verify status shows READY */
    status = VijGpuReadReg32(Ctx->Bar0, VIJGPU_R_STATUS);
    if (!(status & VIJGPU_STATUS_READY)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "vijgpu: device not ready, STATUS=0x%08x\n", status));
        return STATUS_DEVICE_NOT_READY;
    }

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "vijgpu: device is ready\n"));
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * VijGpuResetDevice - write reset key, reinitialize software state
 * --------------------------------------------------------------------- */
VOID
VijGpuResetDevice(PDEVICE_CONTEXT Ctx)
{
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "vijgpu: resetting device\n"));

    /* Disable interrupts */
    VijGpuWriteReg32(Ctx->Bar0, VIJGPU_R_IRQ_MASK, 0);

    /* Write reset key */
    VijGpuWriteReg32(Ctx->Bar0, VIJGPU_R_RESET, (ULONG)VIJGPU_RESET_KEY);

    /* Reset software ring state */
    if (Ctx->Ring.Entries) {
        RtlZeroMemory(Ctx->Ring.Entries,
                      Ctx->Ring.EntryCount * VIJGPU_CMD_SIZE);
        Ctx->Ring.Head = 0;
        Ctx->Ring.Tail = 0;
        Ctx->Ring.NextSequence = 1;
        Ctx->Ring.LastCompletedSeq = 0;
        Ctx->Ring.LastComplStatus  = VIJGPU_COMPL_SUCCESS;
    }

    /* Re-arm queue on device */
    if (Ctx->Ring.Entries) {
        PHYSICAL_ADDRESS pa = Ctx->Ring.PhysicalAddr;
        VijGpuWriteReg32(Ctx->Bar0, VIJGPU_R_CQ_ADDR_LO,
                         (ULONG)(pa.QuadPart & 0xFFFFFFFF));
        VijGpuWriteReg32(Ctx->Bar0, VIJGPU_R_CQ_ADDR_HI,
                         (ULONG)(pa.QuadPart >> 32));
        VijGpuWriteReg32(Ctx->Bar0, VIJGPU_R_CQ_SIZE,
                         Ctx->Ring.EntryCount);
    }

    /* Re-enable interrupt */
    VijGpuWriteReg32(Ctx->Bar0, VIJGPU_R_IRQ_MASK, VIJGPU_IRQ_CMD_DONE);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "vijgpu: reset complete\n"));
}
