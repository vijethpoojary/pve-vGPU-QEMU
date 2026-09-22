/*
 * ioctl.c - vijGPU IOCTL handler
 *
 * Provides the IOCTL interface between user-mode (vijgpu_test.exe) and
 * the kernel driver.  Each IOCTL submits a command to the virtual GPU
 * and returns the result.
 *
 * IOCTL codes and structures are defined in public.h.
 */

#include "device.h"
#include "public.h"

#define VIJGPU_CMD_TIMEOUT_MS   5000    /* 5 second timeout per command */

/* -----------------------------------------------------------------------
 * VijGpuIoQueueCreate
 * Create a default sequential dispatch queue for IOCTL handling.
 * --------------------------------------------------------------------- */
NTSTATUS
VijGpuIoQueueCreate(WDFDEVICE Device)
{
    NTSTATUS                status;
    WDF_IO_QUEUE_CONFIG     queueConfig;
    WDFQUEUE                queue;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig,
                                           WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = VijGpuEvtIoDeviceControl;

    status = WdfIoQueueCreate(Device, &queueConfig,
                               WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "vijgpu: WdfIoQueueCreate failed 0x%x\n", status));
    }

    return status;
}

/* -----------------------------------------------------------------------
 * VijGpuEvtIoDeviceControl - dispatch IOCTL requests
 * --------------------------------------------------------------------- */
VOID
VijGpuEvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
    )
{
    NTSTATUS        status = STATUS_INVALID_DEVICE_REQUEST;
    WDFDEVICE       device = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT ctx    = GetDeviceContext(device);
    size_t          bytesWritten = 0;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    switch (IoControlCode) {

    /* ----------------------------------------------------------------
     * IOCTL_VIJGPU_GET_DEVICE_INFO
     * Returns device identity and VRAM size.
     * -------------------------------------------------------------- */
    case IOCTL_VIJGPU_GET_DEVICE_INFO: {
        PVIJGPU_DEVICE_INFO info;

        status = WdfRequestRetrieveOutputBuffer(
                     Request,
                     sizeof(VIJGPU_DEVICE_INFO),
                     (PVOID *)&info,
                     NULL);
        if (!NT_SUCCESS(status)) break;

        info->DeviceId = VijGpuReadReg32(ctx->Bar0, VIJGPU_R_DEVICE_ID);
        info->Version  = VijGpuReadReg32(ctx->Bar0, VIJGPU_R_VERSION);
        info->Status   = VijGpuReadReg32(ctx->Bar0, VIJGPU_R_STATUS);
        info->VramSize = ctx->VramSize;

        bytesWritten = sizeof(VIJGPU_DEVICE_INFO);
        status = STATUS_SUCCESS;
        break;
    }

    /* ----------------------------------------------------------------
     * IOCTL_VIJGPU_TEST_NOP
     * Submit a NOP command and wait for completion.
     * -------------------------------------------------------------- */
    case IOCTL_VIJGPU_TEST_NOP: {
        PVIJGPU_NOP_RESULT  result;
        ULONGLONG           seq;

        status = WdfRequestRetrieveOutputBuffer(
                     Request,
                     sizeof(VIJGPU_NOP_RESULT),
                     (PVOID *)&result,
                     NULL);
        if (!NT_SUCCESS(status)) break;

        status = VijGpuSubmitCommand(ctx,
                                     VIJGPU_CMD_NOP,
                                     0, 0, 0,
                                     &seq);
        if (!NT_SUCCESS(status)) break;

        status = VijGpuWaitCompletion(ctx, seq, VIJGPU_CMD_TIMEOUT_MS);

        result->Sequence    = ctx->Ring.LastCompletedSeq;
        result->ComplStatus = ctx->Ring.LastComplStatus;

        bytesWritten = sizeof(VIJGPU_NOP_RESULT);
        status = STATUS_SUCCESS;
        break;
    }

    /* ----------------------------------------------------------------
     * IOCTL_VIJGPU_TEST_FILL_VRAM
     * Fill a region of virtual VRAM via command queue.
     * -------------------------------------------------------------- */
    case IOCTL_VIJGPU_TEST_FILL_VRAM: {
        PVIJGPU_FILL_VRAM_INPUT  input;
        PVIJGPU_FILL_VRAM_RESULT result;
        ULONGLONG                seq;

        status = WdfRequestRetrieveInputBuffer(
                     Request,
                     sizeof(VIJGPU_FILL_VRAM_INPUT),
                     (PVOID *)&input,
                     NULL);
        if (!NT_SUCCESS(status)) break;

        status = WdfRequestRetrieveOutputBuffer(
                     Request,
                     sizeof(VIJGPU_FILL_VRAM_RESULT),
                     (PVOID *)&result,
                     NULL);
        if (!NT_SUCCESS(status)) break;

        status = VijGpuSubmitCommand(ctx,
                                     VIJGPU_CMD_FILL_VRAM,
                                     input->Offset,
                                     input->Length,
                                     (ULONGLONG)input->FillByte,
                                     &seq);
        if (!NT_SUCCESS(status)) break;

        status = VijGpuWaitCompletion(ctx, seq, VIJGPU_CMD_TIMEOUT_MS);

        result->Sequence    = ctx->Ring.LastCompletedSeq;
        result->ComplStatus = ctx->Ring.LastComplStatus;

        bytesWritten = sizeof(VIJGPU_FILL_VRAM_RESULT);
        status = STATUS_SUCCESS;
        break;
    }

    /* ----------------------------------------------------------------
     * IOCTL_VIJGPU_GET_COMPLETION
     * Return last completion state without submitting a new command.
     * -------------------------------------------------------------- */
    case IOCTL_VIJGPU_GET_COMPLETION: {
        PVIJGPU_COMPLETION_INFO info;

        status = WdfRequestRetrieveOutputBuffer(
                     Request,
                     sizeof(VIJGPU_COMPLETION_INFO),
                     (PVOID *)&info,
                     NULL);
        if (!NT_SUCCESS(status)) break;

        info->LastSequence = ctx->Ring.LastCompletedSeq;
        info->LastStatus   = ctx->Ring.LastComplStatus;

        bytesWritten = sizeof(VIJGPU_COMPLETION_INFO);
        status = STATUS_SUCCESS;
        break;
    }

    /* ----------------------------------------------------------------
     * IOCTL_VIJGPU_RESET
     * Reset the virtual GPU device.
     * -------------------------------------------------------------- */
    case IOCTL_VIJGPU_RESET:
        VijGpuResetDevice(ctx);
        status = STATUS_SUCCESS;
        break;

    default:
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "vijgpu: unknown IOCTL 0x%x\n", IoControlCode));
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesWritten);
}
