/*
 * public.h - IOCTL interface between user-mode and vijGPU kernel driver
 *
 * Both the kernel driver and the user-mode test app include this header.
 * It must NOT include any kernel-only headers.
 */

#pragma once

#include <initguid.h>

/* Device interface GUID (matches device.h) */
DEFINE_GUID(GUID_DEVINTERFACE_VIJGPU,
    0xa1b2c3d4, 0xe5f6, 0x7890,
    0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90);

/* -----------------------------------------------------------------------
 * IOCTL codes
 * --------------------------------------------------------------------- */
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

/* -----------------------------------------------------------------------
 * IOCTL data structures
 * --------------------------------------------------------------------- */

/* IOCTL_VIJGPU_GET_DEVICE_INFO output */
typedef struct _VIJGPU_DEVICE_INFO {
    ULONG       DeviceId;       /* 0x56494A47 */
    ULONG       Version;        /* 0x00020000 */
    ULONG       Status;         /* STATUS register */
    ULONGLONG   VramSize;       /* bytes */
} VIJGPU_DEVICE_INFO, *PVIJGPU_DEVICE_INFO;

/* IOCTL_VIJGPU_TEST_NOP output */
typedef struct _VIJGPU_NOP_RESULT {
    ULONGLONG   Sequence;
    ULONG       ComplStatus;
} VIJGPU_NOP_RESULT, *PVIJGPU_NOP_RESULT;

/* IOCTL_VIJGPU_TEST_FILL_VRAM input */
typedef struct _VIJGPU_FILL_VRAM_INPUT {
    ULONGLONG   Offset;         /* byte offset into VRAM */
    ULONGLONG   Length;         /* byte length */
    UCHAR       FillByte;       /* fill value */
} VIJGPU_FILL_VRAM_INPUT, *PVIJGPU_FILL_VRAM_INPUT;

/* IOCTL_VIJGPU_TEST_FILL_VRAM output */
typedef struct _VIJGPU_FILL_VRAM_RESULT {
    ULONGLONG   Sequence;
    ULONG       ComplStatus;
} VIJGPU_FILL_VRAM_RESULT, *PVIJGPU_FILL_VRAM_RESULT;

/* IOCTL_VIJGPU_GET_COMPLETION output */
typedef struct _VIJGPU_COMPLETION_INFO {
    ULONGLONG   LastSequence;
    ULONG       LastStatus;
} VIJGPU_COMPLETION_INFO, *PVIJGPU_COMPLETION_INFO;
