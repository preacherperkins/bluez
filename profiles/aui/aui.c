/*-*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
*
*  Author: Dave S. Desrochers <dave.desrochers@aether.com>
*
* Copyright (c) 2014 Morse Project. All rights reserved.
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

enum {
	NOP                = 255,
	VOL_UP             = 10,
	VOL_DOWN           = 11,
	NEXT_TRACK         = 12,
	PREV_TRACK         = 13,
	NEXT_SET           = 14,
	PREV_SET           = 15,
	PLAY_PAUSE_TOGGLE  = 16
};

struct Self {
	uint8_t aui_cmd;
	struct btd_adapter *adapter;
	double volume;
	int16_t volume_in_db;
	uint32_t volume_dbus_id;
} *self;

static gboolean aui_property_cmd(const GDBusPropertyTable *property,
	DBusMessageIter *iter, void *data)
{
	struct Self *self = (struct Self*)data;

	DBG("DSD: %s: New Cmd: %d", __FUNCTION__, self->aui_cmd);

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BYTE, &(self->aui_cmd));

	return TRUE;
}

static DBusMessage *aui_register_watcher(DBusConnection *conn, DBusMessage *msg, void *data)
{
	DBG("DSD: %s", __FUNCTION__);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *aui_unregister_watcher(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	DBG("DSD: %s", __FUNCTION__);

	return dbus_message_new_method_return(msg);
}

static const GDBusPropertyTable aui_manager_properties[] = {
	{ "RemoteCmd", "y", aui_property_cmd },
	{ }
};

static const GDBusMethodTable aui_manager_methods[] = {
	{
		GDBUS_METHOD("RegisterWatcher",
				GDBUS_ARGS({ "agent", "o" }),
				NULL,
				aui_register_watcher)
	},
	{
		GDBUS_METHOD("UnregisterWatcher",
				GDBUS_ARGS({ "agent", "o" }),
				NULL,
				aui_unregister_watcher)
	},
	{ }
};

static uint8_t aui_process_cmd(struct attribute *a, struct btd_device *device, gpointer user_data)
{
	struct Self *self = (struct Self*)user_data;

	DBG("DSD:  Processing command: 0x%x", *a->data);

	self->aui_cmd = *a->data;

	g_dbus_emit_property_changed(btd_get_dbus_connection(),
			adapter_get_path(self->adapter), AUI_MANAGER_INTERFACE, "RemoteCmd" );

	return 0;
}

static uint8_t aui_get_device_id(struct attribute *a, struct btd_device *device, gpointer user_data)
{
        struct Self *self = user_data;
	uint8_t hostname[HOST_NAME_MAX + 1];

	if(-1 == gethostname((char *)hostname, HOST_NAME_MAX)) {
		return ATT_ECODE_IO;
	}

	DBG("DSD:  Returning hostname: '%s'", hostname);

	attrib_db_update(self->adapter, a->handle, NULL, hostname, strlen((const char *)hostname), NULL);

	return 0;
}

static uint8_t aui_send_event_to_remote(struct attribute *a, struct btd_device *device, gpointer user_data)
{
	struct btd_adapter *adapter = user_data;
	const char *garbage = "blablabla";

	DBG("DSD:  Sending async event to remote");

	attrib_db_update(adapter, a->handle, NULL, (uint8_t *)garbage, strlen(garbage), NULL);

	return 0;
}

static uint8_t aui_get_abs_volume(struct attribute *a, struct btd_device *device, gpointer user_data)
{
	struct Self *self = user_data;
	char volume[9];

	DBG("DSD: %s: Returning volume: '%f'", __FUNCTION__, self->volume);

	sprintf(volume, "%f", self->volume);

	attrib_db_update(self->adapter, a->handle, NULL, volume, sizeof(volume), NULL);

	return 0;
}

static gboolean register_aui_service(struct Self *self)
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
			aui_process_cmd, self,

			/* Device ID */
			GATT_OPT_CHR_UUID, &devid_uuid,
			GATT_OPT_CHR_PROPS, GATT_CHR_PROP_READ,
			GATT_OPT_CHR_VALUE_CB, ATTRIB_READ,
			aui_get_device_id, self,

			/* Commands To Remote */
			GATT_OPT_CHR_UUID, &send_uuid,
			GATT_OPT_CHR_PROPS, GATT_CHR_PROP_READ |
						GATT_CHR_PROP_NOTIFY,
			GATT_OPT_CHR_VALUE_CB, ATTRIB_READ,
			aui_send_event_to_remote, self,

			/* Absolute Volume */
			GATT_OPT_CHR_UUID, &volume_uuid,
			GATT_OPT_CHR_PROPS, GATT_CHR_PROP_READ,
			GATT_OPT_CHR_VALUE_CB, ATTRIB_READ,
			aui_get_abs_volume, self,

			GATT_OPT_INVALID);
}

static void aui_destroy_adapter(gpointer user_data)
{
	DBG("DSD: %s",__FUNCTION__);
}

static gboolean set_volume_attr(DBusMessageIter *curr_iter, const char *key, struct Self *self)
{
	int ctype;
	DBusMessageIter new_iter;
	DBusBasicValue value;

	while((ctype = dbus_message_iter_get_arg_type(curr_iter)) != DBUS_TYPE_INVALID) {

		switch(ctype) {
			case DBUS_TYPE_VARIANT :
			case DBUS_TYPE_DICT_ENTRY :
			case DBUS_TYPE_ARRAY :
				dbus_message_iter_recurse(curr_iter, &new_iter);
				set_volume_attr(&new_iter, key, self);
				break;
			case DBUS_TYPE_STRING :
				dbus_message_iter_get_basic(curr_iter, &value);
				strcpy(key, value.str);
				break;
			case DBUS_TYPE_INT16 :
				if(strcmp(key, "Volume") == 0) {
					dbus_message_iter_get_basic(curr_iter, &value);
					self->volume_in_db = value.i16;
					DBG("DSD: Volume DB changed to %d", self->volume_in_db);
				}
				break;
			case DBUS_TYPE_DOUBLE :
				if(strcmp(key, "TuneVolume") == 0) {
					dbus_message_iter_get_basic(curr_iter, &value);
					self->volume = value.dbl;
					DBG("DSD: Volume slider changed to %f", self->volume);
				}
				break;
			default :
				return FALSE;
		}
		dbus_message_iter_next(curr_iter);
	}

	return TRUE;
}

static gboolean volume_changed(DBusConnection *conn, DBusMessage *msg,
		void *user_data)
{
	struct Self *self = user_data;
	DBusMessageIter iter;
	char key[32];

	memset(key, '\0', 32);

	dbus_message_iter_init(msg, &iter);

	return set_volume_attr(&iter, key, self);
}

static int aui_adapter_init(struct btd_profile *p, struct btd_adapter *adapter)
{
	const char *path = adapter_get_path(adapter);

	DBG("DSD: path %s", path);

	self = g_new0(struct Self, 1);
	self->adapter = adapter;
	self->aui_cmd = NOP;

	if (!register_aui_service(self)) {
		error("AUI could not be registered");
		return -EIO;
	}

	if (!g_dbus_register_interface(btd_get_dbus_connection(),
				adapter_get_path(adapter),
				AUI_MANAGER_INTERFACE,
				aui_manager_methods,          /* Methods    */
				NULL,                         /* Signals    */
				aui_manager_properties,       /* Properties */
				self,                         /* User data  */
				aui_destroy_adapter)) {
		error("D-Bus failed to register %s interface", AUI_MANAGER_INTERFACE);
	}

	self->volume_dbus_id = g_dbus_add_properties_watch(btd_get_dbus_connection(),
			NULL, "/com/aether/Volume", "com.aether.Volume.Server",
			volume_changed, self, NULL);

	return 0;
}

static void aui_adapter_remove(struct btd_profile *p, struct btd_adapter *adapter)
{
	const char *path = adapter_get_path(adapter);

	DBG("DSD: path %s", path);

	g_dbus_unregister_interface(btd_get_dbus_connection(),
			adapter_get_path(adapter),
			AUI_MANAGER_INTERFACE);

	g_dbus_remove_watch(btd_get_dbus_connection(), self->volume_dbus_id);

	g_free(self);
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
