/*
*
*  Author: Dave S. Desrochers <dave.desrochers@aether.com
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


#define AUI_SERVICE_UUID "cf0244d6-5081-4e0a-8236-b486a3985162"
#define AUI_RCV_UUID     "409497e8-c42d-4870-aa4f-fe4e5b516410"
#define AUI_SEND_UUID    "9e847894-d33c-4271-a3a5-bf7849fc0e03"
#define AUI_DEVID_UUID   "a8bb3a1f-0afa-463a-83ca-a10054087787"

static uint8_t aui_process_cmd(struct attribute *a, struct btd_device *device, gpointer user_data)
{
	DBG("DSD:  Processing command: 0x%x", *a->data);
	return 0;
}

static uint8_t aui_get_device_id(struct attribute *a, struct btd_device *device, gpointer user_data)
{
	struct btd_adapter *adapter = user_data;
	uint8_t hostname[HOST_NAME_MAX + 1];

	if(-1 == gethostname((char *)hostname, HOST_NAME_MAX)) {
		return ATT_ECODE_IO;
	}

	DBG("DSD:  Returning hostname: '%s'", hostname);

	attrib_db_update(adapter, a->handle, NULL, hostname, strlen((const char *)hostname), NULL);

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

static gboolean register_aui_service(struct btd_adapter *adapter)
{
	bt_uuid_t srv_uuid, rcv_uuid, devid_uuid, send_uuid;

	bt_string_to_uuid(  &srv_uuid, AUI_SERVICE_UUID);
        bt_string_to_uuid(  &rcv_uuid, AUI_RCV_UUID);
        bt_string_to_uuid(&devid_uuid, AUI_DEVID_UUID);
        bt_string_to_uuid( &send_uuid, AUI_SEND_UUID);

	/* Aether User Interface service */
	return gatt_service_add(adapter, GATT_PRIM_SVC_UUID, &srv_uuid,

			/* Commands From Remote */
			GATT_OPT_CHR_UUID, &rcv_uuid,
			GATT_OPT_CHR_PROPS, GATT_CHR_PROP_WRITE,
			GATT_OPT_CHR_VALUE_CB, ATTRIB_WRITE,
			aui_process_cmd, adapter,

			/* Device ID */
			GATT_OPT_CHR_UUID, &devid_uuid,
			GATT_OPT_CHR_PROPS, GATT_CHR_PROP_READ,
			GATT_OPT_CHR_VALUE_CB, ATTRIB_READ,
			aui_get_device_id, adapter,

			/* Commands To Remote */
			GATT_OPT_CHR_UUID, &send_uuid,
			GATT_OPT_CHR_PROPS, GATT_CHR_PROP_READ |
						GATT_CHR_PROP_NOTIFY,
			GATT_OPT_CHR_VALUE_CB, ATTRIB_READ,
			aui_send_event_to_remote, adapter,

			GATT_OPT_INVALID);
}

static int aui_server_init(struct btd_profile *p, struct btd_adapter *adapter)
{
	const char *path = adapter_get_path(adapter);

	DBG("DSD: path %s", path);

	if (!register_aui_service(adapter)) {
		error("AUI could not be registered");
		return -EIO;
	}

	return 0;
}

static void aui_server_remove(struct btd_profile *p, struct btd_adapter *adapter)
{
	const char *path = adapter_get_path(adapter);

	DBG("DSD: path %s", path);
}

static struct btd_profile aui_profile = {
	.name		= "Aether UI Profile",
	.remote_uuid    = AUI_SERVICE_UUID,
	.adapter_probe	= aui_server_init,
	.adapter_remove	= aui_server_remove,
};

static int aui_init(void)
{
	return btd_profile_register(&aui_profile);
}

static void aui_exit(void)
{
	btd_profile_register(&aui_profile);
}

BLUETOOTH_PLUGIN_DEFINE(aui, VERSION, BLUETOOTH_PLUGIN_PRIORITY_LOW,
					aui_init, aui_exit)
