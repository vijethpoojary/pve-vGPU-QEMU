/*
 * interrupt.c - vijGPU interrupt handling (ISR + DPC)
 *
 * The device raises an interrupt when command(s) complete.
 * IRQ_STATUS bit 0 (CMD_DONE) is set by the device.
 * The guest driver acknowledges by writing 1 to IRQ_STATUS (W1C).
 *
 * ISR: runs at device IRQL.  Checks IRQ_STATUS, clears it, schedules DPC.
 * DPC: runs at DISPATCH_LEVEL.  Reads COMPL_STATUS/SEQ, signals event.
 */

#include "device.h"

/* -----------------------------------------------------------------------
 * VijGpuInterruptCreate
 * Register ISR and DPC callbacks with KMDF.
 * --------------------------------------------------------------------- */
NTSTATUS
VijGpuInterruptCreate(WDFDEVICE Device, PDEVICE_CONTEXT Ctx)
{
    WDF_INTERRUPT_CONFIG    intConfig;
    NTSTATUS                status;

    UNREFERENCED_PARAMETER(Ctx);

    WDF_INTERRUPT_CONFIG_INIT(&intConfig,
                               VijGpuInterruptIsr,
                               VijGpuInterruptDpc);

    status = WdfInterruptCreate(Device,
                                &intConfig,
                                WDF_NO_OBJECT_ATTRIBUTES,
                                &Ctx->Interrupt);

    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "vijgpu: WdfInterruptCreate failed 0x%x\n", status));
    }

    return status;
}

/* -----------------------------------------------------------------------
 * VijGpuInterruptIsr
 * Runs at device IRQL.  Must be very fast.
 *
 * 1. Read IRQ_STATUS via BAR2.
 * 2. If CMD_DONE is set, clear it (W1C) and schedule DPC.
 * 3. Return TRUE to claim the interrupt.
 * --------------------------------------------------------------------- */
BOOLEAN
VijGpuInterruptIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG        MessageID
    )
{
    WDFDEVICE       device = WdfInterruptGetDevice(Interrupt);
    PDEVICE_CONTEXT ctx    = GetDeviceContext(device);
    ULONG           irqStatus;

    UNREFERENCED_PARAMETER(MessageID);

    if (ctx->Bar2 == NULL) {
        return FALSE;
    }

    irqStatus = VijGpuReadReg32(ctx->Bar2, VIJGPU_R2_IRQ_STATUS);

    if (!(irqStatus & VIJGPU_IRQ_CMD_DONE)) {
        return FALSE; /* not our interrupt */
    }

    /* Clear the interrupt bit (W1C) */
    VijGpuWriteReg32(ctx->Bar2, VIJGPU_R2_IRQ_STATUS, VIJGPU_IRQ_CMD_DONE);

    /* Schedule DPC to read completion state at DISPATCH_LEVEL */
    WdfInterruptQueueDpcForIsr(Interrupt);

    return TRUE;
}

/* -----------------------------------------------------------------------
 * VijGpuInterruptDpc
 * Runs at DISPATCH_LEVEL.
 *
 * Reads COMPL_STATUS and COMPL_SEQ from BAR0, updates the ring tracking
 * fields, and signals the completion event so any waiter can wake up.
 * --------------------------------------------------------------------- */
VOID
VijGpuInterruptDpc(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT    AssociatedObject
    )
{
    WDFDEVICE       device = WdfInterruptGetDevice(Interrupt);
    PDEVICE_CONTEXT ctx    = GetDeviceContext(device);
    PVIJGPU_RING    ring   = &ctx->Ring;
    ULONG           complStatus;
    ULONG           seqLo, seqHi;
    ULONGLONG       seq;

    UNREFERENCED_PARAMETER(AssociatedObject);

    if (ctx->Bar0 == NULL) {
        return;
    }

    /* Read completion state */
    complStatus = VijGpuReadReg32(ctx->Bar0, VIJGPU_R_COMPL_STATUS);
    seqLo       = VijGpuReadReg32(ctx->Bar0, VIJGPU_R_COMPL_SEQ_LO);
    seqHi       = VijGpuReadReg32(ctx->Bar0, VIJGPU_R_COMPL_SEQ_HI);
    seq         = ((ULONGLONG)seqHi << 32) | seqLo;

    /* Update device head from BAR2 */
    ring->Head = VijGpuReadReg32(ctx->Bar2, VIJGPU_R2_CQ_HEAD);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "vijgpu: DPC: complStatus=%u seq=%llu head=%u\n",
               complStatus, seq, ring->Head));

    ring->LastComplStatus  = complStatus;
    ring->LastCompletedSeq = seq;

    /* Wake any thread waiting in VijGpuWaitCompletion */
    KeSetEvent(&ring->CompletionEvent, IO_NO_INCREMENT, FALSE);
}
