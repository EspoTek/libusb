# PR text for libusb/libusb (confirmed on Windows hardware 2026-10-13; ready to open)

Branch: `EspoTek/libusb:windows-sync-control-length` → `libusb/libusb:master`
Title:

    windows: don't synthesize a completion for a request the kernel owns

Body:

---

## Symptom

On Windows, with a device bound to the libusb-win32 kernel driver (`libusb0.sys`) and the libusbK.dll that its driver packages install (3.0.7.0), control transfers that the device answered correctly come back with `actual_length == 0`, occasionally garbage, on a few percent of transfers (more with debug logging on). With assertions enabled (MSYS2's package) `io.c` aborts on `transfer->actual_length >= 0`. Reported against an Atmel DFU bootloader, where dfu-programmer aborts every firmware flash after the erase and leaves the board stuck in DFU: espotek-org/Labrador#450, measurements in espotek-org/Labrador#458 (62–83 bad lengths per 3000 `DFU_GETSTATUS`; 0 via libusb0.dll), diagnosis in espotek-org/Labrador#459.

`LIBUSB_DEBUG=4` shows the completion-port thread seeing the right count and the handler not:

```
libusb: debug [windows_iocp_thread] transfer 000002090ff655f8 completed, length 4
libusb: debug [windows_handle_transfer_completion] handling transfer 000002090ff655f8 completion with errcode 0, length 0
```

## Cause

1. libusb's `winusbx_submit_control_transfer()` assumes that when the driver DLL's `ControlTransfer` returns TRUE instead of `ERROR_IO_PENDING`, the DLL satisfied the request itself and nothing will reach the completion port, so it calls `windows_force_sync_completion(itransfer, transferred)`: overwrite the OVERLAPPED's status fields and post a completion packet.
2. libusbK.dll 3.0.7 breaks that assumption for `libusb0.sys` devices. Probed directly on the affected bootloader with a handle bound to a completion port, the way libusb uses it: its `ControlTransfer` passes libusb's OVERLAPPED to `DeviceIoControl()`, returns TRUE while the request is still pending (`GetLastError()` = `WAIT_TIMEOUT`, `OVERLAPPED.Internal` = `STATUS_PENDING`) in 2999 of 3000 calls, never writes `LengthTransferred` (3000/3000), and the kernel then delivers exactly one completion packet carrying the right byte count (3000/3000). (3.0.8.0, 2021, replaced this path with a synchronous `usb_control_msg()` on a private OVERLAPPED that does report the length, which is why the bug depends on the DLL version in the driver package.)
3. So libusb clobbers `Internal`/`InternalHigh` with an uninitialised length and posts a second packet, racing the kernel's own completion. Whichever write or packet wins decides whether the application sees the real length, 0, or stack garbage, and the stale second packet can complete the *next* transfer that reuses the OVERLAPPED (seen in a `LIBUSB_DEBUG=4` trace: a 0-byte packet dequeued for a freshly submitted transfer at the same address).

## Fix

`windows_submit_transfer()` stores a sentinel (`WINDOWS_OVERLAPPED_NOT_SUBMITTED`) in `OVERLAPPED.Internal` before calling the backend. `DeviceIoControl()` replaces it with `STATUS_PENDING` at submission ("When the request is issued, the system sets this member to STATUS_PENDING"), before any completion can happen, whenever a DLL hands our OVERLAPPED to the kernel. `windows_force_sync_completion()` therefore has a race-free test: sentinel still present means the DLL emulated the request without kernel I/O (HID descriptor emulation, the SET_CONFIGURATION shortcut, libusbK 3.0.8+'s libusb0 path) and the synthetic completion happens exactly as before; anything else means the kernel owns the request and its packet, with the right byte count, is on its way.

An earlier revision of this change tested for `STATUS_PENDING` itself, i.e. "has the kernel already finished it?". With the handle bound to a completion port that is the minority case, so it still double-completed most transfers and left about 0.1% wrong lengths (76 of 60,000 in the measurement below).

The bulk path (`winusbx_submit_bulk_transfer`), the HID interrupt path and `_hid_get_report()` already accept a synchronous TRUE and wait for the kernel's packet; this makes control transfers consistent with them. `windows_usbdk.c`'s `TransferSuccess` case goes through the same function and now also stops double-completing.

Alternative considered: keep the byte count from `GetQueuedCompletionStatus()` and prefer it when `GetOverlappedResult()` disagrees (Aron Rubin's patch in espotek-org/Labrador#459, hardware-verified: 0/3 → 3/3 flashes). It works because the kernel's packet is always queued first, but leaves the second packet and the clobbered OVERLAPPED in place.

## Test

`tests/control_length` (new): repeats `GET_DESCRIPTOR(DEVICE)` (sync and async), `GET_STATUS` and, on a DFU interface, `DFU_GETSTATUS`/`DFU_GETSTATE`, failing on any transfer that reports a length other than what the device sent. Needs `LIBUSB_TEST_DEVICE=vid:pid`; skips without it so `make check` stays green. Added to automake, the Android makefile and the MSVC solution.

Hardware results (Atmel DFU bootloader `03EB:2FE4` on an EspoTek Labrador, libusb0.sys 1.2.6.0, libusbK.dll 3.0.7.0, Windows 11 Pro 26200, MinGW-w64 GCC 14.2 static builds; 20,000 iterations each of sync `GET_DESCRIPTOR`, async `GET_DESCRIPTOR`, sync `GET_STATUS`):

| build | wrong-length transfers / 60,000 |
|---|---|
| unpatched (1.0.30 backend) | 1,195 (344 / 558 / 293) |
| earlier revision (`STATUS_PENDING` test) | 76 (18 / 33 / 25) |
| this change | **0**; soak of 50 consecutive runs (3,000,000 transfers): 0 wrong, 0 errors |

The `DFU_GETSTATUS` test skips on this bootloader because it exposes no class 0xFE interface descriptor.

---
