/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2013  Intel Corporation. All rights reserved.
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <stdint.h>
#include <stdbool.h>

#include <glib.h>

#include "lib/bluetooth.h"
#include "log.h"
#include "hal-msg.h"
#include "ipc.h"
#include "hid.h"

static GIOChannel *notification_io = NULL;

void bt_hid_handle_cmd(GIOChannel *io, uint8_t opcode, void *buf, uint16_t len)
{
	uint8_t status = HAL_STATUS_FAILED;

	switch (opcode) {
	case HAL_OP_HID_CONNECT:
		break;
	case HAL_OP_HID_DISCONNECT:
		break;
	default:
		DBG("Unhandled command, opcode 0x%x", opcode);
		break;
	}

	ipc_send_rsp(io, HAL_SERVICE_ID_HIDHOST, status);
}

bool bt_hid_register(GIOChannel *io, const bdaddr_t *addr)
{
	DBG("");

	notification_io = g_io_channel_ref(io);

	return true;
}

void bt_hid_unregister(void)
{
	DBG("");

	g_io_channel_unref(notification_io);
	notification_io = NULL;
}
