/*
 * Copyright 2011 Michael Lotz <mmlr@mlotz.ch>
 * Distributed under the terms of the MIT license.
 */


//!	Driver for USB Human Interface Devices.


#include "Driver.h"
#include "JoystickProtocolHandler.h"

#include "HIDCollection.h"
#include "HIDDevice.h"
#include "HIDReport.h"
#include "HIDReportItem.h"

#include <new>
#include <string.h>
#include <usb/USB_hid.h>


JoystickProtocolHandler::JoystickProtocolHandler(HIDReport &report)
	:
	ProtocolHandler(report.Device(), "joystick/usb/", 512),
	fReport(report)
{
	for (uint32 i = 0; i < MAX_AXES; i++)
		fAxis[i] = NULL;

	uint32 buttonCount = 0;
	for (uint32 i = 0; i < report.CountItems(); i++) {
		HIDReportItem *item = report.ItemAt(i);
		if (!item->HasData())
			continue;

		switch (item->UsagePage()) {
			case B_HID_USAGE_PAGE_BUTTON:
			{
				if (item->UsageID() - 1 < MAX_BUTTONS)
					fButtons[buttonCount++] = item;
				break;
			}

			case B_HID_USAGE_PAGE_GENERIC_DESKTOP:
			{
				switch (item->UsageID()) {
					case B_HID_UID_GD_X:
					case B_HID_UID_GD_Y:
					case B_HID_UID_GD_Z:
					case B_HID_UID_GD_RX:
					case B_HID_UID_GD_RY:
					case B_HID_UID_GD_RZ:
						uint16 axis = item->UsageID() - B_HID_UID_GD_X;
						if (axis >= MAX_AXES)
							break;

						fAxis[axis] = item;
						break;
				}

				break;
			}
		}
	}

	fButtons[buttonCount] = NULL;

	TRACE("joystick device with %lu buttons\n", buttonCount);
	TRACE("report id: %u\n", report.ID());
}


void
JoystickProtocolHandler::AddHandlers(HIDDevice &device,
	HIDCollection &collection, ProtocolHandler *&handlerList)
{
	if (collection.UsagePage() != B_HID_USAGE_PAGE_GENERIC_DESKTOP
		|| (collection.UsageID() != B_HID_UID_GD_JOYSTICK
			&& collection.UsageID() != B_HID_UID_GD_GAMEPAD
			&& collection.UsageID() != B_HID_UID_GD_MULTIAXIS)) {
		TRACE("collection not a joystick or gamepad\n");
		return;
	}

	HIDParser &parser = device.Parser();
	uint32 maxReportCount = parser.CountReports(HID_REPORT_TYPE_INPUT);
	if (maxReportCount == 0)
		return;

	uint32 inputReportCount = 0;
	HIDReport *inputReports[maxReportCount];
	collection.BuildReportList(HID_REPORT_TYPE_INPUT, inputReports,
		inputReportCount);

	for (uint32 i = 0; i < inputReportCount; i++) {
		HIDReport *inputReport = inputReports[i];

		// try to find at least one axis
		bool foundAxis = false;
		for (uint32 j = 0; j < inputReport->CountItems(); j++) {
			HIDReportItem *item = inputReport->ItemAt(j);
			if (item == NULL || !item->HasData())
				continue;

			if (item->UsagePage() != B_HID_USAGE_PAGE_GENERIC_DESKTOP)
				continue;

			if (item->UsageID() >= B_HID_UID_GD_X
				&& item->UsageID() <= B_HID_UID_GD_RZ) {
				foundAxis = true;
				break;
			}
		}

		if (!foundAxis)
			continue;

		ProtocolHandler *newHandler
			= new(std::nothrow) JoystickProtocolHandler(*inputReport);
		if (newHandler == NULL) {
			TRACE("failed to allocated joystick protocol handler\n");
			continue;
		}

		newHandler->SetNextHandler(handlerList);
		handlerList = newHandler;
	}
}


status_t
JoystickProtocolHandler::Read(uint32 *cookie, off_t position, void *buffer,
	size_t *numBytes)
{
	if (*numBytes < sizeof(extended_joystick))
		return B_BUFFER_OVERFLOW;

	while (RingBufferReadable() == 0) {
		status_t result = _ReadReport();
		if (result != B_OK)
			return result;
	}

	status_t result = RingBufferRead(buffer, sizeof(extended_joystick));
	if (result != B_OK)
		return result;

	*numBytes = sizeof(extended_joystick);
	return B_OK;
}


status_t
JoystickProtocolHandler::Write(uint32 *cookie, off_t position,
	const void *buffer, size_t *numBytes)
{
	*numBytes = 0;
	return B_NOT_SUPPORTED;
}


status_t
JoystickProtocolHandler::Control(uint32 *cookie, uint32 op, void *buffer,
	size_t length)
{
	switch (op) {
		case B_JOYSTICK_SET_DEVICE_MODULE:
		{
			if (length < sizeof(joystick_module_info))
				return B_BAD_VALUE;

			fJoystickModuleInfo = *(joystick_module_info *)buffer;

			fJoystickModuleInfo.num_axes = 0;
			for (uint32 i = 0; i < MAX_AXES; i++) {
				if (fAxis[i] != NULL)
					fJoystickModuleInfo.num_axes = i + 1;
			}

			fJoystickModuleInfo.num_buttons = 0;
			for (uint32 i = 0; i < MAX_BUTTONS; i++) {
				if (fButtons[i] == NULL)
					break;

				uint8 button = fButtons[i]->UsageID();
				if (button > fJoystickModuleInfo.num_buttons)
					fJoystickModuleInfo.num_buttons = button;
			}

			fJoystickModuleInfo.num_hats = 0;
			fJoystickModuleInfo.num_sticks = 1;
			fJoystickModuleInfo.config_size = 0;
			break;
		}

		case B_JOYSTICK_GET_DEVICE_MODULE:
			if (length < sizeof(joystick_module_info))
				return B_BAD_VALUE;

			*(joystick_module_info *)buffer = fJoystickModuleInfo;
			break;
	}

	return B_ERROR;
}


status_t
JoystickProtocolHandler::_ReadReport()
{
	status_t result = fReport.WaitForReport(B_INFINITE_TIMEOUT);
	if (result != B_OK) {
		if (fReport.Device()->IsRemoved()) {
			TRACE("device has been removed\n");
			return B_DEV_NOT_READY;
		}

		if (result != B_INTERRUPTED) {
			// interrupts happen when other reports come in on the same
			// input as ours
			TRACE_ALWAYS("error waiting for report: %s\n", strerror(result));
		}

		// signal that we simply want to try again
		return B_OK;
	}

	extended_joystick info;
	memset(&info, 0, sizeof(info));

	for (uint32 i = 0; i < MAX_AXES; i++) {
		if (fAxis[i] == NULL)
			continue;

		if (fAxis[i]->Extract() == B_OK && fAxis[i]->Valid())
			info.axes[i] = (int16)fAxis[i]->ScaledData(16, true);
	}

	for (uint32 i = 0; i < MAX_BUTTONS; i++) {
		HIDReportItem *button = fButtons[i];
		if (button == NULL)
			break;

		if (button->Extract() == B_OK && button->Valid())
			info.buttons |= (button->Data() & 1) << (button->UsageID() - 1);
	}

	fReport.DoneProcessing();
	TRACE("got joystick report\n");

	info.timestamp = system_time();
	return RingBufferWrite(&info, sizeof(info));
}