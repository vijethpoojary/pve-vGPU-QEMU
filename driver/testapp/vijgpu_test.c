/*
 * vijgpu_test.c - User-mode test application for vijGPU driver
 *
 * Opens the vijGPU device via its device interface, then runs a sequence
 * of IOCTL tests to verify the driver and virtual GPU are working.
 *
 * Build: cl /W4 vijgpu_test.c /link /out:vijgpu_test.exe
 * Run inside VM 300 after installing the vijgpu driver.
 *
 * Expected output on success:
 *   [INFO] vijGPU Test Application
 *   [INFO] Opening device...
 *   [INFO] Device opened
 *   [PASS] GET_DEVICE_INFO: DeviceId=0x56494A47 Version=0x00020000 VramSize=268435456
 *   [PASS] TEST_NOP: seq=1 status=0
 *   [PASS] TEST_FILL_VRAM: seq=2 status=0
 *   [PASS] GET_COMPLETION: seq=2 status=0
 *   [PASS] RESET
 *   [INFO] All tests passed.
 */

#include <windows.h>
#include <initguid.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Include the public IOCTL definitions.
 * In the test app we include public.h directly; it contains only
 * standard Windows types and IOCTL codes.
 */

/* -----------------------------------------------------------------------
 * Duplicate public.h definitions here to avoid WDK dependency
 * (public.h includes <initguid.h> which is available in the SDK)
 * --------------------------------------------------------------------- */

DEFINE_GUID(GUID_DEVINTERFACE_VIJGPU,
    0xa1b2c3d4, 0xe5f6, 0x7890,
    0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90);

#define VIJGPU_IOCTL_BASE   0x8000

#define IOCTL_VIJGPU_GET_DEVICE_INFO \
    CTL_CODE(FILE_DEVICE_UNKNOWN, VIJGPU_IOCTL_BASE + 0, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_VIJGPU_TEST_NOP \
    CTL_CODE(FILE_DEVICE_UNKNOWN, VIJGPU_IOCTL_BASE + 1, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_VIJGPU_TEST_FILL_VRAM \
    CTL_CODE(FILE_DEVICE_UNKNOWN, VIJGPU_IOCTL_BASE + 2, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_VIJGPU_GET_COMPLETION \
    CTL_CODE(FILE_DEVICE_UNKNOWN, VIJGPU_IOCTL_BASE + 3, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_VIJGPU_RESET \
    CTL_CODE(FILE_DEVICE_UNKNOWN, VIJGPU_IOCTL_BASE + 4, METHOD_BUFFERED, FILE_WRITE_DATA)

#pragma pack(push, 1)
typedef struct { ULONG DeviceId; ULONG Version; ULONG Status; ULONGLONG VramSize; } VIJGPU_DEVICE_INFO;
typedef struct { ULONGLONG Sequence; ULONG ComplStatus; } VIJGPU_NOP_RESULT;
typedef struct { ULONGLONG Offset; ULONGLONG Length; UCHAR FillByte; } VIJGPU_FILL_VRAM_INPUT;
typedef struct { ULONGLONG Sequence; ULONG ComplStatus; } VIJGPU_FILL_VRAM_RESULT;
typedef struct { ULONGLONG LastSequence; ULONG LastStatus; } VIJGPU_COMPLETION_INFO;
#pragma pack(pop)

/* -----------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */

#define LOG_INFO(fmt, ...)  printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_PASS(fmt, ...)  printf("[PASS] " fmt "\n", ##__VA_ARGS__)
#define LOG_FAIL(fmt, ...)  fprintf(stderr, "[FAIL] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...)   fprintf(stderr, "[ERR]  " fmt " (Win32=%lu)\n", ##__VA_ARGS__, GetLastError())

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { LOG_FAIL(msg); g_failures++; } \
} while (0)

/* -----------------------------------------------------------------------
 * Find and open the vijGPU device by its device interface GUID.
 * --------------------------------------------------------------------- */
static HANDLE
OpenVijGpuDevice(void)
{
    HDEVINFO                         devInfo;
    SP_DEVICE_INTERFACE_DATA         ifData;
    PSP_DEVICE_INTERFACE_DETAIL_DATA detail = NULL;
    DWORD                            reqSize = 0;
    HANDLE                           hDevice = INVALID_HANDLE_VALUE;

    devInfo = SetupDiGetClassDevs(
                  &GUID_DEVINTERFACE_VIJGPU,
                  NULL,
                  NULL,
                  DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (devInfo == INVALID_HANDLE_VALUE) {
        LOG_ERR("SetupDiGetClassDevs failed");
        return INVALID_HANDLE_VALUE;
    }

    ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    if (!SetupDiEnumDeviceInterfaces(devInfo, NULL,
                                      &GUID_DEVINTERFACE_VIJGPU,
                                      0, &ifData)) {
        LOG_ERR("SetupDiEnumDeviceInterfaces failed "
                "(is the driver installed and device present?)");
        SetupDiDestroyDeviceInfoList(devInfo);
        return INVALID_HANDLE_VALUE;
    }

    /* Get required size */
    SetupDiGetDeviceInterfaceDetail(devInfo, &ifData, NULL, 0, &reqSize, NULL);

    detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(reqSize);
    if (!detail) {
        LOG_FAIL("out of memory");
        SetupDiDestroyDeviceInfoList(devInfo);
        return INVALID_HANDLE_VALUE;
    }
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

    if (!SetupDiGetDeviceInterfaceDetail(devInfo, &ifData,
                                          detail, reqSize, NULL, NULL)) {
        LOG_ERR("SetupDiGetDeviceInterfaceDetail failed");
        free(detail);
        SetupDiDestroyDeviceInfoList(devInfo);
        return INVALID_HANDLE_VALUE;
    }

    LOG_INFO("Device path: %s", detail->DevicePath);

    hDevice = CreateFile(
                  detail->DevicePath,
                  GENERIC_READ | GENERIC_WRITE,
                  0,
                  NULL,
                  OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL,
                  NULL);

    if (hDevice == INVALID_HANDLE_VALUE) {
        LOG_ERR("CreateFile failed");
    }

    free(detail);
    SetupDiDestroyDeviceInfoList(devInfo);
    return hDevice;
}

/* -----------------------------------------------------------------------
 * Test functions
 * --------------------------------------------------------------------- */

static void
TestGetDeviceInfo(HANDLE h)
{
    VIJGPU_DEVICE_INFO info = {0};
    DWORD              returned = 0;

    if (!DeviceIoControl(h, IOCTL_VIJGPU_GET_DEVICE_INFO,
                          NULL, 0,
                          &info, sizeof(info),
                          &returned, NULL)) {
        LOG_ERR("GET_DEVICE_INFO ioctl failed");
        g_failures++;
        return;
    }

    LOG_PASS("GET_DEVICE_INFO: DeviceId=0x%08lX Version=0x%08lX "
             "Status=0x%08lX VramSize=%llu",
             info.DeviceId, info.Version, info.Status, info.VramSize);

    CHECK(info.DeviceId == 0x56494A47UL, "DeviceId mismatch (expected 0x56494A47)");
    CHECK(info.Version  == 0x00020000UL, "Version mismatch (expected 0x00020000)");
    CHECK(info.VramSize  > 0,            "VramSize is 0");
    CHECK((info.Status & 0x1) != 0,      "STATUS_READY not set");
}

static void
TestNop(HANDLE h)
{
    VIJGPU_NOP_RESULT result = {0};
    DWORD             returned = 0;

    if (!DeviceIoControl(h, IOCTL_VIJGPU_TEST_NOP,
                          NULL, 0,
                          &result, sizeof(result),
                          &returned, NULL)) {
        LOG_ERR("TEST_NOP ioctl failed");
        g_failures++;
        return;
    }

    LOG_PASS("TEST_NOP: seq=%llu status=%lu",
             result.Sequence, result.ComplStatus);

    CHECK(result.ComplStatus == 0, "NOP completion status non-zero");
    CHECK(result.Sequence    >  0, "NOP sequence is 0");
}

static void
TestFillVram(HANDLE h)
{
    VIJGPU_FILL_VRAM_INPUT  input  = {0};
    VIJGPU_FILL_VRAM_RESULT result = {0};
    DWORD                   returned = 0;

    input.Offset   = 0;
    input.Length   = 4096;   /* fill first 4 KiB */
    input.FillByte = 0xAB;

    if (!DeviceIoControl(h, IOCTL_VIJGPU_TEST_FILL_VRAM,
                          &input,  sizeof(input),
                          &result, sizeof(result),
                          &returned, NULL)) {
        LOG_ERR("TEST_FILL_VRAM ioctl failed");
        g_failures++;
        return;
    }

    LOG_PASS("TEST_FILL_VRAM: seq=%llu status=%lu",
             result.Sequence, result.ComplStatus);

    CHECK(result.ComplStatus == 0, "FILL_VRAM completion status non-zero");
}

static void
TestGetCompletion(HANDLE h)
{
    VIJGPU_COMPLETION_INFO info = {0};
    DWORD                  returned = 0;

    if (!DeviceIoControl(h, IOCTL_VIJGPU_GET_COMPLETION,
                          NULL, 0,
                          &info, sizeof(info),
                          &returned, NULL)) {
        LOG_ERR("GET_COMPLETION ioctl failed");
        g_failures++;
        return;
    }

    LOG_PASS("GET_COMPLETION: seq=%llu status=%lu",
             info.LastSequence, info.LastStatus);

    CHECK(info.LastSequence > 0, "no completion recorded");
}

static void
TestReset(HANDLE h)
{
    DWORD returned = 0;

    if (!DeviceIoControl(h, IOCTL_VIJGPU_RESET,
                          NULL, 0,
                          NULL, 0,
                          &returned, NULL)) {
        LOG_ERR("RESET ioctl failed");
        g_failures++;
        return;
    }

    LOG_PASS("RESET");

    /* After reset, device should still respond to GET_DEVICE_INFO */
    TestGetDeviceInfo(h);
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int main(void)
{
    HANDLE h;

    LOG_INFO("vijGPU Test Application v1.0");
    LOG_INFO("Opening device...");

    h = OpenVijGpuDevice();
    if (h == INVALID_HANDLE_VALUE) {
        LOG_FAIL("Could not open vijGPU device");
        LOG_INFO("Make sure:");
        LOG_INFO("  1. The vijgpu.sys driver is installed");
        LOG_INFO("  2. VM has -device vijgpu in its QEMU args");
        LOG_INFO("  3. The device appears in Device Manager");
        return 1;
    }

    LOG_INFO("Device opened successfully");
    printf("\n");

    TestGetDeviceInfo(h);
    TestNop(h);
    TestFillVram(h);
    TestGetCompletion(h);
    TestReset(h);

    CloseHandle(h);

    printf("\n");
    if (g_failures == 0) {
        LOG_INFO("All tests passed.");
        return 0;
    } else {
        LOG_FAIL("%d test(s) failed.", g_failures);
        return 1;
    }
}
