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

#include "src/shared/mgmt.h"
#include "log.h"
#include "adapter.h"

struct bt_adapter {
	struct mgmt *mgmt;
};

struct bt_adapter *bt_adapter_new(uint16_t index, struct mgmt *mgmt_if)
{
	struct bt_adapter *adapter;

	adapter = g_new0(struct bt_adapter, 1);

	adapter->mgmt = mgmt_ref(mgmt_if);

	return adapter;
}

void bt_adapter_start(struct bt_adapter *adapter)
{
	DBG("");

	/* TODO: CB: report scan mode */
	/* TODO: CB: report state on */
}

void bt_adapter_stop(struct bt_adapter *adapter)
{
	DBG("");
}
