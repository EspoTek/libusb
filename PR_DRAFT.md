# Draft PR text for libusb/libusb (do NOT open until confirmed on Windows hardware)

Branch: `EspoTek/libusb:windows-sync-control-length` → `libusb/libusb:master`
Open as a **draft** PR. Title:

    windows: don't synthesize a completion the kernel already delivered

Body:

---

> **Draft.** This fix was reached by reading libusb, libusbK 3.0.7/3.0.8 and libusb0.sys source and matches every symptom we logged, and it compiles on MSYS2 and MSVC, but it has not yet been run against the affected driver combination. A human is testing it on real hardware over the coming days with the new `tests/control_length` program and will update this PR (results, and ready-for-review) once it is confirmed. Until then please treat it as a diagnosis with a proposed fix.

## Symptom

On Windows, with a device bound to the libusb-win32 kernel driver (`libusb0.sys`) and the libusbK.dll that its driver packages install (3.0.7.0), control transfers that the device answered correctly come back with `actual_length == 0`, occasionally garbage, on a few percent of transfers (more with debug logging on). With assertions enabled (MSYS2's package) `io.c` aborts on `transfer->actual_length >= 0`. Reported against an Atmel DFU bootloader, where dfu-programmer aborts every firmware flash after the erase and leaves the board stuck in DFU: espotek-org/Labrador#450, measurements in espotek-org/Labrador#458 (62–83 bad lengths per 3000 `DFU_GETSTATUS`; 0 via libusb0.dll), diagnosis in espotek-org/Labrador#459.

`LIBUSB_DEBUG=4` shows the completion-port thread seeing the right count and the handler not:

```
libusb: debug [windows_iocp_thread] transfer 000002090ff655f8 completed, length 4
libusb: debug [windows_handle_transfer_completion] handling transfer 000002090ff655f8 completion with errcode 0, length 0
```

## Cause

1. `libusb0.sys` completes every control request inside its dispatch routine (`control_transfer()` → `call_usbd_ex()` waits for the URB), so `DeviceIoControl()` returns TRUE rather than `ERROR_IO_PENDING`.
2. libusbK.dll 3.0.7 routes the libusb0 driver's `ControlTransfer` to `UsbK_ControlTransfer()`, which does `Ioctl_Async()` = `DeviceIoControl(dev, code, in, in_size, out, out_size, NULL, Overlapped)` with libusb's own OVERLAPPED and never writes `LengthTransferred` on that path. (3.0.8.0, 2021, replaced it with a synchronous `usb_control_msg()` on a private OVERLAPPED that does report the length, which is why the bug depends on the DLL version in the driver package.)
3. `winusbx_submit_control_transfer()` sees TRUE and calls `windows_force_sync_completion(itransfer, transferred)` with `transferred` uninitialised. That function overwrites `Internal`/`InternalHigh` of an OVERLAPPED the kernel has already completed, and posts a second completion packet for it. The kernel's packet is already queued (it always is, for a synchronous success on a handle bound to a completion port, unless `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS` is set — nobody sets it), so the IOCP thread logs the correct count from the packet, while `GetOverlappedResult()` on the event thread reads whichever write won the race: the kernel's count, 0, or stack garbage. The second packet is later dropped as "ignoring overlapped", or worse, matches a later transfer that reuses the memory.

## Fix

`windows_submit_transfer()` marks the OVERLAPPED `STATUS_PENDING` before calling the backend; only the kernel changes that. `windows_force_sync_completion()` returns early when it finds the status changed: the real completion is already on its way with the right byte count. When the DLL emulated the request without kernel I/O on our OVERLAPPED (HID descriptor emulation, the SET_CONFIGURATION shortcut, libusbK ≥ 3.0.8's libusb0 path), the status is still `STATUS_PENDING` and the synthetic completion happens exactly as before.

The bulk path (`winusbx_submit_bulk_transfer`), the HID interrupt path and `_hid_get_report()` already accept a synchronous TRUE and wait for the kernel's packet; this makes control transfers consistent with them. `windows_usbdk.c`'s `TransferSuccess` case goes through the same function and now also stops double-completing.

Alternative considered: keep the byte count from `GetQueuedCompletionStatus()` and prefer it when `GetOverlappedResult()` disagrees (Aron Rubin's patch in espotek-org/Labrador#459, hardware-verified: 0/3 → 3/3 flashes). It works because the kernel's packet is always queued first, but leaves the second packet and the clobbered OVERLAPPED in place.

## Test

`tests/control_length` (new): repeats `GET_DESCRIPTOR(DEVICE)` (sync and async), `GET_STATUS` and, on a DFU interface, `DFU_GETSTATUS`/`DFU_GETSTATE`, failing on any transfer that reports a length other than what the device sent. Needs `LIBUSB_TEST_DEVICE=vid:pid`; skips without it so `make check` stays green. Added to automake, the Android makefile and the MSVC solution.

Hardware results: *(to be filled in by the tester — unfixed vs fixed, N iterations, driver/DLL versions)*.

---
