/*-*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
**
**  Author: Dirk-Jan C. Binnema <dirk@morseproject.com>
**
** Copyright (c) 2013 Morse Project. All rights reserved.
**
** @file
**
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <dirent.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <gdbus/gdbus.h>

#include "log.h"
#include "textfile.h"

#include "lib/uuid.h"
#include "lib/mgmt.h"
#include "src/shared/mgmt.h"

#include "hcid.h"
#include "sdpd.h"
#include "adapter.h"
#include "device.h"
#include "profile.h"
#include "service.h"
#include "dbus-common.h"
#include "error.h"
#include "glib-helper.h"
#include "storage.h"
#include "attrib/gattrib.h"
#include "attrib/att.h"
#include "attrib/gatt.h"
#include "attrib-server.h"
#include "attrib/att-database.h"
#include "attrib/gatt-service.h"
#include "attio.h"
#include "eir.h"

#include "arc.h"

#define CLIENT_TIMEOUT 60 /*seconds*/

static GSList *ARC_SERVERS = NULL;

typedef struct  {
	struct btd_adapter	*adapter;
	GHashTable		*char_table;
	guint			 adv_id, disc_id;
	struct mgmt		*mgmt;
	guint			 magic;
} ARCServer;

static gboolean do_enable_adv (ARCServer *self);
static gboolean enable_adv (ARCServer *self, gboolean enable);

static void
each_device_disconnect (struct btd_device *device, void *data)
{
	ARCServer *self;

	self = (ARCServer*)data;

	if (!device_is_connected (device))
		return;

	DBG ("arc: automatically disconnecting");
	btd_adapter_disconnect_device (self->adapter,
				       device_get_address(device),
				       BDADDR_LE_PUBLIC);
}

static gboolean
do_disconnect (ARCServer *self)
{
	btd_adapter_for_each_device (self->adapter,
				     each_device_disconnect, self);
	return FALSE;
}

static void
on_connection_event (uint16_t index, uint16_t length,
		 const void *param, ARCServer *self)
{
	DBG ("connected/disconnected now, client has %d seconds "
		"before auto-disconnect",
		CLIENT_TIMEOUT);

	g_timeout_add_seconds (CLIENT_TIMEOUT, (GSourceFunc)do_disconnect,
			self);

	/* clear out any connection-specific data */
	arc_char_table_clear_working_data (self->char_table);
}



ARCServer*
arc_server_new (struct btd_adapter *adapter)
{
	ARCServer	*self;
	unsigned	 u;

	self		 = g_new0 (ARCServer, 1);
	self->adapter	 = btd_adapter_ref (adapter);
	self->char_table = arc_char_table_new ();

	/*
	 * we need the mgmt interface to be able to get the
	 * connected/disconnected callbacks
	 */
	self->mgmt = mgmt_new_default ();
	mgmt_register(self->mgmt, MGMT_EV_DEVICE_DISCONNECTED,
		      btd_adapter_get_index (adapter),
		      (mgmt_notify_func_t)on_connection_event, self, NULL);

	mgmt_register(self->mgmt, MGMT_EV_DEVICE_CONNECTED,
		      btd_adapter_get_index (adapter),
		      (mgmt_notify_func_t)on_connection_event, self, NULL);

	return self;
}



static void
arc_server_destroy (ARCServer *self)
{
	GSList		*cur;
	unsigned	 u;

	if (!self)
		return;

	ARC_SERVERS = g_slist_remove (ARC_SERVERS, self);

	if (self->adv_id != 0) {
		g_source_remove (self->adv_id);
		self->adv_id = 0;
	}

	if (self->char_table)
		g_hash_table_destroy (self->char_table);

	if (self->mgmt)
		mgmt_unref (self->mgmt);

	if (self->adapter)
		btd_adapter_unref (self->adapter);

	g_free (self);
}


static gboolean
do_enable_adv (ARCServer *self)
{
	if (!enable_adv (self, TRUE))
		error ("failed to restart advertising");
	else
		DBG ("restarted advertising");

	return FALSE;
}


static ARCServer*
find_arc_server (struct btd_adapter *adapter)
{
	GSList *cur;

	for (cur = ARC_SERVERS; cur; cur = g_slist_next (cur)) {
		ARCServer *self;
		self = (ARCServer*)cur->data;

		if (self->adapter == adapter)
			return self;
	}
	return NULL;
}

static ARCServer*
find_arc_server_from_service (struct btd_service *service)
{
	struct btd_device	*device;
	struct btd_adapter	*adapter;
	ARCServer		*self;

	device	= btd_service_get_device (service);
	adapter	= device_get_adapter (device);
	self	= find_arc_server (adapter);

	return self;
}



static ARCServer*
find_arc_server_from_device (struct btd_device *device)
{
	struct btd_adapter	*adapter;
	ARCServer		*self;

	adapter	= device_get_adapter (device);
	self = find_arc_server (adapter);

	return self;
}


G_GNUC_UNUSED static void
dump_bytes (uint8_t *bytes, size_t len)
{
	unsigned u;

	for (u = 0; u !=len; ++u)
		g_print ("%02X ", bytes[u]);
	g_print ("(%d)\n", (int)len);
}


static int
hci_set_adv_params (int hcidev)
{
	struct hci_request			rq;
	le_set_advertising_parameters_cp	adv_params_cp;
	uint8_t					status;
	int					ret;

	/* p.1059 of the BT Core Spec 4.x */

	memset(&adv_params_cp, 0, sizeof(adv_params_cp));
	adv_params_cp.min_interval = htobs(0x0800);
	adv_params_cp.max_interval = htobs(0x0800);
	adv_params_cp.chan_map	   = 0x07; /* all channels */
	adv_params_cp.advtype	   = 0x00; /* Connectable undirected
					    * advertising */
	DBG ("hci adv data");
	dump_bytes ((uint8_t*)&adv_params_cp, sizeof(adv_params_cp));

	memset(&rq, 0, sizeof(rq));
	rq.ogf	  = OGF_LE_CTL;
	rq.ocf	  = OCF_LE_SET_ADVERTISING_PARAMETERS;
	rq.cparam = &adv_params_cp;
	rq.clen	  = LE_SET_ADVERTISING_PARAMETERS_CP_SIZE;
	rq.rparam = &status;
	rq.rlen	  = 1;

	return hci_send_req (hcidev, &rq, 1000);
}



/* this all makes sense after reading the BT spec, in particular
 * Appendix C */
static int
hci_set_adv_data (int hcidev, uint8_t data, const char *name)
{
	struct hci_request		rq;
	le_set_advertising_data_cp	advdata_cp;
	uint8_t				status;
	int				ret;
	bt_uuid_t			uuid;
	unsigned			offset, partsize;

	g_return_val_if_fail (hcidev, -1);

	memset(&advdata_cp, 0, sizeof(advdata_cp));
	offset = 0;

	/* the UUID to advertise */
	partsize = sizeof(uint128_t) + 1;
	advdata_cp.data[offset + 0] = partsize;
	advdata_cp.data[offset + 1] = 0x07; /* uuid */
	bt_string_to_uuid (&uuid, ARC_SERVICE_UUID);
	g_assert (uuid.type == BT_UUID128);
	memcpy (&advdata_cp.data[offset + 2], &uuid.value.u128,
		sizeof(uuid.value.u128));
	offset += partsize + 1;

	/* set our manufacturer-specific byte */
	partsize = 4;
	advdata_cp.data[offset + 0] = partsize;
	advdata_cp.data[offset + 1] = 0xff; /* manufacturer-specific data; */
	advdata_cp.data[offset + 2] = 0xf2;  /* Unknown */
	advdata_cp.data[offset + 3] = 0x00;  /* Vendor */
	advdata_cp.data[offset + 4] = data;
	offset += partsize + 1;

	/* the local name to advertise */
	if (name && name[0] != '\0') {
		unsigned strsize;
		strsize = MIN(strlen(name),
			      LE_SET_ADVERTISING_DATA_CP_SIZE - offset - 4);
		partsize = strsize + 1;
		advdata_cp.data[offset + 0] = partsize;
		advdata_cp.data[offset + 1] = 0x08;	/* short local name */
		memcpy (&advdata_cp.data[offset + 2], name, strsize);
		offset += partsize + 1;

		DBG ("advertising name '%s'", name);
	}

	advdata_cp.length  = offset + 1;

	DBG ("hci adv data");
	dump_bytes ((uint8_t*)&advdata_cp, sizeof(advdata_cp));

	memset (&rq, 0, sizeof(rq));
	rq.ogf	  = OGF_LE_CTL;
	rq.ocf	  = OCF_LE_SET_ADVERTISING_DATA;
	rq.cparam = &advdata_cp;
	rq.clen	  = sizeof (advdata_cp);
	rq.rparam = &status;
	rq.rlen	  = 1;

	return hci_send_req (hcidev, &rq, 1000);
}



static gboolean
hci_set_adv_enable (int hcidev, gboolean enable)
{
	struct hci_request		rq;
	le_set_advertise_enable_cp	advertise_cp;
	uint8_t				status;
	int				ret;

	memset(&advertise_cp, 0, sizeof(advertise_cp));
	advertise_cp.enable = enable ? 0x01 : 0x00;

	memset(&rq, 0, sizeof(rq));
	rq.ogf	  = OGF_LE_CTL;
	rq.ocf	  = OCF_LE_SET_ADVERTISE_ENABLE;
	rq.cparam = &advertise_cp;
	rq.clen	  = LE_SET_ADVERTISE_ENABLE_CP_SIZE;
	rq.rparam = &status;
	rq.rlen	  = 1;

	ret = hci_send_req (hcidev, &rq, 1000);
	if (ret < 0) {
		error ("failed to %sable advertising",
		       enable ? "en" : "dis");
		return FALSE;
	}

	return TRUE;
}



static gboolean
arc_attrib_db_update (ARCServer *self, ARCChar *achar)
{
	int ret;

	ret = attrib_db_update (self->adapter,
				achar->val_handle,
				NULL,
				achar->val->data,
				achar->val->len,
				NULL);
	if (ret != 0) {
		error ("failed to write attribute: %s",
		       strerror (-ret));
		return FALSE;
	} else if (achar->val->len == 0)
		DBG ("%s: clearing attr %s",
			__FUNCTION__, achar->name);
	else
		DBG ("%s: written attr %s [%s] (%d)",
			__FUNCTION__,
			achar->name,
			(const char *)achar->val->data,
			achar->val->len);

	return TRUE;
}


static gboolean
arc_attrib_db_clear (ARCServer *self, ARCChar *achar)
{
	int ret;

	ret = attrib_db_update (self->adapter,
				achar->val_handle,
				NULL, NULL, 0, NULL);
	if (ret != 0) {
		error ("failed to write attribute: %s",
		       strerror (-ret));
		return FALSE;
	} else
		DBG ("%s: cleared attr %s", achar->name);

	return TRUE;
}




static void
handle_blob (ARCServer *self, struct attribute *attr,
	     struct btd_device *device, ARCChar *achar)
{
	DBG ("%s", __FUNCTION__);

	if (g_strcmp0 (achar->uuidstr, ARC_REQUEST_UUID) == 0) {

		int		 ret;
		char		*str;
		const char	*objpath;
		const char	*empty;
		ARCChar		*result_char;
		char		*request;

		/* make sure it's valid utf8 */
		if (!g_utf8_validate (
			    (const char*)achar->val->data,
			    achar->val->len, NULL)) {
			error ("request is not valid utf8");
			return;
		}

		objpath = device_get_path (device);

		/* when processing the current method, clear any
		 * existing result */
		/* DBG ("clearing old results"); */
		/* if (!arc_attrib_db_clear (self, achar)) { */
		/* 	error ("failed to update attrib"); */
		/* 	return; */
		/* } */

		request = arc_char_get_value_string (achar);
		if (!request)
			request = g_strdup ("");

		DBG ("emitting method-called (%s)", request);

		g_dbus_emit_signal (
			btd_get_dbus_connection(),
			adapter_get_path (self->adapter),
			ARC_SERVER_IFACE, "MethodCalled",
			DBUS_TYPE_OBJECT_PATH, &objpath,
			DBUS_TYPE_STRING, &request,
			DBUS_TYPE_INVALID);

		g_free (request);
	}
}


static uint8_t
attr_arc_server_write (struct attribute *attr, struct btd_device *device,
		       ARCServer *self)
{
	ARCChar		*achar;
	unsigned	 u;

	DBG ("writing handle 0x%04x", attr->handle);

	achar = arc_char_table_find_by_attr (self->char_table, attr);
	if (!achar) {
		error ("unknown handle");
		return 0;
	}

	if (!achar->flags & ARC_CHAR_FLAG_WRITABLE) {
		error ("characteristic is not writable");
		return 0;
	}

	DBG ("%s: %s", __FUNCTION__, achar->name);

	/* if we see 0xfe, we start from scratch;
	 * otherwise, accumulate until we see 0xff*/
	for (u = 0; u != attr->len; ++u) {
		guint8 byte;
		byte = (guint8)attr->data[u];
		switch (byte) {
		case ARC_GATT_BLURB_PRE: /* remove everything */
			arc_char_set_value_string (achar, NULL);
			break;
		case ARC_GATT_BLURB_POST:
			handle_blob (self, attr, device, achar);
			break;
		default: /* append */
			g_byte_array_append (achar->val, &byte, 1);
			break;
		}
	}

	return 0;
}



static uint8_t
attr_arc_server_read (struct attribute	*attr,
		      struct btd_device *device, ARCServer *self)
{
	ARCChar		*achar;
	ARCID		 id;
	size_t		 len;
	const guint	 BLE_MAXLEN = 19; /* empirically derived */

	achar = arc_char_table_find_by_attr (self->char_table, attr);
	if (!achar) {
		error ("unknown handle");
		return 0;
	}

	if (!achar->flags & ARC_CHAR_FLAG_READABLE) {
		error ("characteristic is not readable");
		return 0;
	}

	DBG ("%s: %s (%u)",
	     __FUNCTION__, achar->name, achar->writing);

	/* write in chunks; this is an ugly workaround because bluez
	 * cannot do long-writes (2013.09.20) */
	if (!achar->writing) /* copy the characteristic to our scratchpad */
		arc_char_init_scratch (achar, TRUE/*copy*/);

	len = MIN (BLE_MAXLEN, achar->val_scratch->len);

	/* we just start with this value; set the beginning-of-data
	 * token (length is one less) */
	if (!achar->writing) {
		DBG ("%s: writing start blurb (%s)", __FUNCTION__, achar->name);
		achar->data[0] = ARC_GATT_BLURB_PRE;	/* the token for begin-of-data */
		if (len > 0) { /* copy a chunk  and remove it */
			memcpy (&achar->data[1],
				achar->val_scratch->data, len - 1);
			g_byte_array_remove_range (
				achar->val_scratch, 0, len - 1);
		}
		achar->writing = TRUE;
	} else if (len > 0) { /* we're in the middle */
		DBG ("%s: writing middle blurb (%s)", __FUNCTION__, achar->name);
		memcpy (achar->data, achar->val_scratch->data, len);
		g_byte_array_remove_range (achar->val_scratch, 0, len);
	}

	/* we're at the end? check if there's space left; if not, this
	 * goes with the next read */
	if (achar->val_scratch->len == 0 && len < BLE_MAXLEN) {
		DBG ("%s: writing end blurb (%s)", __FUNCTION__, achar->name);
		achar->data[len] = ARC_GATT_BLURB_POST;
		len += 1;
		achar->writing	 = FALSE;
		arc_char_init_scratch (achar, FALSE/*don't copy*/);
	}

	attr->data = (uint8_t*)achar->data;
	attr->len  = len;

	DBG ("reading handle 0x%04x", attr->handle);

	return 0;
}

static gboolean
register_service (ARCServer *self)
{
	GHashTableIter	 iter;
	gboolean	 rv;
	const char	*uuidstr;
	bt_uuid_t	 srv_uuid;
	ARCChar		*req_char, *event_char, *result_char,
			*devname_char, *jid_char;

	bt_string_to_uuid (&srv_uuid, ARC_SERVICE_UUID);

	req_char     = arc_char_table_find_by_uuid (self->char_table,
						ARC_REQUEST_UUID);
	event_char   = arc_char_table_find_by_uuid (self->char_table,
						ARC_EVENT_UUID);
	result_char  = arc_char_table_find_by_uuid (self->char_table,
						ARC_RESULT_UUID);
	devname_char = arc_char_table_find_by_uuid (self->char_table,
						ARC_DEVNAME_UUID);
	jid_char     = arc_char_table_find_by_uuid (self->char_table,
						ARC_JID_UUID);

	rv = gatt_service_add (
		self->adapter, GATT_PRIM_SVC_UUID, &srv_uuid,

		GATT_OPT_CHR_UUID, &req_char->uuid,
		GATT_OPT_CHR_PROPS, req_char->gatt_props,
		GATT_OPT_CHR_VALUE_CB, ATTRIB_WRITE,
		attr_arc_server_write, self,
		GATT_OPT_CHR_VALUE_CB, ATTRIB_READ,
		attr_arc_server_read, self,
		GATT_OPT_CHR_VALUE_GET_HANDLE, &req_char->val_handle,

		GATT_OPT_CHR_UUID, &event_char->uuid,
		GATT_OPT_CHR_PROPS, event_char->gatt_props,
		GATT_OPT_CHR_VALUE_CB, ATTRIB_WRITE,
		attr_arc_server_write, self,
		GATT_OPT_CHR_VALUE_CB, ATTRIB_READ,
		attr_arc_server_read, self,
		GATT_OPT_CHR_VALUE_GET_HANDLE, &event_char->val_handle,

		GATT_OPT_CHR_UUID, &result_char->uuid,
		GATT_OPT_CHR_PROPS, result_char->gatt_props,
		GATT_OPT_CHR_VALUE_CB, ATTRIB_WRITE,
		attr_arc_server_write, self,
		GATT_OPT_CHR_VALUE_CB, ATTRIB_READ,
		attr_arc_server_read, self,
		GATT_OPT_CHR_VALUE_GET_HANDLE, &result_char->val_handle,

		GATT_OPT_CHR_UUID, &devname_char->uuid,
		GATT_OPT_CHR_PROPS, devname_char->gatt_props,
		GATT_OPT_CHR_VALUE_CB, ATTRIB_WRITE,
		attr_arc_server_write, self,
		GATT_OPT_CHR_VALUE_CB, ATTRIB_READ,
		attr_arc_server_read, self,
		GATT_OPT_CHR_VALUE_GET_HANDLE, &devname_char->val_handle,

		GATT_OPT_CHR_UUID, &jid_char->uuid,
		GATT_OPT_CHR_PROPS, jid_char->gatt_props,
		GATT_OPT_CHR_VALUE_CB, ATTRIB_WRITE,
		attr_arc_server_write, self,
		GATT_OPT_CHR_VALUE_CB, ATTRIB_READ,
		attr_arc_server_read, self,
		GATT_OPT_CHR_VALUE_GET_HANDLE, &jid_char->val_handle,

		GATT_OPT_INVALID);


	if (!rv)  {
		error ("failed to add characteristics");
		return FALSE;
	} else
		DBG ("added characteristics");

	return TRUE;
}

/*
 * find the bdaddr_t* bluetooth address for the device with the matching
 * object-path, or NULL if it can't be found
 */
typedef struct {
	const char		*obj_path;
	struct btd_device	*device;
} CBData;

static void
each_device (struct btd_device *device, void *data)
{
	CBData	*cbdata;
	cbdata = (CBData*)data;

	if (cbdata->device)
		return; /* already found */

	if (g_strcmp0 (device_get_path (device), cbdata->obj_path) == 0)
		cbdata->device = device;
}

struct btd_device*
find_device_for_object_path (ARCServer *self, const char *obj_path)
{
	CBData cbdata;

	cbdata.obj_path = obj_path;
	cbdata.device	= NULL;

	btd_adapter_for_each_device (self->adapter, each_device, &cbdata);

	return cbdata.device;
}

static int
chunked_attrib_db_update (ARCServer *self, ARCChar *achar)
{
	int		 ret;
	const unsigned	 chunksize = 20;
	unsigned	 len;
	guint8		*bytes, *cur;
	char		*s;

	/* wrap in 0xfe <data> 0xff */
	len   = achar->val->len + 2;
	bytes = g_new (guint8, len);
	memcpy (bytes + 1, achar->val->data, achar->val->len);
	bytes[0]		   = ARC_GATT_BLURB_PRE;
	bytes[achar->val->len + 1] = ARC_GATT_BLURB_POST;

	ret			   = 0;
	cur			   = bytes;


	for (;;) {
		size_t	size;
		size = MIN(len, chunksize);
		if (!arc_attrib_db_update (self, achar)) {
			error ("failed to update attrib ('%s')", achar->name);
			break;
		}
		DBG ("written chunk (%u octets)", size);

		if (size < chunksize)
			break; /* we're done */

		cur += size;
		len -= size;
	}

	g_free (bytes);

	return ret;
}

static DBusMessage*
emit_event_method (DBusConnection *conn, DBusMessage *msg,
		   ARCServer *self)
{
	DBusMessage		*reply;
	const char		*event;
	gboolean		 rv;
	int			 ret;
	struct btd_device	*device;
	ARCChar			*event_achar;

	rv = dbus_message_get_args (msg, NULL,
				    DBUS_TYPE_STRING, &event,
				    DBUS_TYPE_INVALID);
	if (!rv)
		return btd_error_invalid_args (msg);

	event_achar = arc_char_table_find_by_uuid (
		self->char_table, ARC_EVENT_UUID);
	if (!event_achar) {
		error ("cannot find event-char");
		return btd_error_invalid_args (msg);
	}

	arc_char_set_value_string (event_achar, event);

	ret = chunked_attrib_db_update (self, event_achar);
	if (ret != 0)
		DBG ("error writing event to GATT: %s",
		     strerror(-ret));

	reply = dbus_message_new_method_return (msg);
	if (!reply)
		return btd_error_failed (msg, "failed to create reply");

	dbus_message_append_args(reply, DBUS_TYPE_INVALID);

	return reply;
}


static DBusMessage*
submit_result_method (DBusConnection *conn, DBusMessage *msg, ARCServer *self)
{
	DBusMessage		*reply;
	const char		*results, *target_path;
	gboolean		 rv;
	int			 ret;
	char			*res;
	struct btd_device	*device;
	ARCChar			*result_achar;

	DBG ("%s", __FUNCTION__);

	rv     = dbus_message_get_args (msg, NULL,
					DBUS_TYPE_OBJECT_PATH, &target_path,
					DBUS_TYPE_STRING, &results,
					DBUS_TYPE_INVALID);
	if (!rv || !results)
		return btd_error_invalid_args (msg);

	device = find_device_for_object_path (self, target_path);
	if (!device)
		return btd_error_failed (msg, "could not find target");

	result_achar = arc_char_table_find_by_uuid (self->char_table,
						ARC_RESULT_UUID);
	if (!result_achar)
 		return btd_error_failed (msg, "could not find characteristic");

	DBG ("%s: updating with [%s]", __FUNCTION__, results);
	if (!arc_attrib_db_update (self, result_achar)) {
		return btd_error_failed
			(msg, "gatt update failed (result)");
	}

	if (!(reply = dbus_message_new_method_return (msg)))
		return btd_error_failed (msg, "error creating DBus reply");

	dbus_message_append_args(reply, DBUS_TYPE_INVALID);

	return reply;
}


/**
 * This updates the name in GATT as well as the device name (which is
 * not settable of dbus)
 *
 * @param conn
 * @param msg
 * @param self
 *
 * @return
 */
static DBusMessage*
update_name_method (DBusConnection *conn, DBusMessage *msg,
		    ARCServer *self)
{
	DBusMessage	*reply;
	const char	*name;
	gboolean	 rv;
	int		 ret;
	ARCChar		*name_char;

	rv     = dbus_message_get_args (msg, NULL,
					DBUS_TYPE_STRING, &name,
					DBUS_TYPE_INVALID);
	if (!rv)
		return btd_error_invalid_args (msg);

	DBG ("updating name to '%s'", name);

	if (adapter_set_name (self->adapter, name) != 0)
		return btd_error_failed (
			msg, "updating adapter name failed");

	name_char = arc_char_table_find_by_uuid (
		self->char_table, ARC_DEVNAME_UUID);
	if (!name_char)
		return btd_error_failed (
			msg, "cannot find name char");

	arc_char_set_value_string (name_char, name);

	ret = chunked_attrib_db_update (self, name_char);
	if (ret != 0)
		return btd_error_failed (msg, "gatt update failed (name)");

	if (!(reply = dbus_message_new_method_return (msg)))
		return btd_error_failed (msg, "error creating DBus reply");

	dbus_message_append_args(reply, DBUS_TYPE_INVALID);

	return reply;
}




/**
 * Implementation of the EnableAdvertising DBus method.
 *
 * Attempt to change the advertising state of this adapter; this is
 * needed before doing scans, since one cannot do scans while in
 * advertising mode
 *
 * @param conn dbus connection
 * @param msg dbus message
 * @param self this arc server
 *
 * @return a DBUS-message with the response
 */
static DBusMessage*
enable_advertising_method (DBusConnection *conn, DBusMessage *msg, ARCServer *self)
{
	DBusMessage	*reply;
	gboolean	 enable, rv;
	int		 ret;

	rv = dbus_message_get_args (msg, NULL,
				    DBUS_TYPE_BOOLEAN, &enable,
				    DBUS_TYPE_INVALID);

	/* turning advertising on/off */
	if (!enable_adv (self, enable)) {
		char	*blurb;
		blurb = g_strdup_printf ("%sabling advertising failed",
					 enable ? "en" : "dis");
		reply = btd_error_failed (msg, blurb);
		g_free (blurb);
	} else if (!(reply = dbus_message_new_method_return (msg)))
		reply = btd_error_failed (msg, "error creating DBus reply");
	else {
		DBG ("arc: %sable advertising", enable ? "en" : "dis");
		dbus_message_append_args(reply, DBUS_TYPE_INVALID);
	}

	return reply;
}


static void
magic_property_set (const GDBusPropertyTable *property, DBusMessageIter *iter,
		    GDBusPendingPropertySet id, ARCServer *aserver)
{
	ARCChar	*achar;
	uint8_t	 byte;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_BYTE) {
		g_dbus_pending_property_error(
			id, ERROR_INTERFACE ".InvalidArguments",
			"Invalid request parameter");
		return;
	}

	dbus_message_iter_get_basic(iter, &byte);
	aserver->magic = byte;

	DBG ("setting magic to 0x%x", byte);

	g_dbus_emit_property_changed (btd_get_dbus_connection (),
				adapter_get_path (aserver->adapter),
				ARC_SERVER_IFACE, property->name);

	g_dbus_pending_property_success (id);
}



static gboolean
magic_property_get (const GDBusPropertyTable *property,
	      DBusMessageIter *iter, ARCServer *aserver)
{
	dbus_message_iter_append_basic (iter, DBUS_TYPE_BYTE, &aserver->magic);
	return TRUE;
}



static void
gatt_property_set (const GDBusPropertyTable *property, DBusMessageIter *iter,
		   GDBusPendingPropertySet id, ARCServer *aserver)
{
	ARCChar		*achar;
	const char	*str;

	DBG ("%s: %s", __FUNCTION__, property->name);

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_STRING) {
		g_dbus_pending_property_error(
			id, ERROR_INTERFACE ".InvalidArguments",
			"Invalid request parameter");
		return;
	}

	dbus_message_iter_get_basic(iter, &str);

	achar = arc_char_table_find_by_name (aserver->char_table,
					     property->name);
	if (!achar) {
		g_dbus_pending_property_error(
			id, ERROR_INTERFACE ".InvalidArguments",
			"Unknown property");
		return;
	}

	arc_char_set_value_string (achar, str);
	if (chunked_attrib_db_update (aserver, achar) != 0) {
		g_dbus_pending_property_error(
			id, ERROR_INTERFACE ".InvalidArguments",
			"Failed to update GATT");
		return;
	}

	g_dbus_emit_property_changed (btd_get_dbus_connection (),
				      adapter_get_path (aserver->adapter),
				      ARC_SERVER_IFACE, property->name);

	g_dbus_pending_property_success (id);
}


static gboolean
gatt_property_exists (const GDBusPropertyTable *property, ARCServer *aserver)
{
	ARCChar *achar;

	DBG ("%s: %s", __FUNCTION__, property->name);

	achar = arc_char_table_find_by_name (aserver->char_table,
					     property->name);

	return achar ? TRUE : FALSE;
}


static gboolean
gatt_property_get (const GDBusPropertyTable *property,
	      DBusMessageIter *iter, ARCServer *aserver)
{
	ARCChar	*achar;
	char	*str;

	DBG ("%s: %s", __FUNCTION__, property->name);

	achar = arc_char_table_find_by_name (aserver->char_table,
						property->name);
	if (!achar) {
		error ("unknown property %s", property->name);
		return FALSE;
	}

	str = arc_char_get_value_string (achar);
	if (!str)
		str = g_strdup ("");

	dbus_message_iter_append_basic (iter, DBUS_TYPE_STRING, &str);
	g_free (str);

	return TRUE;
}



static const GDBusPropertyTable
ARC_SERVER_PROPS[] = {
	{ "JID", "s",
	  (GDBusPropertyGetter)gatt_property_get,
	  (GDBusPropertySetter)gatt_property_set,
	  NULL
	},

	{ "Magic", "y",
	  (GDBusPropertyGetter)magic_property_get,
	  (GDBusPropertySetter)magic_property_set,
	  NULL
	},
	{}
};


static const GDBusSignalTable
ARC_SERVER_SIGNALS[] = {
	{ GDBUS_SIGNAL ("MethodCalled",
			GDBUS_ARGS({"Caller", "o"},
				   {"Params", "s"}))},
	{}
};

/* an event is sent to all connected devics */
static const GDBusMethodTable
ARC_SERVER_METHODS[] = {
	{ GDBUS_METHOD
	  ("SubmitResult",
	   GDBUS_ARGS({"Recipient", "o"},
		      {"Result", "s" }), /* json blob (in) */
	   NULL, (GDBusMethodFunction)submit_result_method)},

	{ GDBUS_METHOD
	  ("EmitEvent",
	   GDBUS_ARGS({ "Event", "s" }), /* json blob (in) */
	   NULL, (GDBusMethodFunction)emit_event_method) },

	{ GDBUS_METHOD
	  ("UpdateName",
	   GDBUS_ARGS({ "Name", "s" }),
	   NULL, (GDBusMethodFunction)update_name_method) },

	{ GDBUS_METHOD
	  ("EnableAdvertising",
	   GDBUS_ARGS({ "Enable", "b" }),
	   NULL, (GDBusMethodFunction)enable_advertising_method) },

	{}
};

int
arc_probe_server (struct btd_profile *profile, struct btd_adapter *adapter)
{
	ARCServer *self;

	self = arc_server_new (adapter);

	register_service (self);

	g_dbus_register_interface (btd_get_dbus_connection(),
				   adapter_get_path (adapter),
				   ARC_SERVER_IFACE,
				   ARC_SERVER_METHODS,
				   ARC_SERVER_SIGNALS,
				   ARC_SERVER_PROPS,
				   self, NULL);

	ARC_SERVERS = g_slist_prepend (ARC_SERVERS, self);

	return 0;
}

void
arc_remove_server (struct btd_profile *profile, struct btd_adapter *adapter)
{
	GSList		*cur;
	ARCServer	*self;
	/* GHashTableIter	 iter; */
	ARCChar		*achar;
	const char	*uuidstr;

	self = find_arc_server (adapter);
	if (!self)
		return;

	g_dbus_unregister_interface(btd_get_dbus_connection(),
				    adapter_get_path (adapter),
				    ARC_SERVER_IFACE);

	/* make sure to delete the attribute, otherwise
	 * bluez will try to remove them after we're gone already */
	/* g_hash_table_iter_init (&iter, self->char_table); */
	/* while (g_hash_table_iter_next (&iter, (gpointer)&uuidstr, */
	/* 			       (gpointer)&achar)) { */
	/* 	attrib_db_update (self->adapter, */
	/* 			  achar->val_handle, NULL, */
	/* 			  NULL, 0, NULL); */
	/* 	attrib_db_del (self->adapter, achar->val_handle); */
	/* } */
	arc_server_destroy (self);
}


/**
 * Get an HCI-device handle. Close with hci_close_dev
 *
 * @param serf an ARCServer
 * @param hcisock an HCI-socket
 *
 * @return the device handle, < 0 in case or error
 */
static int
get_hci_device (ARCServer *self, int hcisock)
{
	int			hcidev, fd;
	struct hci_dev_info	devinfo;

	devinfo.dev_id = btd_adapter_get_index (self->adapter);
	if (devinfo.dev_id == MGMT_INDEX_NONE) {
		error ("can't get adapter index");
		return -1;
	}

	devinfo.flags = HCI_RAW;

	if (ioctl (hcisock, HCIGETDEVINFO, (void*)&devinfo) != 0) {
		error ("can't get hci socket");
		return -1;
	}

	fd = hci_open_dev (devinfo.dev_id);
	hci_read_bd_addr(fd, &devinfo.bdaddr, 1000);
	hci_close_dev(fd);

	hcidev = hci_open_dev (devinfo.dev_id);
	if (hcidev < 0) {
		error ("failed to open hci device");
		return -1;
	}

	return hcidev;
}



static void
disco_callback (uint8_t status, uint16_t len, const void *param,
		void *user_data)
{
	if (status == 0) {
		DBG ("disco started/stopped");
		return;
	}

	g_debug ("Unable to start/stop discovery. status 0x%02x (%s)",
		status, mgmt_errstr(status));
}

static void
enable_discovery (ARCServer *aserver, gboolean enable)
{
	struct mgmt_cp_start_discovery	cp;
	uint8_t				type;
	int res;

	type = 0;
	hci_clear_bit (BDADDR_BREDR, &type);
	hci_set_bit   (BDADDR_LE_PUBLIC, &type);
	hci_set_bit   (BDADDR_LE_RANDOM, &type);

	cp.type = type;

	if (enable)
		res = mgmt_send (aserver->mgmt, MGMT_OP_START_DISCOVERY,
				btd_adapter_get_index (aserver->adapter),
				sizeof(cp), &cp, disco_callback,
				NULL, NULL);
	else
		res = mgmt_send (aserver->mgmt, MGMT_OP_STOP_DISCOVERY,
				btd_adapter_get_index (aserver->adapter),
				sizeof(cp), &cp, disco_callback,
				NULL, NULL);

	if (res != 0)
		error ("%s disco failed",
			enable ? "starting" : "stopping");
}


/**
 * Enable/Disable advertising using the HCI-interface
 *
 * @param self an ARC-server instance
 * @param enable if TRUE, enable advertising, otherwise disable it
 *
 * @return TRUE if enabling or disabling worked, FALSE otherwise
 */
static gboolean
enable_adv (ARCServer *self, gboolean enable)
{
	gboolean		rv;
	struct hci_dev_info	devinfo;
	struct hci_request	rq;
	int			hcisock, hcidev, ret;
	uint8_t byte[] = { 0x0 };

	rv     = FALSE;
	hcidev = hcisock = -1;

	hcisock = socket (AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
	if (hcisock < 0)  {
		error ("cannot open hci-socket: %s", strerror (errno));
		goto leave;
	}

	hcidev = get_hci_device (self, hcisock);
	if (hcidev < 0)
		goto leave;

	if (enable /* ie, if we're currently disabled */) {
		ret = hci_set_adv_params (hcidev);
		if (ret != 0) {
			error ("setting adv params failed (%d)", ret);
			goto leave;
		}
	}

	/* /\* when enabling advertising, disable discovery  *\/ */
	/* if (enable) */
	/* 	enable_discovery (self, FALSE); */

	if (!hci_set_adv_enable (hcidev, enable)) {
		error ("failed to %sable advertising",
		       enable ? "en" : "dis");
		goto leave;
	}

	ret = hci_set_adv_data (
		hcidev, self->magic,
		btd_adapter_get_name (self->adapter));
	if (ret != 0) {
		error ("setting arc data failed (%d)", ret);
		goto leave;
	}

	rv = TRUE;
leave:
	if (hcidev >= 0)
		hci_close_dev (hcidev);

	if (hcisock >= 0)
		close (hcisock);

	/* when disabling advertising, enable discovery  */
	/* if (!enable) */
	/* 	enable_discovery (self, FALSE); */

	return rv;
}
