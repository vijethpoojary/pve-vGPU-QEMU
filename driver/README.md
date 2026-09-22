# vijGPU Windows KMDF Driver

Windows kernel driver for the vijGPU virtual GPU device implemented in QEMU.

## Overview

This is a **KMDF function driver** that communicates with the `vijgpu` PCI device
provided by a custom QEMU 11.0.0 build running under Proxmox VE.

It is **NOT** a display/graphics driver. It does **NOT** implement WDDM, DirectX,
or any rendering pipeline. Its purpose is to validate the hardware ABI and provide
a foundation for future display driver development.

---

## Hardware ABI

The driver matches `hw/misc/vijgpu.c` in the QEMU source tree exactly.

### PCI Identity
| Field     | Value  |
|-----------|--------|
| Vendor ID | 0x1234 |
| Device ID | 0x1200 |
| Revision  | 0x01   |

### BAR Layout
| BAR | Size    | Purpose                        |
|-----|---------|--------------------------------|
| 0   | 4 KiB   | Control/status registers (MMIO)|
| 1   | 256 MiB | Virtual VRAM (RAM-backed)      |
| 2   | 4 KiB   | Command queue / doorbell       |

### Key BAR0 Registers
| Offset | Name          | Access  | Value/Notes                  |
|--------|---------------|---------|------------------------------|
| 0x00   | DEVICE_ID     | RO      | 0x56494A47 ("VIJG")          |
| 0x04   | VERSION       | RO      | 0x00020000 (v2.0)            |
| 0x08   | STATUS        | RO      | bit0=READY                   |
| 0x0C   | CTRL          | RW      | bit0=IRQ_ENABLE              |
| 0x10   | VRAM_SIZE_LO  | RO      | VRAM size low 32 bits        |
| 0x14   | VRAM_SIZE_HI  | RO      | VRAM size high 32 bits       |
| 0x20   | CQ_ADDR_LO    | RW      | Command ring PA low          |
| 0x24   | CQ_ADDR_HI    | RW      | Command ring PA high         |
| 0x28   | CQ_SIZE       | RW      | Ring entry count             |
| 0x2C   | CQ_HEAD       | RO      | Device head index            |
| 0x30   | CQ_TAIL       | RW      | Guest tail (triggers cmds)   |
| 0x34   | COMPL_STATUS  | RO      | Last completion status code  |
| 0x38   | COMPL_SEQ_LO  | RO      | Last completed sequence low  |
| 0x3C   | COMPL_SEQ_HI  | RO      | Last completed sequence high |
| 0x40   | IRQ_STATUS    | RO/W1C  | bit0=CMD_DONE                |
| 0x44   | IRQ_MASK      | RW      | bit0 enables CMD_DONE IRQ    |
| 0x48   | RESET         | WO      | Write 0xDEADBEEF to reset    |

### Command Format (40 bytes, little-endian)
```
Offset  Size  Field
  0      4    Opcode  (0=NOP, 1=WRITE_REG, 2=READ_REG, 3=FILL_VRAM, 4=COPY_VRAM)
  4      4    Flags   (reserved, must be 0)
  8      8    Arg0
 16      8    Arg1
 24      8    Arg2
 32      8    Sequence (caller-assigned, echoed in completion)
```

---

## File Structure

```
driver/
├── vijgpu.h        Hardware ABI definitions (shared with QEMU source)
├── public.h        IOCTL interface (shared with user-mode test app)
├── device.h        KMDF device context and declarations
├── driver.c        DriverEntry, EvtDeviceAdd
├── device.c        PrepareHardware, ReleaseHardware, D0Entry/Exit, Reset
├── queue.c         Command ring allocation, submit, wait-for-completion
├── interrupt.c     ISR and DPC for CMD_DONE interrupt
├── vram.c          VRAM accessibility verification
├── ioctl.c         IOCTL dispatch (GET_DEVICE_INFO, TEST_NOP, etc.)
├── vijgpu.inf      Driver INF for Windows PnP installation
├── vijgpu.vcxproj  Visual Studio project
└── testapp/
    ├── vijgpu_test.c   User-mode test application
    └── build_test.bat  Build script for test app
```

---

## Build Requirements

- **Visual Studio 2022** (Community or higher)
- **Windows Driver Kit (WDK)** for Windows 11
  - Download: https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk
  - Install WDK *after* Visual Studio
- **Windows SDK** (included with Visual Studio)

---

## Building the Driver

1. Install Visual Studio 2022 + WDK on your Windows build machine.

2. Open `vijgpu.vcxproj` in Visual Studio.

3. Select **Debug | x64** configuration.

4. Build → **Build Solution** (Ctrl+Shift+B).

5. Output: `x64\Debug\vijgpu.sys`

Alternatively from the Developer Command Prompt:
```
msbuild vijgpu.vcxproj /p:Configuration=Debug /p:Platform=x64
```

---

## Building the Test Application

Inside VM 300 (or any Windows x64 with MSVC):

```
cd testapp
build_test.bat
```

Output: `vijgpu_test.exe`

---

## Installing the Driver in VM 300

### Option A: Self-signed (for testing only)

1. Enable test signing in VM 300:
   ```
   bcdedit /set testsigning on
   ```
   Reboot.

2. Sign the driver:
   ```
   makecert -r -pe -ss PrivateCertStore -n "CN=vijGPU Test" vijgpu_test.cer
   certmgr /add vijgpu_test.cer /s /r localMachine root
   certmgr /add vijgpu_test.cer /s /r localMachine trustedpublisher
   signtool sign /s PrivateCertStore /n "vijGPU Test" /t http://timestamp.digicert.com vijgpu.sys
   ```

3. Install:
   ```
   pnputil /add-driver vijgpu.inf /install
   ```

4. The device should appear in Device Manager as **"vijGPU Virtual GPU"**
   instead of "Video Controller".

### Option B: Using devcon

```
devcon install vijgpu.inf "PCI\VEN_1234&DEV_1200"
```

---

## Running the Test

Inside VM 300, after the driver is installed:

```
vijgpu_test.exe
```

Expected output:
```
[INFO] vijGPU Test Application v1.0
[INFO] Opening device...
[INFO] Device path: \\?\pci#ven_1234&dev_1200...
[INFO] Device opened successfully

[PASS] GET_DEVICE_INFO: DeviceId=0x56494A47 Version=0x00020000 VramSize=268435456
[PASS] TEST_NOP: seq=1 status=0
[PASS] TEST_FILL_VRAM: seq=2 status=0
[PASS] GET_COMPLETION: seq=2 status=0
[PASS] RESET
[PASS] GET_DEVICE_INFO: DeviceId=0x56494A47 Version=0x00020000 VramSize=268435456

[INFO] All tests passed.
```

---

## Debugging

Enable kernel debug output via WinDbg or DebugView:

1. Enable kernel debug messages in VM:
   ```
   bcdedit /debug on
   bcdedit /dbgsettings serial debugport:1 baudrate:115200
   ```

2. Or use **DebugView** (Sysinternals) inside the VM:
   - Check "Capture Kernel"
   - Filter on `vijgpu:`

Driver debug output uses `KdPrintEx` with `DPFLTR_IHVDRIVER_ID`.

---

## Known Limitations (First Milestone)

- Not a display driver (no WDDM, no DirectX)
- VRAM not exposed to user-mode directly (use FILL_VRAM via IOCTL)
- No live migration support (VMState not implemented in QEMU side)
- Physical GPU backend not connected (software-only)
