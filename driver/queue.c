/*
 * queue.c - vijGPU command ring management
 *
 * Allocates a contiguous DMA buffer for the command ring, programs the
 * device with the physical address, and provides submit/wait helpers.
 *
 * The ring is a simple FIFO.  The driver owns Tail; the device owns Head.
 * Submission writes a command at Tail, advances Tail, then writes the
 * new Tail to BAR2 CQ_TAIL (which triggers device processing).
 *
 * Completion is interrupt-driven: the ISR/DPC records the last completed
 * sequence and signals Ring.CompletionEvent.
 */

#include "device.h"

/* -----------------------------------------------------------------------
 * VijGpuQueueInit
 * Allocate DMA ring buffer and program the device.
 * --------------------------------------------------------------------- */
NTSTATUS
VijGpuQueueInit(PDEVICE_CONTEXT Ctx)
{
    PVIJGPU_RING        ring = &Ctx->Ring;
    SIZE_T              ringBytes;
    PHYSICAL_ADDRESS    highAddr, lowAddr, boundaryAddr;

    ringBytes = (SIZE_T)VIJGPU_CQ_ENTRIES * VIJGPU_CMD_SIZE;

    /* Allocate contiguous non-paged memory accessible by the device.
     * We use MmAllocateContiguousMemory for simplicity.  For production
     * use WDF DMA APIs; here we just need a page-aligned physical buffer. */
    highAddr.QuadPart  = 0xFFFFFFFFFFFFFFFFLL; /* no upper limit */
    lowAddr.QuadPart   = 0;
    boundaryAddr.QuadPart = 0;

    ring->Entries = (PVIJGPU_COMMAND)MmAllocateContiguousMemorySpecifyCache(
                        ringBytes,
                        lowAddr,
                        highAddr,
                        boundaryAddr,
                        MmNonCached);

    if (ring->Entries == NULL) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "vijgpu: failed to allocate ring buffer (%Iu bytes)\n",
                   ringBytes));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(ring->Entries, ringBytes);

    ring->PhysicalAddr  = MmGetPhysicalAddress(ring->Entries);
    ring->AllocSize     = ringBytes;
    ring->EntryCount    = VIJGPU_CQ_ENTRIES;
    ring->Head          = 0;
    ring->Tail          = 0;
    ring->NextSequence  = 1;
    ring->LastCompletedSeq = 0;
    ring->LastComplStatus  = VIJGPU_COMPL_SUCCESS;

    KeInitializeEvent(&ring->CompletionEvent, SynchronizationEvent, FALSE);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "vijgpu: ring PA=0x%llx entries=%u (%Iu bytes)\n",
               ring->PhysicalAddr.QuadPart,
               ring->EntryCount,
               ringBytes));

    /* Program device with ring address and size */
    VijGpuWriteReg32(Ctx->Bar0, VIJGPU_R_CQ_ADDR_LO,
                     (ULONG)(ring->PhysicalAddr.QuadPart & 0xFFFFFFFF));
    VijGpuWriteReg32(Ctx->Bar0, VIJGPU_R_CQ_ADDR_HI,
                     (ULONG)(ring->PhysicalAddr.QuadPart >> 32));
    VijGpuWriteReg32(Ctx->Bar0, VIJGPU_R_CQ_SIZE, ring->EntryCount);

    /* Also set via BAR2 for redundancy */
    VijGpuWriteReg32(Ctx->Bar2, VIJGPU_R2_CQ_SIZE, ring->EntryCount);

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "vijgpu: command ring initialized\n"));
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * VijGpuQueueFree
 * --------------------------------------------------------------------- */
VOID
VijGpuQueueFree(PDEVICE_CONTEXT Ctx)
{
    PVIJGPU_RING ring = &Ctx->Ring;

    if (ring->Entries) {
        MmFreeContiguousMemory(ring->Entries);
        ring->Entries = NULL;
    }
}

/* -----------------------------------------------------------------------
 * VijGpuSubmitCommand
 * Write one command to the ring and ring the doorbell.
 *
 * Returns STATUS_SUCCESS and the assigned sequence number in *OutSequence.
 * Caller must hold no spinlock (this function acquires RingLock).
 * --------------------------------------------------------------------- */
NTSTATUS
VijGpuSubmitCommand(
    _In_  PDEVICE_CONTEXT Ctx,
    _In_  ULONG           Opcode,
    _In_  ULONGLONG       Arg0,
    _In_  ULONGLONG       Arg1,
    _In_  ULONGLONG       Arg2,
    _Out_ PULONGLONG      OutSequence
    )
{
    PVIJGPU_RING    ring = &Ctx->Ring;
    PVIJGPU_COMMAND entry;
    ULONG           nextTail;
    ULONGLONG       seq;
    KIRQL           oldIrql;

    if (ring->Entries == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    WdfSpinLockAcquire(Ctx->RingLock);

    /* Check for ring full: next Tail must not equal Head */
    nextTail = (ring->Tail + 1) % ring->EntryCount;
    if (nextTail == ring->Head) {
        WdfSpinLockRelease(Ctx->RingLock);
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "vijgpu: command ring full\n"));
        return STATUS_DEVICE_BUSY;
    }

    seq = ring->NextSequence++;

    /* Write command into the ring */
    entry = &ring->Entries[ring->Tail];
    entry->Opcode   = Opcode;
    entry->Flags    = 0;
    entry->Arg0     = Arg0;
    entry->Arg1     = Arg1;
    entry->Arg2     = Arg2;
    entry->Sequence = seq;

    /* Ensure the command write is visible before we advance the tail */
    KeMemoryBarrier();

    ring->Tail = nextTail;

    /* Write new tail to device via BAR2 to trigger processing */
    VijGpuWriteReg32(Ctx->Bar2, VIJGPU_R2_CQ_TAIL, ring->Tail);

    WdfSpinLockRelease(Ctx->RingLock);

    *OutSequence = seq;

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "vijgpu: submitted cmd opcode=%u seq=%llu tail=%u\n",
               Opcode, seq, ring->Tail));

    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * VijGpuWaitCompletion
 * Wait until the device has completed a command with the given sequence
 * number, or until the timeout expires.
 *
 * TimeoutMs = 0 means poll once without waiting.
 * --------------------------------------------------------------------- */
NTSTATUS
VijGpuWaitCompletion(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONGLONG       Sequence,
    _In_ ULONG           TimeoutMs
    )
{
    PVIJGPU_RING    ring = &Ctx->Ring;
    LARGE_INTEGER   timeout;
    NTSTATUS        status;
    ULONG           complStatus;

    /* Fast path: already done */
    if (ring->LastCompletedSeq >= Sequence) {
        complStatus = ring->LastComplStatus;
        if (complStatus == VIJGPU_COMPL_SUCCESS) {
            return STATUS_SUCCESS;
        }
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "vijgpu: seq %llu completed with error %u\n",
                   Sequence, complStatus));
        return STATUS_UNSUCCESSFUL;
    }

    if (TimeoutMs == 0) {
        return STATUS_TIMEOUT;
    }

    /* Wait for interrupt-driven completion event */
    timeout.QuadPart = -(LONGLONG)TimeoutMs * 10000LL; /* relative, 100ns units */

    status = KeWaitForSingleObject(
                 &ring->CompletionEvent,
                 Executive,
                 KernelMode,
                 FALSE,
                 &timeout);

    if (status == STATUS_TIMEOUT) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "vijgpu: timeout waiting for seq %llu\n", Sequence));
        return STATUS_TIMEOUT;
    }

    /* Check completion status */
    if (ring->LastCompletedSeq >= Sequence) {
        complStatus = ring->LastComplStatus;
        if (complStatus == VIJGPU_COMPL_SUCCESS) {
            return STATUS_SUCCESS;
        }
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "vijgpu: seq %llu completed with error %u\n",
                   Sequence, complStatus));
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_UNSUCCESSFUL;
}
