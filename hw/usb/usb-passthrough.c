#include <libusb.h>
#include <stdbool.h>
#include "glib-compat.h"
#include "hw/usb-passthrough.h"
#include "host-libusb.h"

/*
 * XEMU USB PASSTHROUGH API
 *
 * Copyright (c) 2023 Fred Hallock
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// ...from ui/xemu-input.h
void xemu_input_bind_passthrough(int index, LibusbDevice *device, int save);

typedef struct known_libusb_device {
    unsigned short vendor_id;
    unsigned short product_id;
    const char *name;
    int hub_ports;
} known_libusb_device;

const char *unknownDeviceName = "Unknown Device";

known_libusb_device wellKnownDevices[] = {
    { 0x0d2f, 0x0002, "Andamiro Pump It Up pad", 3 },
	{ 0x045e, 0x0202, "Xbox Controller", 3 },
    { 0x045e, 0x0285, "Xbox Controller S", 3 },
    { 0x045e, 0x0287, "Xbox Controller S", 3 },
    { 0x045e, 0x0289, "Xbox Controller S", 3 },
    { 0x046d, 0xca84, "Logitech Xbox Cordless Controller", 3 },
    { 0x046d, 0xca88, "Logitech Compact Controller for Xbox", 3 },
    { 0x05fd, 0x1007, "Mad Catz Controller (unverified)", 3 },
    { 0x05fd, 0x107a, "InterAct 'PowerPad Pro' X-Box pad (Germany)", 3 },
    { 0x0738, 0x4516, "Mad Catz Control Pad", 3 },
    { 0x0738, 0x4522, "Mad Catz LumiCON", 3 },
    { 0x0738, 0x4526, "Mad Catz Control Pad Pro", 3 },
    { 0x0738, 0x4536, "Mad Catz MicroCON", 3 },
    { 0x0738, 0x4556, "Mad Catz Lynx Wireless Controller", 3 },
    { 0x0c12, 0x8802, "Zeroplus Xbox Controller", 3 },
    { 0x0c12, 0x8810, "Zeroplus Xbox Controller", 3 },
    { 0x0c12, 0x9902, "HAMA VibraX - *FAULTY HARDWARE*", 3 },
    { 0x0e4c, 0x1097, "Radica Gamester Controller", 3 },
    { 0x0e4c, 0x2390, "Radica Games Jtech Controller", 3 },
    { 0x0e6f, 0x0003, "Logic3 Freebird wireless Controller", 3 },
    { 0x0e6f, 0x0005, "Eclipse wireless Controller", 3 },
    { 0x0e6f, 0x0006, "Edge wireless Controller", 3 },
    { 0x0f30, 0x0202, "Joytech Advanced Controller", 3 },
    { 0x0f30, 0x8888, "BigBen XBMiniPad Controller", 3 },
    { 0x102c, 0xff0c, "Joytech Wireless Advanced Controller", 3 },
    { 0x044f, 0x0f07, "Thrustmaster, Inc. Controller", 3 },
    { 0x0e8f, 0x3008, "Generic xbox control (dealextreme)", 3 },
    { 0x0a7b, 0xd000, "Steel Battalion Controller", 0 },
    { 0x0f0d, 0x0001, "HORI Fight Stick", 2 }
};

#define NUM_KNOWN_XID_DEVICES ARRAY_SIZE(wellKnownDevices)

LibusbDeviceList available_libusb_devices =
    QTAILQ_HEAD_INITIALIZER(available_libusb_devices);

void get_libusb_devices(void)
{
    libusb_device **devs = NULL;
    struct libusb_device_descriptor ddesc;
    unsigned int bus;
    unsigned short vendor_id, product_id;
    char port[16];
    int i, j, n, hub_ports;
    const char *name;
    LibusbDevice *iter;
    bool previously_detected;

    if (usb_host_init() != 0) {
        return;
    }

    QTAILQ_FOREACH(iter, &available_libusb_devices, entry) {
        iter->detected = false;
    }

    n = libusb_get_device_list(ctx, &devs);
    for (i = 0; i < n; i++) {
        if (libusb_get_device_descriptor(devs[i], &ddesc) != 0) {
            continue;
        }
        if (ddesc.bDeviceClass == LIBUSB_CLASS_HUB) {
            continue;
        }

        name = unknownDeviceName;

        usb_host_get_port(devs[i], port, sizeof(port));
        bus = libusb_get_bus_number(devs[i]);
        vendor_id = ddesc.idVendor;
        product_id = ddesc.idProduct;

        previously_detected = false;
        // We already know about this one
        QTAILQ_FOREACH(iter, &available_libusb_devices, entry) {
            if (iter->vendor_id == vendor_id &&
                iter->product_id == product_id &&
                iter->host_bus == bus &&
                strcmp(iter->host_port, port) == 0) {
                previously_detected = true;
                iter->detected = true;
            }
        }

        if(previously_detected)
            continue;

        for(j = 0; j < NUM_KNOWN_XID_DEVICES; j++) {
            if (wellKnownDevices[j].vendor_id == vendor_id && 
                wellKnownDevices[j].product_id == product_id) {
                name = wellKnownDevices[j].name;
                hub_ports = wellKnownDevices[j].hub_ports;
                break;
            }
        }

        // Skip any devices we don't already know about
        if(name == unknownDeviceName)
            continue;

        LibusbDevice *device = g_malloc(sizeof(LibusbDevice));
        memset(device, 0, sizeof(LibusbDevice));
        device->vendor_id = vendor_id;
        device->product_id = product_id;
        device->host_bus = bus;
        device->host_port = g_strdup(port);
        device->name = name;
        device->bound = -1;
        device->detected = true;
        device->internal_hub_ports = hub_ports;

        QTAILQ_INSERT_TAIL(&available_libusb_devices, device, entry);
    }

    // Remove any devices that aren't detected anymore
    QTAILQ_FOREACH(iter, &available_libusb_devices, entry) {
        if(!iter->detected) {
            if(iter->bound) {
                xemu_input_bind_passthrough(iter->bound, NULL, 1);
            }
            QTAILQ_REMOVE(&available_libusb_devices, iter, entry);
        }
    }

    libusb_free_device_list(devs, 1);
}

LibusbDevice *find_libusb_device(int host_bus, const char *host_port)
{
    LibusbDevice *iter;
    QTAILQ_FOREACH(iter, &available_libusb_devices, entry) {
        if (iter->host_bus == host_bus &&
            strcmp(iter->host_port, host_port) == 0) {
            return iter;
        }
    }
    return NULL;
}
