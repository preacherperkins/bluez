/*-*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
*
*  Author: Dave S. Desrochers <dave.desrochers@aether.com>
*
* Copyright (c) 2014 Aether Things. All rights reserved.
*
* @file Implements Aether User Interface (AUI) GATT server
*
*/

/*
 * Random thoughts:
 *  - Napa needs to tell Cone when it's going to sleep via descripter in notify characteristic
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib.h>
#include <gdbus/gdbus.h>
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>

#include "src/adapter.h"
#include "src/device.h"
#include "src/profile.h"
#include "src/plugin.h"

#include "lib/uuid.h"
#include "attrib/gattrib.h"
#include "attrib/att.h"
#include "attrib/gatt.h"
#include "attrib/att-database.h"
#include "src/shared/util.h"
#include "src/attrib-server.h"
#include "attrib/gatt-service.h"
#include "src/log.h"
#include "src/service.h"
#include "src/dbus-common.h"

#include "aui.h"

struct Self {
	uint8_t aui_cmd;
	struct btd_adapter *adapter;
	GDBusProxy *volumed_proxy;
	GDBusClient *volumed_client;
} *self;

static gboolean on_aui_dbus_cmd(const GDBusPropertyTable *, DBusMessageIter *, void *);

static const GDBusPropertyTable aui_manager_properties[] = {
	{ "RemoteCmd", "y", on_aui_dbus_cmd },
	{ }
};

static uint8_t on_aui_ble_cmd(struct attribute *a, struct btd_device *device, gpointer user_data)
{
	struct Self *self = (struct Self*)user_data;

	DBG("AUI:  Processing command: 0x%x", *a->data);

	self->aui_cmd = *a->data;

	g_dbus_emit_property_changed(btd_get_dbus_connection(),
			adapter_get_path(self->adapter), AUI_MANAGER_INTERFACE, "RemoteCmd" );

	return 0;
}

static uint8_t on_aui_ble_get_device_id(struct attribute *a, struct btd_device *device, gpointer user_data)
{
	struct Self *self = user_data;
	uint8_t hostname[HOST_NAME_MAX + 1];

	if(-1 == gethostname((char *)hostname, HOST_NAME_MAX)) {
		return ATT_ECODE_IO;
	}

	DBG("AUI:  Returning hostname: '%s'", hostname);

	attrib_db_update(self->adapter, a->handle, NULL, hostname, strlen((const char *)hostname), NULL);

	return 0;
}

static uint8_t on_aui_ble_send_async_event(struct attribute *a, struct btd_device *device, gpointer user_data)
{
	struct btd_adapter *adapter = user_data;
	const char *garbage = "blablabla";

	DBG("AUI:  Sending async event to remote");

	attrib_db_update(adapter, a->handle, NULL, (uint8_t *)garbage, strlen(garbage), NULL);

	return 0;
}

static uint8_t on_aui_ble_get_abs_volume(struct attribute *a, struct btd_device *device, gpointer user_data)
{
	struct Self *self = user_data;
	DBusMessageIter iter;
	double dbus_volume;
	char btd_volume[9];

	if(!g_dbus_proxy_get_property(self->volumed_proxy, "TuneVolume", &iter)) {
		error("AUI: ERROR: Could not get 'TuneVolume' property from VolumeD. Returning 0");
		strncpy(btd_volume, "0.0", sizeof(btd_volume));
	}
	else {
		dbus_message_iter_get_basic(&iter, &dbus_volume);
		snprintf(btd_volume, sizeof(btd_volume), "%f", dbus_volume);
	}

	DBG("AUI: %s: Returning volume: '%f'", __FUNCTION__, dbus_volume);
	attrib_db_update(self->adapter, a->handle, NULL, (uint8_t*)btd_volume, sizeof(btd_volume), NULL);

	return 0;
}

static gboolean register_aui_ble_service(struct Self *self)
{
	bt_uuid_t srv_uuid, rcv_uuid, devid_uuid, send_uuid, volume_uuid;

	bt_string_to_uuid(   &srv_uuid, AUI_SERVICE_UUID);
	bt_string_to_uuid(   &rcv_uuid, AUI_RCV_UUID);
	bt_string_to_uuid( &devid_uuid, AUI_DEVID_UUID);
	bt_string_to_uuid(  &send_uuid, AUI_SEND_UUID);
	bt_string_to_uuid(&volume_uuid, AUI_VOLUME_UUID);

	/* Aether User Interface service */
	return gatt_service_add(self->adapter, GATT_PRIM_SVC_UUID, &srv_uuid,

			/* Commands From Remote */
			GATT_OPT_CHR_UUID, &rcv_uuid,
			GATT_OPT_CHR_PROPS, GATT_CHR_PROP_WRITE,
			GATT_OPT_CHR_VALUE_CB, ATTRIB_WRITE,
			on_aui_ble_cmd, self,

			/* Device ID */
			GATT_OPT_CHR_UUID, &devid_uuid,
			GATT_OPT_CHR_PROPS, GATT_CHR_PROP_READ,
			GATT_OPT_CHR_VALUE_CB, ATTRIB_READ,
			on_aui_ble_get_device_id, self,

			/* Commands To Remote */
			GATT_OPT_CHR_UUID, &send_uuid,
			GATT_OPT_CHR_PROPS, GATT_CHR_PROP_READ |
						GATT_CHR_PROP_NOTIFY,
			GATT_OPT_CHR_VALUE_CB, ATTRIB_READ,
			on_aui_ble_send_async_event, self,

			/* Absolute Volume */
			GATT_OPT_CHR_UUID, &volume_uuid,
			GATT_OPT_CHR_PROPS, GATT_CHR_PROP_READ,
			GATT_OPT_CHR_VALUE_CB, ATTRIB_READ,
			on_aui_ble_get_abs_volume, self,

			GATT_OPT_INVALID);
}

static void aui_destroy_adapter(gpointer user_data)
{
	DBG("AUI: %s",__FUNCTION__);
}

static gboolean on_aui_dbus_cmd(const GDBusPropertyTable *property,
		DBusMessageIter *iter, void *data)
{
	struct Self *self = (struct Self*)data;

	DBG("AUI: %s: New Cmd: %d", __FUNCTION__, self->aui_cmd);

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BYTE, &(self->aui_cmd));

	return TRUE;
}

static gboolean volumed_proxy_init(struct Self *self)
{
	GDBusClient *client;

	client = g_dbus_client_new(btd_get_dbus_connection(), "com.aether.Volume", "/com/aether/Volume");
	if (!client) {
		return FALSE;
	}

	self->volumed_proxy = g_dbus_proxy_new(client, "/com/aether/Volume", "com.aether.Volume.Server");
	if (!self->volumed_proxy) {
		return FALSE;
	}

	return TRUE;
}

static int aui_adapter_init(struct btd_profile *p, struct btd_adapter *adapter)
{
	const char *path = adapter_get_path(adapter);

	DBG("AUI: path %s", path);

	self = g_new0(struct Self, 1);
	self->adapter = adapter;
	self->aui_cmd = NOP;

	if (!register_aui_ble_service(self)) {
		error("AUI could not be registered");
		return -EIO;
	}

	if (!g_dbus_register_interface(btd_get_dbus_connection(),
				adapter_get_path(adapter),
				AUI_MANAGER_INTERFACE,
				NULL,                         /* Methods    */
				NULL,                         /* Signals    */
				aui_manager_properties,       /* Properties */
				self,                         /* User data  */
				aui_destroy_adapter)) {
		error("D-Bus failed to register %s interface", AUI_MANAGER_INTERFACE);
	}

	if (!volumed_proxy_init(self)) {
		error("Could not connect to VolumeD");
	}

	return 0;
}

static void free_aui(struct Self *self)
{
	g_dbus_client_unref(self->volumed_client);
	g_dbus_proxy_unref(self->volumed_proxy);
	g_free(self);
}

static void aui_adapter_remove(struct btd_profile *p, struct btd_adapter *adapter)
{
	const char *path = adapter_get_path(adapter);

	DBG("AUI: path %s", path);

	g_dbus_unregister_interface(btd_get_dbus_connection(),
			adapter_get_path(adapter),
			AUI_MANAGER_INTERFACE);

	free_aui(self);
}

static struct btd_profile aui_profile = {
	.name		= "Aether UI Profile",
	.remote_uuid	= AUI_SERVICE_UUID,

	.adapter_probe	= aui_adapter_init,
	.adapter_remove	= aui_adapter_remove,
};

static int aui_init(void)
{
	return btd_profile_register(&aui_profile);
}

static void aui_exit(void)
{
	btd_profile_unregister(&aui_profile);
}

BLUETOOTH_PLUGIN_DEFINE(aui, VERSION, BLUETOOTH_PLUGIN_PRIORITY_LOW,
		aui_init, aui_exit)
