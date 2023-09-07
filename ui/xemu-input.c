/*
 * xemu Input Management
 *
 * Copyright (C) 2020-2021 Matt Borgerson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "qemu/osdep.h"
#include "hw/usb.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "monitor/qdev.h"
#include "qapi/qmp/qdict.h"
#include "qemu/option.h"
#include "qemu/timer.h"
#include "qemu/config-file.h"

#include "xemu-input.h"
#include "xemu-notifications.h"
#include "xemu-settings.h"

// #define DEBUG_INPUT

#ifdef DEBUG_INPUT
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

#define XEMU_INPUT_MIN_INPUT_UPDATE_INTERVAL_US  2500
#define XEMU_INPUT_MIN_RUMBLE_UPDATE_INTERVAL_US 2500

#if 0
static void xemu_input_print_controller_state(ControllerState *state)
{
    DPRINTF("     A = %d,      B = %d,     X = %d,     Y = %d\n"
           "  Left = %d,     Up = %d, Right = %d,  Down = %d\n"
           "  Back = %d,  Start = %d, White = %d, Black = %d\n"
           "Lstick = %d, Rstick = %d, Guide = %d\n"
           "\n"
           "LTrig   = %.3f, RTrig   = %.3f\n"
           "LStickX = %.3f, RStickX = %.3f\n"
           "LStickY = %.3f, RStickY = %.3f\n\n",
        !!(state->buttons & CONTROLLER_BUTTON_A),
        !!(state->buttons & CONTROLLER_BUTTON_B),
        !!(state->buttons & CONTROLLER_BUTTON_X),
        !!(state->buttons & CONTROLLER_BUTTON_Y),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_LEFT),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_UP),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_RIGHT),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_DOWN),
        !!(state->buttons & CONTROLLER_BUTTON_BACK),
        !!(state->buttons & CONTROLLER_BUTTON_START),
        !!(state->buttons & CONTROLLER_BUTTON_WHITE),
        !!(state->buttons & CONTROLLER_BUTTON_BLACK),
        !!(state->buttons & CONTROLLER_BUTTON_LSTICK),
        !!(state->buttons & CONTROLLER_BUTTON_RSTICK),
        !!(state->buttons & CONTROLLER_BUTTON_GUIDE),
        state->axis[CONTROLLER_AXIS_LTRIG],
        state->axis[CONTROLLER_AXIS_RTRIG],
        state->axis[CONTROLLER_AXIS_LSTICK_X],
        state->axis[CONTROLLER_AXIS_RSTICK_X],
        state->axis[CONTROLLER_AXIS_LSTICK_Y],
        state->axis[CONTROLLER_AXIS_RSTICK_Y]
        );
}
#endif

ControllerStateList available_controllers =
    QTAILQ_HEAD_INITIALIZER(available_controllers);

ControllerState *bound_controllers[4] = { NULL, NULL, NULL, NULL };
LibusbDevice *bound_libusb_devices[4] = { NULL, NULL, NULL, NULL };
const char *all_drivers[5] = { DRIVER_DUKE, DRIVER_S, DRIVER_SB, DRIVER_FIGHT_STICK, DRIVER_USB_PASSTHROUGH };
const char *bound_drivers[4] = { DRIVER_DUKE, DRIVER_DUKE, DRIVER_DUKE, DRIVER_DUKE };
int test_mode;

static const char **port_index_to_settings_key_map[] = {
    &g_config.input.bindings.port1,
    &g_config.input.bindings.port2,
    &g_config.input.bindings.port3,
    &g_config.input.bindings.port4,
};

static const char **port_index_to_driver_settings_key_map[] = {
    &g_config.input.bindings.port1_driver,
    &g_config.input.bindings.port2_driver,
    &g_config.input.bindings.port3_driver,
    &g_config.input.bindings.port4_driver
};

static int sdl_kbd_scancode_map[25];
static int sdl_sbc_kbd_scancode_map[56];

const char *get_bound_driver(int port) {
    assert(port >= 0 && port <= 3);
    const char *driver = NULL;    

    driver = *port_index_to_driver_settings_key_map[port];

    if(driver == NULL)
        return DRIVER_DUKE; // Shouldn't be possible
    if(strlen(driver) == 0)
        return DRIVER_DUKE;

    for(int i = 0; i < 5; i++) {
        if(strcmp(driver, all_drivers[i]) == 0)
            return all_drivers[i];
    }
    
    // Shouldn't be possible
    assert(false);
}

void xemu_input_init(void)
{
    if (g_config.input.background_input_capture) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }

    if (SDL_Init(SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "Failed to initialize SDL gamecontroller subsystem: %s\n", SDL_GetError());
        exit(1);
    }

    // Create the keyboard input (always first)
    ControllerState *new_con = malloc(sizeof(ControllerState));
    memset(new_con, 0, sizeof(ControllerState));
    new_con->type = INPUT_DEVICE_SDL_KEYBOARD;
    new_con->name = "Keyboard";
    new_con->bound = -1;

    sdl_kbd_scancode_map[0] = g_config.input.keyboard_controller_scancode_map.a;
    sdl_kbd_scancode_map[1] = g_config.input.keyboard_controller_scancode_map.b;
    sdl_kbd_scancode_map[2] = g_config.input.keyboard_controller_scancode_map.x;
    sdl_kbd_scancode_map[3] = g_config.input.keyboard_controller_scancode_map.y;
    sdl_kbd_scancode_map[4] = g_config.input.keyboard_controller_scancode_map.dpad_left;
    sdl_kbd_scancode_map[5] = g_config.input.keyboard_controller_scancode_map.dpad_up;
    sdl_kbd_scancode_map[6] = g_config.input.keyboard_controller_scancode_map.dpad_right;
    sdl_kbd_scancode_map[7] = g_config.input.keyboard_controller_scancode_map.dpad_down;
    sdl_kbd_scancode_map[8] = g_config.input.keyboard_controller_scancode_map.back;
    sdl_kbd_scancode_map[9] = g_config.input.keyboard_controller_scancode_map.start;
    sdl_kbd_scancode_map[10] = g_config.input.keyboard_controller_scancode_map.white;
    sdl_kbd_scancode_map[11] = g_config.input.keyboard_controller_scancode_map.black;
    sdl_kbd_scancode_map[12] = g_config.input.keyboard_controller_scancode_map.lstick_btn;
    sdl_kbd_scancode_map[13] = g_config.input.keyboard_controller_scancode_map.rstick_btn;
    sdl_kbd_scancode_map[14] = g_config.input.keyboard_controller_scancode_map.guide;
    sdl_kbd_scancode_map[15] = g_config.input.keyboard_controller_scancode_map.lstick_up;
    sdl_kbd_scancode_map[16] = g_config.input.keyboard_controller_scancode_map.lstick_left;
    sdl_kbd_scancode_map[17] = g_config.input.keyboard_controller_scancode_map.lstick_right;
    sdl_kbd_scancode_map[18] = g_config.input.keyboard_controller_scancode_map.lstick_down;
    sdl_kbd_scancode_map[19] = g_config.input.keyboard_controller_scancode_map.ltrigger;
    sdl_kbd_scancode_map[20] = g_config.input.keyboard_controller_scancode_map.rstick_up;
    sdl_kbd_scancode_map[21] = g_config.input.keyboard_controller_scancode_map.rstick_left;
    sdl_kbd_scancode_map[22] = g_config.input.keyboard_controller_scancode_map.rstick_right;
    sdl_kbd_scancode_map[23] = g_config.input.keyboard_controller_scancode_map.rstick_down;
    sdl_kbd_scancode_map[24] = g_config.input.keyboard_controller_scancode_map.rtrigger;

    for (int i = 0; i < 25; i++) {
        if( (sdl_kbd_scancode_map[i] < SDL_SCANCODE_UNKNOWN) ||
            (sdl_kbd_scancode_map[i] >= SDL_NUM_SCANCODES) ) {
            fprintf(stderr, "WARNING: Keyboard controller map scancode out of range (%d) : Disabled\n", sdl_kbd_scancode_map[i]);
            sdl_kbd_scancode_map[i] = SDL_SCANCODE_UNKNOWN;
        }
    }

    sdl_sbc_kbd_scancode_map[0] = g_config.input.keyboard_sbc_scancode_map.main_weapon;
    sdl_sbc_kbd_scancode_map[1] = g_config.input.keyboard_sbc_scancode_map.sub_weapon;
    sdl_sbc_kbd_scancode_map[2] = g_config.input.keyboard_sbc_scancode_map.lock_on;
    sdl_sbc_kbd_scancode_map[3] = g_config.input.keyboard_sbc_scancode_map.eject;
    sdl_sbc_kbd_scancode_map[4] = g_config.input.keyboard_sbc_scancode_map.cockpit_hatch;
    sdl_sbc_kbd_scancode_map[5] = g_config.input.keyboard_sbc_scancode_map.ignition;
    sdl_sbc_kbd_scancode_map[6] = g_config.input.keyboard_sbc_scancode_map.start;
    sdl_sbc_kbd_scancode_map[7] = g_config.input.keyboard_sbc_scancode_map.open_close;
    sdl_sbc_kbd_scancode_map[8] = g_config.input.keyboard_sbc_scancode_map.map_zoom_in_out;
    sdl_sbc_kbd_scancode_map[9] = g_config.input.keyboard_sbc_scancode_map.mode_select;
    sdl_sbc_kbd_scancode_map[10] = g_config.input.keyboard_sbc_scancode_map.sub_monitor_mode_select;
    sdl_sbc_kbd_scancode_map[11] = g_config.input.keyboard_sbc_scancode_map.zoom_in;
    sdl_sbc_kbd_scancode_map[12] = g_config.input.keyboard_sbc_scancode_map.zoom_out;
    sdl_sbc_kbd_scancode_map[13] = g_config.input.keyboard_sbc_scancode_map.fss;
    sdl_sbc_kbd_scancode_map[14] = g_config.input.keyboard_sbc_scancode_map.manipulator;
    sdl_sbc_kbd_scancode_map[15] = g_config.input.keyboard_sbc_scancode_map.line_color_change;
    sdl_sbc_kbd_scancode_map[16] = g_config.input.keyboard_sbc_scancode_map.washing;
    sdl_sbc_kbd_scancode_map[17] = g_config.input.keyboard_sbc_scancode_map.extinguisher;
    sdl_sbc_kbd_scancode_map[18] = g_config.input.keyboard_sbc_scancode_map.chaff;
    sdl_sbc_kbd_scancode_map[19] = g_config.input.keyboard_sbc_scancode_map.tank_detach;
    sdl_sbc_kbd_scancode_map[20] = g_config.input.keyboard_sbc_scancode_map.override;
    sdl_sbc_kbd_scancode_map[21] = g_config.input.keyboard_sbc_scancode_map.night_scope;
    sdl_sbc_kbd_scancode_map[22] = g_config.input.keyboard_sbc_scancode_map.func1;
    sdl_sbc_kbd_scancode_map[23] = g_config.input.keyboard_sbc_scancode_map.func2;
    sdl_sbc_kbd_scancode_map[24] = g_config.input.keyboard_sbc_scancode_map.func3;
    sdl_sbc_kbd_scancode_map[25] = g_config.input.keyboard_sbc_scancode_map.main_weapon_control;
    sdl_sbc_kbd_scancode_map[26] = g_config.input.keyboard_sbc_scancode_map.sub_weapon_control;
    sdl_sbc_kbd_scancode_map[27] = g_config.input.keyboard_sbc_scancode_map.magazine_change;
    sdl_sbc_kbd_scancode_map[28] = g_config.input.keyboard_sbc_scancode_map.com1;
    sdl_sbc_kbd_scancode_map[29] = g_config.input.keyboard_sbc_scancode_map.com2;
    sdl_sbc_kbd_scancode_map[30] = g_config.input.keyboard_sbc_scancode_map.com3;
    sdl_sbc_kbd_scancode_map[31] = g_config.input.keyboard_sbc_scancode_map.com4;
    sdl_sbc_kbd_scancode_map[32] = g_config.input.keyboard_sbc_scancode_map.com5;         
    sdl_sbc_kbd_scancode_map[33] = g_config.input.keyboard_sbc_scancode_map.sight_change;
    sdl_sbc_kbd_scancode_map[34] = g_config.input.keyboard_sbc_scancode_map.filt_control_system;
    sdl_sbc_kbd_scancode_map[35] = g_config.input.keyboard_sbc_scancode_map.oxygen_supply_system;
    sdl_sbc_kbd_scancode_map[36] = g_config.input.keyboard_sbc_scancode_map.fuel_flow_rate;
    sdl_sbc_kbd_scancode_map[37] = g_config.input.keyboard_sbc_scancode_map.buffer_material;
    sdl_sbc_kbd_scancode_map[38] = g_config.input.keyboard_sbc_scancode_map.vt_location_measurement;
    sdl_sbc_kbd_scancode_map[39] = g_config.input.keyboard_sbc_scancode_map.gear_up;
    sdl_sbc_kbd_scancode_map[40] = g_config.input.keyboard_sbc_scancode_map.gear_down;
    sdl_sbc_kbd_scancode_map[41] = g_config.input.keyboard_sbc_scancode_map.tuner_left;
    sdl_sbc_kbd_scancode_map[42] = g_config.input.keyboard_sbc_scancode_map.tuner_right;
    sdl_sbc_kbd_scancode_map[43] = g_config.input.keyboard_sbc_scancode_map.aiming_up;
    sdl_sbc_kbd_scancode_map[44] = g_config.input.keyboard_sbc_scancode_map.aiming_down;
    sdl_sbc_kbd_scancode_map[45] = g_config.input.keyboard_sbc_scancode_map.aiming_left;
    sdl_sbc_kbd_scancode_map[46] = g_config.input.keyboard_sbc_scancode_map.aiming_right;
    sdl_sbc_kbd_scancode_map[47] = g_config.input.keyboard_sbc_scancode_map.sight_change_up;
    sdl_sbc_kbd_scancode_map[48] = g_config.input.keyboard_sbc_scancode_map.sight_change_down;
    sdl_sbc_kbd_scancode_map[49] = g_config.input.keyboard_sbc_scancode_map.sight_change_left;
    sdl_sbc_kbd_scancode_map[50] = g_config.input.keyboard_sbc_scancode_map.sight_change_right;
    sdl_sbc_kbd_scancode_map[51] = g_config.input.keyboard_sbc_scancode_map.rotation_left;
    sdl_sbc_kbd_scancode_map[52] = g_config.input.keyboard_sbc_scancode_map.rotation_right;
    sdl_sbc_kbd_scancode_map[53] = g_config.input.keyboard_sbc_scancode_map.left_pedal;
    sdl_sbc_kbd_scancode_map[54] = g_config.input.keyboard_sbc_scancode_map.right_pedal;
    sdl_sbc_kbd_scancode_map[55] = g_config.input.keyboard_sbc_scancode_map.middle_pedal;

    for (int i = 0; i < 56; i++) {
        if( (sdl_sbc_kbd_scancode_map[i] < SDL_SCANCODE_UNKNOWN) ||
            (sdl_sbc_kbd_scancode_map[i] >= SDL_NUM_SCANCODES) ) {
            fprintf(stderr, "WARNING: Keyboard steel battalion controller map scancode out of range (%d) : Disabled\n", sdl_sbc_kbd_scancode_map[i]);
            sdl_sbc_kbd_scancode_map[i] = SDL_SCANCODE_UNKNOWN;
        }
    }

    bound_drivers[0] = get_bound_driver(0);
    bound_drivers[1] = get_bound_driver(1);
    bound_drivers[2] = get_bound_driver(2);
    bound_drivers[3] = get_bound_driver(3);

    get_libusb_devices();

    LibusbDevice *iter;

    LibusbDevice *devices_to_bind[4] = { NULL, NULL, NULL, NULL };

    QTAILQ_FOREACH(iter, &available_libusb_devices, entry) {
        if(iter->bound < 0) {
            int p = xemu_input_get_libusb_device_default_bind_port(iter, 0);
            if(p >= 0) {
                devices_to_bind[p] = iter;
            }
        }
    }

    for(int i = 0; i < 4; i++) {
        if(devices_to_bind[i] != NULL)
            xemu_input_bind_passthrough(i, devices_to_bind[i], 1);
    }

    // Check to see if we should auto-bind the keyboard
    int port = xemu_input_get_controller_default_bind_port(new_con, 0);
    if (port >= 0) {
        xemu_input_bind(port, new_con, 0);
        char buf[128];
        snprintf(buf, sizeof(buf), "Connected '%s' to port %d", new_con->name, port+1);
        xemu_queue_notification(buf);
    }

    QTAILQ_INSERT_TAIL(&available_controllers, new_con, entry);
}

int xemu_input_get_controller_default_bind_port(ControllerState *state, int start)
{
    char guid[35] = { 0 };
    if (state->type == INPUT_DEVICE_SDL_GAMECONTROLLER) {
        SDL_JoystickGetGUIDString(state->sdl_joystick_guid, guid, sizeof(guid));
    } else if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
        snprintf(guid, sizeof(guid), "keyboard");
    }

    for (int i = start; i < 4; i++) {
        if (strcmp(guid, *port_index_to_settings_key_map[i]) == 0) {
            return i;
        }
    }

    return -1;
}

int xemu_input_get_libusb_device_default_bind_port(LibusbDevice *device, int start)
{
    assert(device);
    char guid[35] = { 0 };
    snprintf(guid, sizeof(guid), "USB\\%04x:%04x:%d:%s", device->vendor_id, device->product_id, device->host_bus, device->host_port);
    
    for(int i = start; i < 4; i++) {
        if(strcmp(guid, *port_index_to_settings_key_map[i])== 0) {
            return i;
        }
    }

    return -1;
}

void xemu_input_process_sdl_events(const SDL_Event *event)
{
    if (event->type == SDL_CONTROLLERDEVICEADDED) {
        DPRINTF("Controller Added: %d\n", event->cdevice.which);

        // Attempt to open the added controller
        SDL_GameController *sdl_con;
        sdl_con = SDL_GameControllerOpen(event->cdevice.which);
        if (sdl_con == NULL) {
            DPRINTF("Could not open joystick %d as a game controller\n", event->cdevice.which);
            return;
        }

        // Success! Create a new node to track this controller and continue init
        ControllerState *new_con = malloc(sizeof(ControllerState));
        memset(new_con, 0, sizeof(ControllerState));
        new_con->type                 = INPUT_DEVICE_SDL_GAMECONTROLLER;
        new_con->name                 = SDL_GameControllerName(sdl_con);
        new_con->gp.rumble_enabled    = true;
        new_con->sdl_gamecontroller   = sdl_con;
        new_con->sdl_joystick         = SDL_GameControllerGetJoystick(new_con->sdl_gamecontroller);
        new_con->sdl_joystick_id      = SDL_JoystickInstanceID(new_con->sdl_joystick);
        new_con->sdl_joystick_guid    = SDL_JoystickGetGUID(new_con->sdl_joystick);
        new_con->bound                = -1;

        char guid_buf[35] = { 0 };
        SDL_JoystickGetGUIDString(new_con->sdl_joystick_guid, guid_buf, sizeof(guid_buf));
        DPRINTF("Opened %s (%s)\n", new_con->name, guid_buf);

        QTAILQ_INSERT_TAIL(&available_controllers, new_con, entry);

        // Do not replace binding for a currently bound device. In the case that
        // the same GUID is specified multiple times, on different ports, allow
        // any available port to be bound.
        //
        // This can happen naturally with X360 wireless receiver, in which each
        // controller gets the same GUID (go figure). We cannot remember which
        // controller is which in this case, but we can try to tolerate this
        // situation by binding to any previously bound port with this GUID. The
        // upside in this case is that a person can use the same GUID on all
        // ports and just needs to bind to the receiver and never needs to hit
        // this dialog.


        // Attempt to re-bind to port previously bound to
        int port = 0;
        bool did_bind = false;
        while (!did_bind) {
            port = xemu_input_get_controller_default_bind_port(new_con, port);
            if (port < 0) {
                // No (additional) default mappings
                break;
            } else if (!xemu_input_get_bound(port)) {
                xemu_input_bind(port, new_con, 0);
                did_bind = true;
                break;
            } else {
                // Try again for another port
                port++;
            }
        }

        // Try to bind to any open port, and if so remember the binding
        if (!did_bind && g_config.input.auto_bind) {
            for (port = 0; port < 4; port++) {
                if (!xemu_input_get_bound(port) &&
                    !xemu_input_get_bound_device(port)) {
                    xemu_input_bind(port, new_con, 1);
                    did_bind = true;
                    break;
                }
            }
        }

        if (did_bind) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Connected '%s' to port %d", new_con->name, port+1);
            xemu_queue_notification(buf);
        }
    } else if (event->type == SDL_CONTROLLERDEVICEREMOVED) {
        DPRINTF("Controller Removed: %d\n", event->cdevice.which);
        int handled = 0;
        ControllerState *iter, *next;
        QTAILQ_FOREACH_SAFE(iter, &available_controllers, entry, next) {
            if (iter->type != INPUT_DEVICE_SDL_GAMECONTROLLER) continue;

            if (iter->sdl_joystick_id == event->cdevice.which) {
                DPRINTF("Device removed: %s\n", iter->name);

                // Disconnect
                if (iter->bound >= 0) {
                    // Queue a notification to inform user controller disconnected
                    // FIXME: Probably replace with a callback registration thing,
                    // but this works well enough for now.
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Port %d disconnected", iter->bound+1);
                    xemu_queue_notification(buf);

                    // Unbind the controller, but don't save the unbinding in
                    // case the controller is reconnected
                    xemu_input_bind(iter->bound, NULL, 0);
                }

                // Unlink
                QTAILQ_REMOVE(&available_controllers, iter, entry);

                // Deallocate
                if (iter->sdl_gamecontroller) {
                    SDL_GameControllerClose(iter->sdl_gamecontroller);
                }
                free(iter);

                handled = 1;
                break;
            }
        }
        if (!handled) {
            DPRINTF("Could not find handle for joystick instance\n");
        }
    } else if (event->type == SDL_CONTROLLERDEVICEREMAPPED) {
        DPRINTF("Controller Remapped: %d\n", event->cdevice.which);
    }
}

void xemu_input_update_controller(ControllerState *state)
{
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (ABS(now - state->last_input_updated_ts) <
        XEMU_INPUT_MIN_INPUT_UPDATE_INTERVAL_US) {
        return;
    }

    if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
        xemu_input_update_sdl_kbd_controller_state(state);
    } else if (state->type == INPUT_DEVICE_SDL_GAMECONTROLLER) {
        xemu_input_update_sdl_controller_state(state);
    }

    state->last_input_updated_ts = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
}

void xemu_input_update_controllers(void)
{
    ControllerState *iter;
    QTAILQ_FOREACH(iter, &available_controllers, entry) {
        xemu_input_update_controller(iter);
    }
    QTAILQ_FOREACH(iter, &available_controllers, entry) {
        xemu_input_update_rumble(iter);
    }
}

void xemu_input_update_sdl_kbd_controller_state(ControllerState *state)
{
    state->gp.buttons = 0;
    state->sbc.buttons = 0;
    memset(state->gp.axis, 0, sizeof(state->gp.axis));
    memset(state->sbc.axis, 0, sizeof(state->sbc.axis));

    const uint8_t *kbd = SDL_GetKeyboardState(NULL);

// Update Gamepad Buttons
    for (int i = 0; i < 15; i++) {
        state->gp.buttons |= kbd[sdl_kbd_scancode_map[i]] << i;
    }

    // Update Gamepad Axes
    if (kbd[sdl_kbd_scancode_map[15]]) state->gp.axis[CONTROLLER_AXIS_LSTICK_Y] = 32767;
    if (kbd[sdl_kbd_scancode_map[16]]) state->gp.axis[CONTROLLER_AXIS_LSTICK_X] = -32768;
    if (kbd[sdl_kbd_scancode_map[17]]) state->gp.axis[CONTROLLER_AXIS_LSTICK_X] = 32767;
    if (kbd[sdl_kbd_scancode_map[18]]) state->gp.axis[CONTROLLER_AXIS_LSTICK_Y] = -32768;
    if (kbd[sdl_kbd_scancode_map[19]]) state->gp.axis[CONTROLLER_AXIS_LTRIG] = 32767;

    if (kbd[sdl_kbd_scancode_map[20]]) state->gp.axis[CONTROLLER_AXIS_RSTICK_Y] = 32767;
    if (kbd[sdl_kbd_scancode_map[21]]) state->gp.axis[CONTROLLER_AXIS_RSTICK_X] = -32768;
    if (kbd[sdl_kbd_scancode_map[22]]) state->gp.axis[CONTROLLER_AXIS_RSTICK_X] = 32767;
    if (kbd[sdl_kbd_scancode_map[23]]) state->gp.axis[CONTROLLER_AXIS_RSTICK_Y] = -32768;
    if (kbd[sdl_kbd_scancode_map[24]]) state->gp.axis[CONTROLLER_AXIS_RTRIG] = 32767;

    state->sbc.buttons = 0;

    if(state->sbc.gearLever == 0)
        state->sbc.gearLever = 255;

    // Update SBC Buttons
    for(int i = 0; i < 43; i++) {
        if(kbd[sdl_sbc_kbd_scancode_map[i]])
            state->sbc.buttons |= (1ULL << i);
    }

    const uint64_t toggles[5] = { 
        SBC_BUTTON_FILT_CONTROL_SYSTEM,
        SBC_BUTTON_OXYGEN_SUPPLY_SYSTEM,
        SBC_BUTTON_FUEL_FLOW_RATE,
        SBC_BUTTON_BUFFER_MATERIAL,
        SBC_BUTTON_VT_LOCATION_MEASUREMENT
    };

    for(int i = 0; i < 5; i++) {
        if((state->sbc.buttons & toggles[i]) && !(state->sbc.previousButtons & toggles[i])) {   // When the for the toggle is pressed
            uint8_t byteMask = (uint8_t)(toggles[i] >> 32);
            // Toggle the toggle switch
            state->sbc.toggleSwitches ^= byteMask;
        }
    }

    // Tuner Dial Left
    if((state->sbc.buttons & SBC_BUTTON_TUNER_LEFT) && !(state->sbc.previousButtons & SBC_BUTTON_TUNER_LEFT)) {
        if(state->sbc.tunerDial == 0)
            state->sbc.tunerDial = 15;
        else
            state->sbc.tunerDial--;
    }

    // Tuner Dial Right
    if((state->sbc.buttons & SBC_BUTTON_TUNER_RIGHT) && !(state->sbc.previousButtons & SBC_BUTTON_TUNER_RIGHT)) {
        if(state->sbc.tunerDial == 15)
            state->sbc.tunerDial = 0;
        else
            state->sbc.tunerDial++;
    }

    // Gear Lever Up
    if((state->sbc.buttons & SBC_BUTTON_GEAR_UP) && !(state->sbc.previousButtons & SBC_BUTTON_GEAR_UP)) {
        if(state->sbc.gearLever != 5)
        {
            if(state->sbc.gearLever == 255)
                state->sbc.gearLever = 1;
            else
                state->sbc.gearLever++;
        }
    }

    // Gear Lever Down
    if((state->sbc.buttons & SBC_BUTTON_GEAR_DOWN) && !(state->sbc.previousButtons & SBC_BUTTON_GEAR_DOWN)) {
        if(state->sbc.gearLever != 254)
        {
            if(state->sbc.gearLever == 1)
                state->sbc.gearLever = 255;
            else
                state->sbc.gearLever--;
        }
    }

    // Update SBC Axes
    if(kbd[sdl_sbc_kbd_scancode_map[43]]) state->sbc.axis[SBC_AXIS_AIMING_Y] = -32768;
    if(kbd[sdl_sbc_kbd_scancode_map[44]]) state->sbc.axis[SBC_AXIS_AIMING_Y] =  32767;
    if(kbd[sdl_sbc_kbd_scancode_map[45]]) state->sbc.axis[SBC_AXIS_AIMING_X] = -32768;
    if(kbd[sdl_sbc_kbd_scancode_map[46]]) state->sbc.axis[SBC_AXIS_AIMING_X] =  32767;

    if(kbd[sdl_sbc_kbd_scancode_map[47]]) state->sbc.axis[SBC_AXIS_SIGHT_CHANGE_Y] = -32768;
    if(kbd[sdl_sbc_kbd_scancode_map[48]]) state->sbc.axis[SBC_AXIS_SIGHT_CHANGE_Y] =  32767;
    if(kbd[sdl_sbc_kbd_scancode_map[49]]) state->sbc.axis[SBC_AXIS_SIGHT_CHANGE_X] = -32768;
    if(kbd[sdl_sbc_kbd_scancode_map[50]]) state->sbc.axis[SBC_AXIS_SIGHT_CHANGE_X] =  32767;

    if(kbd[sdl_sbc_kbd_scancode_map[51]]) state->sbc.axis[SBC_AXIS_ROTATION_LEVER] = -32768;
    if(kbd[sdl_sbc_kbd_scancode_map[52]]) state->sbc.axis[SBC_AXIS_ROTATION_LEVER] =  32767;

    if(kbd[sdl_sbc_kbd_scancode_map[53]]) state->sbc.axis[SBC_AXIS_LEFT_PEDAL] = 32767;
    if(kbd[sdl_sbc_kbd_scancode_map[54]]) state->sbc.axis[SBC_AXIS_RIGHT_PEDAL] = 32767;
    if(kbd[sdl_sbc_kbd_scancode_map[55]]) state->sbc.axis[SBC_AXIS_MIDDLE_PEDAL] = 32767;

    state->sbc.previousButtons = state->sbc.buttons;
}

void xemu_input_update_sdl_controller_state(ControllerState *state)
{
    state->gp.buttons = 0;
    state->sbc.buttons = 0;
    memset(state->gp.axis, 0, sizeof(state->gp.axis));
    memset(state->sbc.axis, 0, sizeof(state->sbc.axis));

    const SDL_GameControllerButton sdl_button_map[15] = {
        SDL_CONTROLLER_BUTTON_A,
        SDL_CONTROLLER_BUTTON_B,
        SDL_CONTROLLER_BUTTON_X,
        SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_DPAD_LEFT,
        SDL_CONTROLLER_BUTTON_DPAD_UP,
        SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
        SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_BACK,
        SDL_CONTROLLER_BUTTON_START,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_LEFTSTICK,
        SDL_CONTROLLER_BUTTON_RIGHTSTICK,
        SDL_CONTROLLER_BUTTON_GUIDE
    };

    for (int i = 0; i < 15; i++) {
        state->gp.buttons |= SDL_GameControllerGetButton(state->sdl_gamecontroller, sdl_button_map[i]) << i;
    }

    const SDL_GameControllerAxis sdl_axis_map[6] = {
        SDL_CONTROLLER_AXIS_TRIGGERLEFT,
        SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
        SDL_CONTROLLER_AXIS_LEFTX,
        SDL_CONTROLLER_AXIS_LEFTY,
        SDL_CONTROLLER_AXIS_RIGHTX,
        SDL_CONTROLLER_AXIS_RIGHTY,
    };

    for (int i = 0; i < 6; i++) {
        state->gp.axis[i] = SDL_GameControllerGetAxis(state->sdl_gamecontroller, sdl_axis_map[i]);
    }

    // FIXME: Check range
    state->gp.axis[CONTROLLER_AXIS_LSTICK_Y] = -1 - state->gp.axis[CONTROLLER_AXIS_LSTICK_Y];
    state->gp.axis[CONTROLLER_AXIS_RSTICK_Y] = -1 - state->gp.axis[CONTROLLER_AXIS_RSTICK_Y];

    // xemu_input_print_controller_state(state);
    
    // Update the SBC too, just in case
    const uint64_t sdl_button_map_sbc[8][2] = {
        { SDL_CONTROLLER_BUTTON_A, SBC_BUTTON_MAIN_WEAPON },
        { SDL_CONTROLLER_BUTTON_B, SBC_BUTTON_LOCK_ON },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SBC_BUTTON_FUNC1 },
        { SDL_CONTROLLER_BUTTON_LEFTSTICK, SBC_BUTTON_SIGHT_CHANGE },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, SBC_BUTTON_GEAR_UP },
        { SDL_CONTROLLER_BUTTON_DPAD_DOWN, SBC_BUTTON_GEAR_DOWN },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, SBC_BUTTON_TUNER_LEFT },
        { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SBC_BUTTON_TUNER_RIGHT }
    };

    state->sbc.buttons = 0;

    if(state->sbc.gearLever == 0)
        state->sbc.gearLever = 255;

    for(int i = 0; i < 8; i++) {
        if(SDL_GameControllerGetButton(state->sdl_gamecontroller, sdl_button_map_sbc[i][0]))
            state->sbc.buttons |= sdl_button_map_sbc[i][1];
    }

    const uint64_t toggles[5] = { 
        SBC_BUTTON_FILT_CONTROL_SYSTEM,
        SBC_BUTTON_OXYGEN_SUPPLY_SYSTEM,
        SBC_BUTTON_FUEL_FLOW_RATE,
        SBC_BUTTON_BUFFER_MATERIAL,
        SBC_BUTTON_VT_LOCATION_MEASUREMENT
    };

    for(int i = 0; i < 5; i++) {
        if((state->sbc.buttons & toggles[i]) && !(state->sbc.previousButtons & toggles[i])) {   // When the for the toggle is pressed
            uint8_t byteMask = (uint8_t)(toggles[i] >> 32);
            // Toggle the toggle switch
            state->sbc.toggleSwitches ^= byteMask;
        }
    }

    // Tuner Dial Left
    if((state->sbc.buttons & SBC_BUTTON_TUNER_LEFT) && !(state->sbc.previousButtons & SBC_BUTTON_TUNER_LEFT)) {
        if(state->sbc.tunerDial == 0)
            state->sbc.tunerDial = 15;
        else
            state->sbc.tunerDial--;
    }

    // Tuner Dial Right
    if((state->sbc.buttons & SBC_BUTTON_TUNER_RIGHT) && !(state->sbc.previousButtons & SBC_BUTTON_TUNER_RIGHT)) {
        if(state->sbc.tunerDial == 15)
            state->sbc.tunerDial = 0;
        else
            state->sbc.tunerDial++;
    }

    // Gear Lever Up
    if((state->sbc.buttons & SBC_BUTTON_GEAR_UP) && !(state->sbc.previousButtons & SBC_BUTTON_GEAR_UP)) {
        if(state->sbc.gearLever != 5)
        {
            if(state->sbc.gearLever == 255)
                state->sbc.gearLever = 1;
            else
                state->sbc.gearLever++;
        }
    }

    // Gear Lever Down
    if((state->sbc.buttons & SBC_BUTTON_GEAR_DOWN) && !(state->sbc.previousButtons & SBC_BUTTON_GEAR_DOWN)) {
        if(state->sbc.gearLever != 254)
        {
            if(state->sbc.gearLever == 1)
                state->sbc.gearLever = 255;
            else
                state->sbc.gearLever--;
        }
    }

    state->sbc.axis[SBC_AXIS_SIGHT_CHANGE_X] = SDL_GameControllerGetAxis(state->sdl_gamecontroller, SDL_CONTROLLER_AXIS_LEFTX);
    state->sbc.axis[SBC_AXIS_SIGHT_CHANGE_Y] = SDL_GameControllerGetAxis(state->sdl_gamecontroller, SDL_CONTROLLER_AXIS_LEFTY);
    state->sbc.axis[SBC_AXIS_AIMING_X] = SDL_GameControllerGetAxis(state->sdl_gamecontroller, SDL_CONTROLLER_AXIS_RIGHTX);
    state->sbc.axis[SBC_AXIS_AIMING_Y] = SDL_GameControllerGetAxis(state->sdl_gamecontroller, SDL_CONTROLLER_AXIS_RIGHTY);
    state->sbc.axis[SBC_AXIS_MIDDLE_PEDAL] = SDL_GameControllerGetAxis(state->sdl_gamecontroller, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    state->sbc.axis[SBC_AXIS_RIGHT_PEDAL] = SDL_GameControllerGetAxis(state->sdl_gamecontroller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);

    state->sbc.previousButtons = state->sbc.buttons;
}

void xemu_input_update_rumble(ControllerState *state)
{
    if (!state->gp.rumble_enabled) {
        return;
    }

    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (ABS(now - state->last_rumble_updated_ts) <
        XEMU_INPUT_MIN_RUMBLE_UPDATE_INTERVAL_US) {
        return;
    }

    SDL_GameControllerRumble(state->sdl_gamecontroller, state->gp.rumble_l, state->gp.rumble_r, 250);
    state->last_rumble_updated_ts = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
}

ControllerState *xemu_input_get_bound(int index)
{
    return bound_controllers[index];
}

LibusbDevice *xemu_input_get_bound_device(int index)
{
    return bound_libusb_devices[index];
}

DeviceState *xemu_bind_usb_hub(int num_ports, const char *port)
{
    QDict *usbhub_qdict = qdict_new();
    qdict_put_str(usbhub_qdict, "driver", "usb-hub");
    qdict_put_str(usbhub_qdict, "port", port);
    qdict_put_int(usbhub_qdict, "ports", num_ports);
    QemuOpts *usbhub_opts = qemu_opts_from_qdict(qemu_find_opts("device"), usbhub_qdict, &error_abort);
    DeviceState *usbhub_dev = qdev_device_add(usbhub_opts, &error_abort);

    return usbhub_dev;
}

void xemu_input_bind_driver(int index, const char *port, const char *driver)
{
    // Create XID controller. This is connected to Port 1 of the controller's internal USB Hub
    QDict *qdict = qdict_new();

    // Specify device driver
    qdict_put_str(qdict, "driver", driver);

    // Specify device identifier
    static int id_counter = 0;
    char *tmp = g_strdup_printf("gamepad_%d", id_counter++);
    qdict_put_str(qdict, "id", tmp);
    g_free(tmp);

    // Specify index/port
    qdict_put_int(qdict, "index", index);
    qdict_put_str(qdict, "port", port);

    // Create the device
    QemuOpts *opts = qemu_opts_from_qdict(qemu_find_opts("device"), qdict, &error_abort);
    DeviceState *dev = qdev_device_add(opts, &error_abort);
    assert(dev);

    qobject_unref(qdict);
    object_unref(OBJECT(dev));
}

DeviceState *xemu_bind_usb_host(int hostbus, const char *hostport, const char *port)
{
    // Create XID controller. This is connected to Port 1 of the controller's internal USB Hub
    QDict *qdict = qdict_new();

    // specify device driver
    qdict_put_str(qdict, "driver", "usb-host");

    // specify hostbus
    qdict_put_int(qdict, "hostbus", hostbus);

    // specify hostport
    qdict_put_str(qdict, "hostport", hostport);

    // Specify port
    qdict_put_str(qdict, "port", port);

    // Create the device
    QemuOpts *opts = qemu_opts_from_qdict(qemu_find_opts("device"), qdict, &error_abort);
    DeviceState *dev = qdev_device_add(opts, &error_abort);
    assert(dev);

    qobject_unref(qdict);

    return dev;
}

void xemu_input_bind(int index, ControllerState *state, int save)
{
    // FIXME: Attempt to disable rumble when unbinding so it's not left
    // in rumble mode

    // Unbind existing controller
    if (bound_controllers[index]) {
        assert(bound_controllers[index]->device != NULL);
        Error *err = NULL;
        qdev_unplug((DeviceState *)bound_controllers[index]->device, &err);
        assert(err == NULL);

        bound_controllers[index]->bound = -1;
        bound_controllers[index]->device = NULL;
        bound_controllers[index] = NULL;
    }

    // Save this controller's GUID in settings for auto re-connect
    if (save) {
        char guid_buf[35] = { 0 };
        if (state) {
            if (state->type == INPUT_DEVICE_SDL_GAMECONTROLLER) {
                SDL_JoystickGetGUIDString(state->sdl_joystick_guid, guid_buf, sizeof(guid_buf));
            } else if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
                snprintf(guid_buf, sizeof(guid_buf), "keyboard");
            }
        }
        xemu_settings_set_string(port_index_to_settings_key_map[index], guid_buf);
        xemu_settings_set_string(port_index_to_driver_settings_key_map[index], bound_drivers[index]);
    }

    if(strcmp(bound_drivers[index], DRIVER_USB_PASSTHROUGH) != 0) {
        if (state) {
            if (state->bound >= 0) {
                // Device was already bound to another port. Unbind it.
                xemu_input_bind(state->bound, NULL, 1);
            }

            bound_controllers[index] = state;
            bound_controllers[index]->bound = index;

            const int port_map[4] = {3, 4, 1, 2};
            char *tmp;

            // Create controller's internal USB hub.
            tmp = g_strdup_printf("1.%d", port_map[index]);
            DeviceState *usbhub_dev = xemu_bind_usb_hub(3, tmp);
            g_free(tmp);

            char *port = g_strdup_printf("1.%d.1", port_map[index]);
            xemu_input_bind_driver(index, port, bound_drivers[index]);

            // Unref for eventual cleanup
            object_unref(OBJECT(usbhub_dev));

            state->device = usbhub_dev;
        }
    }
}

void xemu_input_bind_passthrough(int index, LibusbDevice *state, int save)
{
    if(bound_libusb_devices[index]) {
        assert(bound_libusb_devices[index]->device != NULL);
        Error *err = NULL;
        qdev_unplug((DeviceState *)bound_libusb_devices[index]->device, &err);
        assert(err == NULL);

        bound_libusb_devices[index]->bound = -1;
        bound_libusb_devices[index]->device = NULL;
        bound_libusb_devices[index] = NULL;
    }

    if(save) {
        char guid_buf[35] = { 0 };
        if(state) {
            // format:     hex       hex        int      string
            //         USB\vendor_id:product_id:host_bus:host_port
            snprintf(guid_buf, sizeof(guid_buf), "USB\\%04x:%04x:%d:%s", state->vendor_id, state->product_id, state->host_bus, state->host_port);
        }
        xemu_settings_set_string(port_index_to_settings_key_map[index], guid_buf);
        xemu_settings_set_string(port_index_to_driver_settings_key_map[index], bound_drivers[index]);
    }

    if(strcmp(bound_drivers[index], DRIVER_USB_PASSTHROUGH) == 0) {
        if(state) {
            if (state->bound >= 0) {
                // Device was already bound to another port. Unbind it.
                xemu_input_bind_passthrough(state->bound, NULL, 1);
            }

            bound_libusb_devices[index] = state;
            bound_libusb_devices[index]->bound = index;

            const int port_map[4] = {3, 4, 1, 2};
            char *tmp;

            if(state->internal_hub_ports > 0) {
                // Create controller's internal USB hub.
                char *port = g_strdup_printf("1.%d", port_map[index]);
                DeviceState *usbhub_dev = xemu_bind_usb_hub(state->internal_hub_ports, port);
                g_free(port);

                // Create XID controller. This is connected to Port 1 of the controller's internal USB Hub
                port = g_strdup_printf("1.%d.1", port_map[index]);
                DeviceState *controller_dev = xemu_bind_usb_host(state->host_bus, state->host_port, port);
                g_free(port);

                if(state->internal_hub_ports > 1) {
                    for(int i = 1; i < state->internal_hub_ports; i++) {
                        port = g_strdup_printf("1.%d.%d", port_map[index], i+1);
                        char *hostport = g_strdup_printf("%.*s%d", strlen(state->host_port) - 1, state->host_port, i+1);
                        DeviceState *expansion_port_dev = xemu_bind_usb_host(state->host_bus, hostport, port);
                        g_free(port);
                        g_free(hostport);
                        object_unref(OBJECT(expansion_port_dev));
                    }
                }

                // Unref for eventual cleanup
                object_unref(OBJECT(usbhub_dev));
                object_unref(OBJECT(controller_dev));
                
                state->device = usbhub_dev;
            } else {
                // Create XID controller. This is connected to Port 1 of the controller's internal USB Hub
                char *port = g_strdup_printf("1.%d", port_map[index]);
                DeviceState *controller_dev = xemu_bind_usb_host(state->host_bus, state->host_port, port);
                g_free(port);

                // Unref for eventual cleanup
                object_unref(OBJECT(controller_dev));

                state->device = controller_dev;
            }
        }
    }
}

void xemu_input_set_test_mode(int enabled)
{
    test_mode = enabled;
}

int xemu_input_get_test_mode(void)
{
    return test_mode;
}
