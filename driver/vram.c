/*
 * vram.c - vijGPU virtual VRAM helpers
 *
 * BAR1 is the virtual VRAM region backed by host RAM in QEMU.
 * The driver can read/write it directly through the mapped BAR1 pointer.
 * VRAM size is read from the device registers at startup.
 *
 * At this stage we do not expose VRAM to user-mode directly.
 * Commands (FILL_VRAM, COPY_VRAM) are submitted via the command queue.
 */

#include "device.h"

/* -----------------------------------------------------------------------
 * VijGpuVramInit
 * Verify VRAM is accessible by doing a simple read/write test on the
 * first DWORD.  Saves and restores the original value.
 * --------------------------------------------------------------------- */
NTSTATUS
VijGpuVramInit(PDEVICE_CONTEXT Ctx)
{
    PULONG  vram;
    ULONG   original, test;

    if (Ctx->Bar1 == NULL || Ctx->VramSize < sizeof(ULONG)) {
        return STATUS_DEVICE_NOT_READY;
    }

    vram     = (PULONG)Ctx->Bar1;
    original = READ_REGISTER_ULONG(vram);

    /* Write a test pattern */
    WRITE_REGISTER_ULONG(vram, 0xCAFEBABEUL);
    test = READ_REGISTER_ULONG(vram);

    /* Restore original */
    WRITE_REGISTER_ULONG(vram, original);

    if (test != 0xCAFEBABEUL) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "vijgpu: VRAM read-back test failed "
                   "(wrote 0xCAFEBABE, read 0x%08x)\n", test));
        return STATUS_DEVICE_DATA_ERROR;
    }

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "vijgpu: VRAM accessible, size=%llu MiB\n",
               Ctx->VramSize / (1024 * 1024)));
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * VijGpuVramRelease
 * Nothing to free — BAR1 is unmapped in ReleaseHardware.
 * --------------------------------------------------------------------- */
VOID
VijGpuVramRelease(PDEVICE_CONTEXT Ctx)
{
    UNREFERENCED_PARAMETER(Ctx);
    /* BAR1 unmapped by ReleaseHardware */
}
