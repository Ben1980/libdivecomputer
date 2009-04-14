/* 
 * libdivecomputer
 * 
 * Copyright (C) 2008 Jef Driesen
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free
#include <assert.h>	// assert

#include "device-private.h"
#include "suunto_vyper.h"
#include "suunto_common.h"
#include "serial.h"
#include "checksum.h"
#include "array.h"
#include "utils.h"

#define MIN(a,b)	(((a) < (b)) ? (a) : (b))
#define MAX(a,b)	(((a) > (b)) ? (a) : (b))

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

#define HDR_DEVINFO_VYPER   0x24
#define HDR_DEVINFO_SPYDER  0x16
#define HDR_DEVINFO_BEGIN   (HDR_DEVINFO_SPYDER)
#define HDR_DEVINFO_END     (HDR_DEVINFO_VYPER + 6)

#define FP_OFFSET_VYPER		9
#define FP_OFFSET_SPYDER 	7
#define FP_SIZE				5

typedef struct suunto_vyper_device_t {
	device_t base;
	struct serial *port;
	unsigned int delay;
	unsigned char fingerprint[FP_SIZE];
} suunto_vyper_device_t;

static device_status_t suunto_vyper_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size);
static device_status_t suunto_vyper_device_read (device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static device_status_t suunto_vyper_device_write (device_t *abstract, unsigned int address, const unsigned char data[], unsigned int size);
static device_status_t suunto_vyper_device_dump (device_t *abstract, unsigned char data[], unsigned int size, unsigned int *result);
static device_status_t suunto_vyper_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata);
static device_status_t suunto_vyper_device_close (device_t *abstract);

static const device_backend_t suunto_vyper_device_backend = {
	DEVICE_TYPE_SUUNTO_VYPER,
	suunto_vyper_device_set_fingerprint, /* set_fingerprint */
	NULL, /* version */
	suunto_vyper_device_read, /* read */
	suunto_vyper_device_write, /* write */
	suunto_vyper_device_dump, /* dump */
	suunto_vyper_device_foreach, /* foreach */
	suunto_vyper_device_close /* close */
};

static int
device_is_suunto_vyper (device_t *abstract)
{
	if (abstract == NULL)
		return 0;

    return abstract->backend == &suunto_vyper_device_backend;
}


device_status_t
suunto_vyper_device_open (device_t **out, const char* name)
{
	if (out == NULL)
		return DEVICE_STATUS_ERROR;

	// Allocate memory.
	suunto_vyper_device_t *device = (suunto_vyper_device_t *) malloc (sizeof (suunto_vyper_device_t));
	if (device == NULL) {
		WARNING ("Failed to allocate memory.");
		return DEVICE_STATUS_MEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, &suunto_vyper_device_backend);

	// Set the default values.
	device->port = NULL;
	device->delay = 500;
	memset (device->fingerprint, 0, FP_SIZE);

	// Open the device.
	int rc = serial_open (&device->port, name);
	if (rc == -1) {
		WARNING ("Failed to open the serial port.");
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the serial communication protocol (2400 8O1).
	rc = serial_configure (device->port, 2400, 8, SERIAL_PARITY_ODD, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		WARNING ("Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the timeout for receiving data (1000 ms).
	if (serial_set_timeout (device->port, 1000) == -1) {
		WARNING ("Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Set the DTR line (power supply for the interface).
	if (serial_set_dtr (device->port, 1) == -1) {
		WARNING ("Failed to set the DTR line.");
		serial_close (device->port);
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Give the interface 100 ms to settle and draw power up.
	serial_sleep (100);

	// Make sure everything is in a sane state.
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	*out = (device_t*) device;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_vyper_device_close (device_t *abstract)
{
	suunto_vyper_device_t *device = (suunto_vyper_device_t*) abstract;

	if (! device_is_suunto_vyper (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Close the device.
	if (serial_close (device->port) == -1) {
		free (device);
		return DEVICE_STATUS_IO;
	}

	// Free memory.	
	free (device);

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
suunto_vyper_device_set_delay (device_t *abstract, unsigned int delay)
{
	suunto_vyper_device_t *device = (suunto_vyper_device_t*) abstract;

	if (! device_is_suunto_vyper (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	device->delay = delay;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_vyper_device_set_fingerprint (device_t *abstract, const unsigned char data[], unsigned int size)
{
	suunto_vyper_device_t *device = (suunto_vyper_device_t*) abstract;

	if (! device_is_suunto_vyper (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size && size != FP_SIZE)
		return DEVICE_STATUS_ERROR;

	if (size)
		memcpy (device->fingerprint, data, FP_SIZE);
	else
		memset (device->fingerprint, 0, FP_SIZE);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_vyper_send (suunto_vyper_device_t *device, const unsigned char command[], unsigned int csize)
{
	serial_sleep (device->delay);

	// Set RTS to send the command.
	serial_set_rts (device->port, 1);

	// Send the command to the dive computer and 
	// wait until all data has been transmitted.
	serial_write (device->port, command, csize);
	serial_drain (device->port);

	// If the interface sends an echo back (which is the case for many clone 
	// interfaces), this echo should be removed from the input queue before 
	// attempting to read the real reply from the dive computer. Otherwise, 
	// the data transfer will fail. Timing is also critical here! We have to 
	// wait at least until the echo appears (40ms), but not until the reply 
	// from the dive computer appears (600ms).
	// The original suunto interface does not have this problem, because it 
	// does not send an echo and the RTS switching makes it impossible to 
	// receive the reply before RTS is cleared. We have to wait some time 
	// before clearing RTS (around 30ms). But if we wait too long (> 500ms), 
	// the reply disappears again.
	serial_sleep (200);
	serial_flush (device->port, SERIAL_QUEUE_INPUT);

	// Clear RTS to receive the reply.
	serial_set_rts (device->port, 0);

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_vyper_transfer (suunto_vyper_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int size)
{
	assert (asize >= size + 2);

	// Send the command to the dive computer.
	device_status_t rc = suunto_vyper_send (device, command, csize);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Failed to send the command.");
		return rc;
	}

	// Receive the answer of the dive computer.
	int n = serial_read (device->port, answer, asize);
	if (n != asize) {
		WARNING ("Failed to receive the answer.");
		if (n == -1)
			return DEVICE_STATUS_IO;
		return DEVICE_STATUS_TIMEOUT;
	}

	// Verify the header of the package.
	if (memcmp (command, answer, asize - size - 1) != 0) {
		WARNING ("Unexpected answer start byte(s).");
		return DEVICE_STATUS_PROTOCOL;
	}

	// Verify the checksum of the package.
	unsigned char crc = answer[asize - 1];
	unsigned char ccrc = checksum_xor_uint8 (answer, asize - 1, 0x00);
	if (crc != ccrc) {
		WARNING ("Unexpected answer CRC.");
		return DEVICE_STATUS_PROTOCOL;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_vyper_read (device_t *abstract, unsigned int address, unsigned char data[], unsigned int size, device_progress_t *progress)
{
	suunto_vyper_device_t *device = (suunto_vyper_device_t*) abstract;

	if (! device_is_suunto_vyper (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// The data transmission is split in packages
	// of maximum $SUUNTO_VYPER_PACKET_SIZE bytes.

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the package size.
		unsigned int len = MIN (size - nbytes, SUUNTO_VYPER_PACKET_SIZE);

		// Read the package.
		unsigned char answer[SUUNTO_VYPER_PACKET_SIZE + 5] = {0};
		unsigned char command[5] = {0x05,
				(address >> 8) & 0xFF, // high
				(address     ) & 0xFF, // low
				len, // count
				0};  // CRC
		command[4] = checksum_xor_uint8 (command, 4, 0x00);
		device_status_t rc = suunto_vyper_transfer (device, command, sizeof (command), answer, len + 5, len);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

		memcpy (data, answer + 4, len);

#ifndef NDEBUG
		message ("VyperRead(0x%04x,%d)=\"", address, len);
		for (unsigned int i = 0; i < len; ++i) {
			message("%02x", data[i]);
		}
		message("\"\n");
#endif

		// Update and emit a progress event.
		if (progress) {
			progress->current += len;
			device_event_emit (abstract, DEVICE_EVENT_PROGRESS, progress);
		}

		nbytes += len;
		address += len;
		data += len;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_vyper_device_read (device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	return suunto_vyper_read (abstract, address, data, size, NULL);
}


static device_status_t
suunto_vyper_device_write (device_t *abstract, unsigned int address, const unsigned char data[], unsigned int size)
{
	suunto_vyper_device_t *device = (suunto_vyper_device_t*) abstract;

	if (! device_is_suunto_vyper (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// The data transmission is split in packages
	// of maximum $SUUNTO_VYPER_PACKET_SIZE bytes.

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the package size.
		unsigned int len = MIN (size - nbytes, SUUNTO_VYPER_PACKET_SIZE);

		// Prepare to write the package.
		unsigned char panswer[3] = {0};
		unsigned char pcommand[3] = {0x07, 0xA5, 0xA2};
		device_status_t rc = suunto_vyper_transfer (device, pcommand, sizeof (pcommand), panswer, sizeof (panswer), 0);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

#ifndef NDEBUG
		message("VyperPrepareWrite();\n");
#endif

		// Write the package.
		unsigned char wanswer[5] = {0};
		unsigned char wcommand[SUUNTO_VYPER_PACKET_SIZE + 5] = {0x06,
				(address >> 8) & 0xFF, // high
				(address     ) & 0xFF, // low
				len, // count
				0};  // data + CRC
		memcpy (wcommand + 4, data, len);
		wcommand[len + 4] = checksum_xor_uint8 (wcommand, len + 4, 0x00);
		rc = suunto_vyper_transfer (device, wcommand, len + 5, wanswer, sizeof (wanswer), 0);
		if (rc != DEVICE_STATUS_SUCCESS)
			return rc;

#ifndef NDEBUG
		message ("VyperWrite(0x%04x,%d,\"", address, len);
		for (unsigned int i = 0; i < len; ++i) {
			message ("%02x", data[i]);
		}
		message ("\");\n");
#endif

		nbytes += len;
		address += len;
		data += len;
	}

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_vyper_read_dive (device_t *abstract, unsigned char data[], unsigned int size, unsigned int *result, int init, device_progress_t *progress)
{
	suunto_vyper_device_t *device = (suunto_vyper_device_t*) abstract;

	if (! device_is_suunto_vyper (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Send the command to the dive computer.
	unsigned char command[3] = {init ? 0x08 : 0x09, 0xA5, 0x00};
	command[2] = checksum_xor_uint8 (command, 2, 0x00);
	device_status_t rc = suunto_vyper_send (device, command, 3);
	if (rc != DEVICE_STATUS_SUCCESS) {
		WARNING ("Failed to send the command.");
		return rc;
	}

	// The data transmission is split in packages
	// of maximum $SUUNTO_VYPER_PACKET_SIZE bytes.

	unsigned int nbytes = 0;
	for (unsigned int npackages = 0;; ++npackages) {
		// Receive the header of the package.
		unsigned char answer[SUUNTO_VYPER_PACKET_SIZE + 3] = {0};
		int n = serial_read (device->port, answer, 2);
		if (n != 2) {
			// If no data is received because a timeout occured, we assume 
			// the last package was already received and the transmission 
			// can be finished. Unfortunately this is not 100% reliable, 
			// because there is always a small chance that more data will 
			// arrive later (especially with a short timeout). But it works 
			// good enough in practice.
			// Only for the very first package, we can be sure there was 
			// an error, because the DC always sends at least one package.
			if (n == 0 && npackages != 0)
				break;
			WARNING ("Failed to receive the answer.");
			if (n == -1)
				return DEVICE_STATUS_IO;
			return DEVICE_STATUS_TIMEOUT;
		}

		// Verify the header of the package.
		if (answer[0] != command[0] || 
			answer[1] > SUUNTO_VYPER_PACKET_SIZE) {
			WARNING ("Unexpected answer start byte(s).");
			return DEVICE_STATUS_PROTOCOL;
		}

		// Receive the remaining part of the package.
		unsigned char len = answer[1];
		n = serial_read (device->port, answer + 2, len + 1);
		if (n != len + 1) {
			WARNING ("Failed to receive the answer.");
			if (n == -1)
				return DEVICE_STATUS_IO;
			return DEVICE_STATUS_TIMEOUT;
		}

		// Verify the checksum of the package.
		unsigned char crc = answer[len + 2];
		unsigned char ccrc = checksum_xor_uint8 (answer, len + 2, 0x00);
		if (crc != ccrc) {
			WARNING ("Unexpected answer CRC.");
			return DEVICE_STATUS_PROTOCOL;
		}

		// The DC sends a null package (a package with length zero) when it 
		// has reached the end of its internal ring buffer. From this point on, 
		// the current dive has been overwritten with newer data. Therefore, 
		// we discard the current (incomplete) dive and end the transmission.
		if (len == 0) {
			WARNING ("Null package received.");
			if (result)
				*result = 0;
			return DEVICE_STATUS_SUCCESS;
		}

		// Update and emit a progress event.
		if (progress) {
			progress->current += len;
			device_event_emit (abstract, DEVICE_EVENT_PROGRESS, progress);
		}

		// Append the package to the output buffer.
		// Reporting of buffer overflows is delayed until the entire
		// transfer is finished. This approach leaves no data behind in
		// the serial receive buffer, and if this packet is part of the
		// last incomplete dive, no error has to be reported at all.
		if (size >= nbytes + len)
			memcpy (data + nbytes, answer + 2, len);
		nbytes += len;

		// If a package is smaller than $SUUNTO_VYPER_PACKET_SIZE bytes, 
		// we assume it's the last packet and the transmission can be 
		// finished early. However, this approach does not work if the 
		// last packet is exactly $SUUNTO_VYPER_PACKET_SIZE bytes long!
#if 0
		if (len != SUUNTO_VYPER_PACKET_SIZE) 
			break;
#endif
	}

	// Check for a buffer overflow.
	if (size < nbytes) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// The DC traverses its internal ring buffer backwards. The most recent 
	// dive is send first (which allows you to download only the new dives), 
	// but also the contents of each dive is reversed. Therefore, we reverse 
	// the bytes again before returning them to the application.
	array_reverse_bytes (data, nbytes);

#ifndef NDEBUG
	message ("Vyper%sProfile=\"", init ? "First" : "");
	for (unsigned int i = 0; i < nbytes; ++i) {
		message("%02x", data[i]);
	}
	message("\"\n");
#endif

	if (result)
		*result = nbytes;

	return DEVICE_STATUS_SUCCESS;
}


device_status_t
suunto_vyper_device_read_dive (device_t *abstract, unsigned char data[], unsigned int size, unsigned int *result, int init)
{
	return suunto_vyper_read_dive (abstract, data, size, result, init, NULL);
}


static device_status_t
suunto_vyper_device_dump (device_t *abstract, unsigned char data[], unsigned int size, unsigned int *result)
{
	if (! device_is_suunto_vyper (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	if (size < SUUNTO_VYPER_MEMORY_SIZE) {
		WARNING ("Insufficient buffer space available.");
		return DEVICE_STATUS_MEMORY;
	}

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = SUUNTO_VYPER_MEMORY_SIZE;
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	device_status_t rc = suunto_vyper_read (abstract, 0x00, data, SUUNTO_VYPER_MEMORY_SIZE, &progress);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	if (result)
		*result = SUUNTO_VYPER_MEMORY_SIZE;

	return DEVICE_STATUS_SUCCESS;
}


static device_status_t
suunto_vyper_device_foreach (device_t *abstract, dive_callback_t callback, void *userdata)
{
	suunto_vyper_device_t *device = (suunto_vyper_device_t*) abstract;

	if (! device_is_suunto_vyper (abstract))
		return DEVICE_STATUS_TYPE_MISMATCH;

	// Enable progress notifications.
	device_progress_t progress = DEVICE_PROGRESS_INITIALIZER;
	progress.maximum = (SUUNTO_VYPER_MEMORY_SIZE - 0x4C) +
		(HDR_DEVINFO_END - HDR_DEVINFO_BEGIN);
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Read the device info. The Vyper and the Spyder store this data
	// in a different location. To minimize the number of (slow) reads,
	// we read a larger block of memory that always contains the data
	// for both devices.
	unsigned char header[HDR_DEVINFO_END - HDR_DEVINFO_BEGIN] = {0};
	device_status_t rc = suunto_vyper_read (abstract, HDR_DEVINFO_BEGIN, header, sizeof (header), NULL);
	if (rc != DEVICE_STATUS_SUCCESS)
		return rc;

	// Identify the connected device as a Vyper or a Spyder, by inspecting
	// the Vyper model code. For a Spyder, this value will contain the
	// sample interval (20, 30 or 60s) instead of the model code.
	unsigned int vyper = 1;
	unsigned int hoffset = HDR_DEVINFO_VYPER - HDR_DEVINFO_BEGIN;
	if (header[hoffset] == 20 || header[hoffset] == 30 || header[hoffset] == 60) {
		vyper = 0;
		hoffset = HDR_DEVINFO_SPYDER - HDR_DEVINFO_BEGIN;
	}

	// Update and emit a progress event.
	if (vyper)
		progress.maximum -= 0x71 - 0x4C;
	progress.current += sizeof (header);
	device_event_emit (abstract, DEVICE_EVENT_PROGRESS, &progress);

	// Emit a device info event.
	device_devinfo_t devinfo;
	devinfo.model = header[hoffset + 0];
	devinfo.firmware = header[hoffset + 1];
	devinfo.serial = array_uint32_be (header + hoffset + 2);
	device_event_emit (abstract, DEVICE_EVENT_DEVINFO, &devinfo);

	// The memory layout of the Spyder is different from the Vyper
	// (and all other compatible dive computers). The Spyder has
	// the largest ring buffer for the profile memory, so we use
	// that value as the maximum size of the memory buffer.

	unsigned char data[SUUNTO_VYPER_MEMORY_SIZE - 0x4C] = {0};

	unsigned int ndives = 0;
	unsigned int offset = 0;
	unsigned int nbytes = 0;
	while ((rc = suunto_vyper_read_dive (abstract, data + offset, sizeof (data) - offset, &nbytes, (ndives == 0), &progress)) == DEVICE_STATUS_SUCCESS) {
		if (nbytes == 0)
			return DEVICE_STATUS_SUCCESS;

		unsigned int fp_offset = (vyper ? FP_OFFSET_VYPER : FP_OFFSET_SPYDER);
		if (memcmp (data + offset + fp_offset, device->fingerprint, FP_SIZE) == 0)
			return DEVICE_STATUS_SUCCESS;

		if (callback && !callback (data + offset, nbytes, userdata))
			return DEVICE_STATUS_SUCCESS;

		ndives++;
		offset += nbytes;
	}

	return rc;
}


static int
fp_compare_vyper (device_t *abstract, const unsigned char data[], unsigned int size)
{
	suunto_vyper_device_t *device = (suunto_vyper_device_t*) abstract;

	return memcmp (data + FP_OFFSET_VYPER, device->fingerprint, FP_SIZE);
}


static int
fp_compare_spyder (device_t *abstract, const unsigned char data[], unsigned int size)
{
	suunto_vyper_device_t *device = (suunto_vyper_device_t*) abstract;

	return memcmp (data + FP_OFFSET_SPYDER, device->fingerprint, FP_SIZE);
}


device_status_t
suunto_vyper_extract_dives (device_t *abstract, const unsigned char data[], unsigned int size, dive_callback_t callback, void *userdata)
{
	assert (size >= SUUNTO_VYPER_MEMORY_SIZE);

	unsigned int vyper = 1;
	if (data[HDR_DEVINFO_VYPER] == 20 || data[HDR_DEVINFO_VYPER] == 30 || data[HDR_DEVINFO_VYPER] == 60)
		vyper = 0;

	if (vyper) {
		unsigned int eop = array_uint16_be (data + 0x51);
		return suunto_common_extract_dives (abstract, data, 0x71, SUUNTO_VYPER_MEMORY_SIZE, eop, 5, fp_compare_vyper, callback, userdata);
	} else {
		unsigned int eop = array_uint16_be (data + 0x1C);
		return suunto_common_extract_dives (abstract, data, 0x4C, SUUNTO_VYPER_MEMORY_SIZE, eop, 3, fp_compare_spyder, callback, userdata);
	}
}
