/*
 * libusb control transfer length test
 * Copyright © 2026 Chris Esposito <admin@espotek.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * Checks that the length a control transfer reports back matches what the
 * device actually sent, many times in a row.
 *
 * Regression test for the Windows backend handing the application an
 * actual_length of 0 (or garbage) for a control transfer the device answered
 * correctly. That happened when the driver DLL completed the request before
 * DeviceIoControl() returned: libusb0.sys does so for every control request,
 * and libusbK.dll 3.0.7 passes libusb's OVERLAPPED straight through to it
 * without filling in LengthTransferred. libusb then overwrote the byte count
 * the kernel had already stored with an uninitialized value. On the Atmel DFU
 * bootloader of the EspoTek Labrador that broke about 2% of transfers, enough
 * to abort every firmware flash part-way. See espotek-org/Labrador#450,
 * espotek-org/Labrador#458 (measurements) and espotek-org/Labrador#459.
 *
 * The test needs a device and is skipped without one:
 *
 *   LIBUSB_TEST_DEVICE=vid:pid     hexadecimal, e.g. 03eb:2fe4
 *   LIBUSB_TEST_ITERATIONS=N       transfers per test (default 1000)
 *
 * Every test only reads from the device with standard requests (device
 * descriptor, device status). If the device exposes a DFU interface (class
 * 0xFE, subclass 1) the DFU status request is exercised too, since that is
 * the request dfu-programmer hammers during a flash.
 *
 * To reproduce the original bug on Windows, bind the device to libusb0.sys
 * (libusb-win32) with libusbK.dll 3.0.7 installed, and run against a libusb
 * built without this fix: the sync and async GET_DESCRIPTOR tests report
 * transfers that returned 0 bytes.
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libusb.h"
#include "libusb_testlib.h"

#define DEFAULT_ITERATIONS	1000
#define TRANSFER_TIMEOUT_MS	1000
#define MAX_REPORTED_PROBLEMS	10

#define DFU_CLASS		0xFE
#define DFU_SUBCLASS		0x01
#define DFU_GETSTATUS		0x03
#define DFU_GETSTATUS_LENGTH	6
#define DFU_GETSTATE		0x05
#define DFU_GETSTATE_LENGTH	1

struct test_device {
	libusb_context *ctx;
	libusb_device_handle *handle;
	struct libusb_device_descriptor desc;
	int iterations;
	int dfu_interface; /* -1 when the device has none */
};

/* Running tally for one test; keeps the log readable at 1000+ iterations. */
struct tally {
	int ok;
	int wrong_length;
	int errors;
	int reported;
};

static void tally_problem(struct tally *t, int iteration, const char *what, int value)
{
	if (value < 0)
		t->errors++;
	else
		t->wrong_length++;

	if (t->reported < MAX_REPORTED_PROBLEMS) {
		if (value < 0)
			libusb_testlib_logf("  iteration %d: %s failed: %s", iteration, what, libusb_error_name(value));
		else
			libusb_testlib_logf("  iteration %d: %s returned %d bytes", iteration, what, value);
		t->reported++;
	} else if (t->reported == MAX_REPORTED_PROBLEMS) {
		libusb_testlib_logf("  (further problems not listed)");
		t->reported++;
	}
}

static libusb_testlib_result tally_result(const struct tally *t, const char *what, int expected, int iterations)
{
	libusb_testlib_logf("%s: %d/%d transfers returned the expected %d bytes (%d wrong length, %d errors)",
		what, t->ok, iterations, expected, t->wrong_length, t->errors);

	if (t->wrong_length > 0)
		return TEST_STATUS_FAILURE;
	if (t->errors > 0)
		return TEST_STATUS_ERROR;
	return TEST_STATUS_SUCCESS;
}

static int parse_env_int(const char *name, int fallback)
{
	const char *value = getenv(name);
	char *end;
	long n;

	if (value == NULL || *value == '\0')
		return fallback;

	n = strtol(value, &end, 10);
	if (*end != '\0' || n <= 0) {
		libusb_testlib_logf("%s must be a positive integer, not '%s'", name, value);
		return -1;
	}
	return (int)n;
}

/* Finds the first DFU interface in the active configuration, -1 if none. */
static int find_dfu_interface(libusb_device_handle *handle)
{
	struct libusb_config_descriptor *config;
	int found = -1;
	int r;

	r = libusb_get_active_config_descriptor(libusb_get_device(handle), &config);
	if (r != LIBUSB_SUCCESS) {
		libusb_testlib_logf("Could not read the active configuration: %s", libusb_error_name(r));
		return -1;
	}

	for (uint8_t i = 0; i < config->bNumInterfaces && found < 0; i++) {
		const struct libusb_interface *iface = &config->interface[i];

		for (int a = 0; a < iface->num_altsetting; a++) {
			const struct libusb_interface_descriptor *alt = &iface->altsetting[a];

			if (alt->bInterfaceClass == DFU_CLASS && alt->bInterfaceSubClass == DFU_SUBCLASS) {
				found = alt->bInterfaceNumber;
				break;
			}
		}
	}

	libusb_free_config_descriptor(config);
	return found;
}

/* Opens the device named by LIBUSB_TEST_DEVICE. Returns TEST_STATUS_SKIP
 * when the variable is unset, so the test suite passes without hardware. */
static libusb_testlib_result open_test_device(struct test_device *dev)
{
	const char *spec = getenv("LIBUSB_TEST_DEVICE");
	unsigned int vid, pid;
	char trailing;
	int r;

	memset(dev, 0, sizeof(*dev));
	dev->dfu_interface = -1;

	if (spec == NULL || *spec == '\0') {
		libusb_testlib_logf("LIBUSB_TEST_DEVICE is not set (expected vid:pid in hex), skipping");
		return TEST_STATUS_SKIP;
	}

	if (sscanf(spec, "%x:%x%c", &vid, &pid, &trailing) != 2 || vid > 0xFFFF || pid > 0xFFFF) {
		libusb_testlib_logf("LIBUSB_TEST_DEVICE must be vid:pid in hex, not '%s'", spec);
		return TEST_STATUS_ERROR;
	}

	dev->iterations = parse_env_int("LIBUSB_TEST_ITERATIONS", DEFAULT_ITERATIONS);
	if (dev->iterations < 0)
		return TEST_STATUS_ERROR;

	r = libusb_init_context(&dev->ctx, NULL, 0);
	if (r != LIBUSB_SUCCESS) {
		libusb_testlib_logf("Failed to init libusb: %s", libusb_error_name(r));
		return TEST_STATUS_ERROR;
	}

	dev->handle = libusb_open_device_with_vid_pid(dev->ctx, (uint16_t)vid, (uint16_t)pid);
	if (dev->handle == NULL) {
		libusb_testlib_logf("No openable device %04x:%04x (not connected, or no usable driver / permission)", vid, pid);
		libusb_exit(dev->ctx);
		dev->ctx = NULL;
		return TEST_STATUS_SKIP;
	}

	r = libusb_get_device_descriptor(libusb_get_device(dev->handle), &dev->desc);
	if (r != LIBUSB_SUCCESS) {
		libusb_testlib_logf("Failed to get the device descriptor: %s", libusb_error_name(r));
		libusb_close(dev->handle);
		libusb_exit(dev->ctx);
		dev->handle = NULL;
		dev->ctx = NULL;
		return TEST_STATUS_ERROR;
	}

	dev->dfu_interface = find_dfu_interface(dev->handle);

	libusb_testlib_logf("Using device %04x:%04x (bcdDevice %04x), %d iterations%s",
		vid, pid, dev->desc.bcdDevice, dev->iterations,
		dev->dfu_interface >= 0 ? ", has a DFU interface" : "");

	return TEST_STATUS_SUCCESS;
}

static void close_test_device(struct test_device *dev)
{
	if (dev->handle != NULL)
		libusb_close(dev->handle);
	if (dev->ctx != NULL)
		libusb_exit(dev->ctx);
	dev->handle = NULL;
	dev->ctx = NULL;
}

/* The descriptor bytes that must come back from GET_DESCRIPTOR(DEVICE),
 * built from what enumeration already read. A transfer that reports the
 * right length but hands back the wrong bytes is a failure too. */
static int device_descriptor_matches(const struct test_device *dev, const uint8_t *raw, int length)
{
	const struct libusb_device_descriptor *d = &dev->desc;

	if (length != LIBUSB_DT_DEVICE_SIZE)
		return 0;

	return raw[0] == LIBUSB_DT_DEVICE_SIZE
		&& raw[1] == LIBUSB_DT_DEVICE
		&& (raw[8] | (raw[9] << 8)) == d->idVendor
		&& (raw[10] | (raw[11] << 8)) == d->idProduct
		&& (raw[12] | (raw[13] << 8)) == d->bcdDevice
		&& raw[4] == d->bDeviceClass
		&& raw[17] == d->bNumConfigurations;
}

/** Synchronous GET_DESCRIPTOR(DEVICE), many times: every one must return the
 * 18-byte descriptor read at enumeration. This is the request that returned
 * 0 bytes on libusb0.sys + libusbK 3.0.7. */
static libusb_testlib_result test_sync_get_descriptor(void)
{
	struct test_device dev;
	struct tally tally = { 0, 0, 0, 0 };
	uint8_t buf[LIBUSB_DT_DEVICE_SIZE];
	libusb_testlib_result result;

	result = open_test_device(&dev);
	if (result != TEST_STATUS_SUCCESS)
		return result;

	for (int i = 0; i < dev.iterations; i++) {
		int r;

		memset(buf, 0, sizeof(buf));
		r = libusb_control_transfer(dev.handle,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_RECIPIENT_DEVICE,
			LIBUSB_REQUEST_GET_DESCRIPTOR, (uint16_t)(LIBUSB_DT_DEVICE << 8), 0,
			buf, sizeof(buf), TRANSFER_TIMEOUT_MS);

		if (r == LIBUSB_DT_DEVICE_SIZE && device_descriptor_matches(&dev, buf, r))
			tally.ok++;
		else if (r == LIBUSB_DT_DEVICE_SIZE)
			tally_problem(&tally, i, "GET_DESCRIPTOR returned the right length but wrong bytes; length", r);
		else
			tally_problem(&tally, i, "GET_DESCRIPTOR", r);
	}

	close_test_device(&dev);
	return tally_result(&tally, "sync GET_DESCRIPTOR", LIBUSB_DT_DEVICE_SIZE, dev.iterations);
}

struct async_state {
	int completed;
};

static void LIBUSB_CALL async_callback(struct libusb_transfer *transfer)
{
	struct async_state *state = (struct async_state *)transfer->user_data;

	state->completed = 1;
}

/** The same request through the asynchronous API, checking the transfer's
 * status and actual_length directly rather than the sync wrapper's return. */
static libusb_testlib_result test_async_get_descriptor(void)
{
	struct test_device dev;
	struct tally tally = { 0, 0, 0, 0 };
	struct libusb_transfer *transfer;
	uint8_t buf[LIBUSB_CONTROL_SETUP_SIZE + LIBUSB_DT_DEVICE_SIZE];
	libusb_testlib_result result;

	result = open_test_device(&dev);
	if (result != TEST_STATUS_SUCCESS)
		return result;

	transfer = libusb_alloc_transfer(0);
	if (transfer == NULL) {
		libusb_testlib_logf("Failed to allocate a transfer");
		close_test_device(&dev);
		return TEST_STATUS_ERROR;
	}

	for (int i = 0; i < dev.iterations; i++) {
		struct async_state state = { 0 };
		int r;

		memset(buf, 0, sizeof(buf));
		libusb_fill_control_setup(buf,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_RECIPIENT_DEVICE,
			LIBUSB_REQUEST_GET_DESCRIPTOR, (uint16_t)(LIBUSB_DT_DEVICE << 8), 0,
			LIBUSB_DT_DEVICE_SIZE);
		libusb_fill_control_transfer(transfer, dev.handle, buf, async_callback, &state, TRANSFER_TIMEOUT_MS);

		r = libusb_submit_transfer(transfer);
		if (r != LIBUSB_SUCCESS) {
			tally_problem(&tally, i, "submit", r);
			continue;
		}

		while (!state.completed) {
			r = libusb_handle_events_completed(dev.ctx, &state.completed);
			if (r != LIBUSB_SUCCESS && r != LIBUSB_ERROR_INTERRUPTED) {
				libusb_testlib_logf("  iteration %d: libusb_handle_events failed: %s", i, libusb_error_name(r));
				break;
			}
		}

		if (!state.completed) {
			/* Event loop failed; cancel and drain so the transfer is not freed in flight. */
			libusb_cancel_transfer(transfer);
			while (!state.completed)
				if (libusb_handle_events_completed(dev.ctx, &state.completed) != LIBUSB_SUCCESS)
					break;
			tally.errors++;
			continue;
		}

		if (transfer->status != LIBUSB_TRANSFER_COMPLETED)
			tally_problem(&tally, i, "async GET_DESCRIPTOR", transfer->status == LIBUSB_TRANSFER_TIMED_OUT ?
				LIBUSB_ERROR_TIMEOUT : LIBUSB_ERROR_IO);
		else if (transfer->actual_length == LIBUSB_DT_DEVICE_SIZE
				&& device_descriptor_matches(&dev, buf + LIBUSB_CONTROL_SETUP_SIZE, transfer->actual_length))
			tally.ok++;
		else if (transfer->actual_length == LIBUSB_DT_DEVICE_SIZE)
			tally_problem(&tally, i, "async GET_DESCRIPTOR returned the right length but wrong bytes; length",
				transfer->actual_length);
		else
			tally_problem(&tally, i, "async GET_DESCRIPTOR actual_length", transfer->actual_length);
	}

	libusb_free_transfer(transfer);
	close_test_device(&dev);
	return tally_result(&tally, "async GET_DESCRIPTOR", LIBUSB_DT_DEVICE_SIZE, dev.iterations);
}

/** Synchronous GET_STATUS(DEVICE): a 2-byte answer, the smallest standard
 * read, so a short or zero length is unambiguous. */
static libusb_testlib_result test_sync_get_status(void)
{
	struct test_device dev;
	struct tally tally = { 0, 0, 0, 0 };
	uint8_t buf[2];
	libusb_testlib_result result;

	result = open_test_device(&dev);
	if (result != TEST_STATUS_SUCCESS)
		return result;

	for (int i = 0; i < dev.iterations; i++) {
		int r = libusb_control_transfer(dev.handle,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_RECIPIENT_DEVICE,
			LIBUSB_REQUEST_GET_STATUS, 0, 0, buf, sizeof(buf), TRANSFER_TIMEOUT_MS);

		if (r == (int)sizeof(buf))
			tally.ok++;
		else
			tally_problem(&tally, i, "GET_STATUS", r);
	}

	close_test_device(&dev);
	return tally_result(&tally, "sync GET_STATUS", (int)sizeof(buf), dev.iterations);
}

/** DFU_GETSTATUS and DFU_GETSTATE on a DFU interface, the class requests
 * dfu-programmer polls between every block of a flash. Skipped when the
 * device has no DFU interface. */
static libusb_testlib_result test_sync_dfu_getstatus(void)
{
	struct test_device dev;
	struct tally tally = { 0, 0, 0, 0 };
	uint8_t status[DFU_GETSTATUS_LENGTH];
	uint8_t state[DFU_GETSTATE_LENGTH];
	libusb_testlib_result result;
	int r;

	result = open_test_device(&dev);
	if (result != TEST_STATUS_SUCCESS)
		return result;

	if (dev.dfu_interface < 0) {
		libusb_testlib_logf("Device has no DFU interface, skipping");
		close_test_device(&dev);
		return TEST_STATUS_SKIP;
	}

	r = libusb_claim_interface(dev.handle, dev.dfu_interface);
	if (r != LIBUSB_SUCCESS) {
		libusb_testlib_logf("Failed to claim DFU interface %d: %s", dev.dfu_interface, libusb_error_name(r));
		close_test_device(&dev);
		return TEST_STATUS_ERROR;
	}

	for (int i = 0; i < dev.iterations; i++) {
		r = libusb_control_transfer(dev.handle,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE,
			DFU_GETSTATUS, 0, (uint16_t)dev.dfu_interface, status, sizeof(status), TRANSFER_TIMEOUT_MS);
		if (r == DFU_GETSTATUS_LENGTH)
			tally.ok++;
		else
			tally_problem(&tally, i, "DFU_GETSTATUS", r);

		/* GETSTATE answers 1 byte; counted in the same tally against the
		 * expected total of two transfers per iteration. */
		r = libusb_control_transfer(dev.handle,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE,
			DFU_GETSTATE, 0, (uint16_t)dev.dfu_interface, state, sizeof(state), TRANSFER_TIMEOUT_MS);
		if (r == DFU_GETSTATE_LENGTH)
			tally.ok++;
		else
			tally_problem(&tally, i, "DFU_GETSTATE", r);
	}

	libusb_release_interface(dev.handle, dev.dfu_interface);
	close_test_device(&dev);

	libusb_testlib_logf("(DFU_GETSTATUS expects %d bytes, DFU_GETSTATE %d; %d of each)",
		DFU_GETSTATUS_LENGTH, DFU_GETSTATE_LENGTH, dev.iterations);
	return tally_result(&tally, "sync DFU_GETSTATUS + DFU_GETSTATE", DFU_GETSTATUS_LENGTH, dev.iterations * 2);
}

/* Fill in the tests */
static const libusb_testlib_test tests[] = {
	{ "sync_get_descriptor", &test_sync_get_descriptor },
	{ "async_get_descriptor", &test_async_get_descriptor },
	{ "sync_get_status", &test_sync_get_status },
	{ "sync_dfu_getstatus", &test_sync_dfu_getstatus },
	LIBUSB_NULL_TEST
};

int main(int argc, const char *argv[])
{
	return libusb_testlib_run_tests(argc, argv, tests);
}
