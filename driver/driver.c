/*
 * driver.c - vijGPU KMDF driver entry point
 */

#include "device.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, VijGpuEvtDeviceAdd)
#endif

/*
 * DriverEntry - KMDF driver entry point.
 */
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS            status;
    WDF_DRIVER_CONFIG   config;

    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "vijgpu: DriverEntry\n"));

    WDF_DRIVER_CONFIG_INIT(&config, VijGpuEvtDeviceAdd);

    status = WdfDriverCreate(
                 DriverObject,
                 RegistryPath,
                 WDF_NO_OBJECT_ATTRIBUTES,
                 &config,
                 WDF_NO_HANDLE);

    if (!NT_SUCCESS(status)) {
        KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "vijgpu: WdfDriverCreate failed 0x%x\n", status));
    }

    return status;
}

/*
 * VijGpuEvtDeviceAdd - called by the framework when a new vijGPU device
 * is discovered by PnP.
 */
NTSTATUS
VijGpuEvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    PAGED_CODE();
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "vijgpu: VijGpuEvtDeviceAdd\n"));
    return VijGpuCreateDevice(Driver, DeviceInit);
}
