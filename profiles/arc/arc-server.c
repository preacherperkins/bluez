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

#define CLIENT_TIMEOUT 10 /*seconds*/
#define AUTO_DISCONNECT

static GSList *ARC_SERVERS = NULL;

typedef struct  {
	struct btd_adapter	*adapter;
	uint16_t		 handles[ARC_ID_NUM];
	uint16_t		 val_handles[ARC_ID_NUM];
	GString                  *values[ARC_ID_NUM];

	guint		adv_id, disc_id;
	gboolean	writing_result;

	struct mgmt *mgmt;
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

	btd_adapter_disconnect_device (self->adapter,
				       device_get_address(device),
				       BDADDR_LE_PUBLIC);
}


static void
on_disconnected (uint16_t index, uint16_t length,
		 const void *param, ARCServer *self)
{
	/* make sure it's really disconnected */
	btd_adapter_for_each_device (self->adapter,
				     each_device_disconnect, self);

	/* re-enable advertising, since the connection turned it off */
	enable_adv (self,TRUE);
}



static void
on_connected (uint16_t index, uint16_t length,
		 const void *param, ARCServer *self)
{
	DBG ("%s", __FUNCTION__);
}


ARCServer*
arc_server_new (struct btd_adapter *adapter)
{
	ARCServer	*self;
	unsigned	 u;

	self		 = g_new0 (ARCServer, 1);
	self->adapter = adapter;

	for (u = 0; u != ARC_ID_NUM; ++u)
		self->values[u] = g_string_sized_new (20);

	/*
	 * we need the mgmt interface to be able to get the
	 * connected/disconnected callbacks
	 */
	self->mgmt = mgmt_new_default ();
	mgmt_register(self->mgmt, MGMT_EV_DEVICE_DISCONNECTED,
		      btd_adapter_get_index (adapter),
		      (mgmt_notify_func_t)on_disconnected, self, NULL);

	mgmt_register(self->mgmt, MGMT_EV_DEVICE_CONNECTED,
		      btd_adapter_get_index (adapter),
		      (mgmt_notify_func_t)on_connected, self, NULL);

	return self;
}

static void
arc_server_destroy (ARCServer *self)
{
	GSList		*cur;
	unsigned	 u;

	if (!self)
		return;

	for (u = 0; u != ARC_ID_NUM; ++u)
		g_string_free (self->values[u], TRUE);

	ARC_SERVERS = g_slist_remove (ARC_SERVERS, self);

	if (self->adv_id != 0) {
		g_source_remove (self->adv_id);
		self->adv_id = 0;
	}

	mgmt_unref (self->mgmt);
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
	self = find_arc_server (adapter);

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
	/* DBG ("hci adv data"); */
	/* dump_bytes ((uint8_t*)&adv_params_cp, sizeof(adv_params_cp)); */

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
hci_set_adv_data (int hcidev, const char *name)
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

	/* the local name to advertise */
	if (name) {
		unsigned strsize;
		strsize = MIN(strlen(name),
			      LE_SET_ADVERTISING_DATA_CP_SIZE - offset - 3);
		advdata_cp.data[offset + 0] = strsize + 1;
		advdata_cp.data[offset + 1] = 0x09;	/* local name */
		memcpy (&advdata_cp.data[offset + 2], name, strsize);
		offset += strsize + 2;
	}

	advdata_cp.length  = offset;

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




static void
handle_blob (ARCServer *self, struct attribute *attr,
	     struct btd_device *device, ARCID id)
{
	if (id == ARC_REQUEST_ID) {

		int		 ret;
		char		*str;
		const char	*objpath;
		const char	*empty;

		objpath = device_get_path (device);

		/* when processing the current method, clear any
		 * existing result */
		DBG ("clearing old results");
		ret = attrib_db_update (
			self->adapter,
			self->val_handles[ARC_RESULT_ID],
			NULL,
			(uint8_t*)NULL,
			0,
			NULL);

		if (ret != 0)
			error ("failed to write attrib");

		DBG ("emitting method-called");
		g_dbus_emit_signal (
			btd_get_dbus_connection(),
			adapter_get_path (self->adapter),
			ARC_SERVER_IFACE, "MethodCalled",
			DBUS_TYPE_OBJECT_PATH, &objpath,
			DBUS_TYPE_STRING,
			&self->values[ARC_REQUEST_ID]->str,
			DBUS_TYPE_INVALID);
	}
}

static gboolean
do_disconnect (ARCServer *self)
{
#ifdef AUTO_DISCONNECT
	DBG ("automatically disconnecting");
	g_timeout_add (1000, (GSourceFunc)do_enable_adv, self);
	btd_adapter_for_each_device (self->adapter,
				     each_device_disconnect, self);
#endif /*AUIO_DISCONNECT*/
	return FALSE;
}



/* when AUTO_DISCONNECT is defined (see do_disconnect), disconnect
 * clients after a certain inactive period; this is needed to ensure
 * the device is able to service other devices too
 */
static void
update_disconnect_timeouts (ARCServer *self, struct btd_device *device)
{
	if (self->disc_id != 0)
		g_source_remove (self->disc_id);

	self->disc_id  =
		g_timeout_add_seconds (
			CLIENT_TIMEOUT,
			(GSourceFunc)do_disconnect, self);
}



static ARCID
find_id_for_attribute (ARCServer *self, struct attribute* attr)
{
	unsigned u;

	for (u = 0; u != ARC_ID_NUM; ++u) {
		if (attr->handle == self->val_handles[u])
			return u;
	}

	return u;
}


static uint8_t
on_attr_write (struct attribute *attr, struct btd_device *device,
	       ARCServer *self)
{
	ARCID		 id;
	GString		*gstr;
	char		*str, *s;

	DBG ("writing handle 0x%04x", attr->handle);
	update_disconnect_timeouts (self, device);

	id = find_id_for_attribute (self, attr);
	if (id == ARC_ID_NUM) {
		error ("unknown handle");
		return 0;
	}

	/* if we see 0xfe, we start from scratch;
	 * otherwise, accumulate until we see 0xff*/
	str = g_strndup ((const char*)attr->data, attr->len);

	for (s = str; *s; ++s) {
		switch ((unsigned char)*s) {
		case 0xfe:
			g_string_truncate (self->values[id], 0);
			break;
		case 0xff:
			handle_blob (self, attr, device, id);
			g_string_truncate (self->values[id], 0);
			break;
		default:
			g_string_append_c (self->values[id], *s);
			break;
		}
	}
	g_free (str);

	return 0;
}


static uint8_t
on_attr_read (struct attribute	*attr,
	      struct btd_device *device, ARCServer *self)
{
	ARCID	id;
	GString *gstr;
	char	 str[ATT_MAX_VALUE_LEN];
	size_t	 len;

	id = find_id_for_attribute (self, attr);
	if (id == ARC_ID_NUM) {
		error ("unknown handle");
		return 0;
	}

	gstr = self->values[id];
	if (!gstr)
		return 0;

	len  = MIN (sizeof(gstr->str) - 2, gstr->len);
	if (len == 0)
		return 0;

	if (!self->writing_result) {
		str[0] = 0xfe;
		memcpy (str + 1, gstr->str, len - 1);
		g_string_erase (gstr, 0, len  - 1);
		self->writing_result = TRUE;
	} else {
		memcpy (str, gstr->str, len);
		g_string_erase (gstr, 0, len);
	}

	if (gstr->len == 0) {
		str[len] = 0xff;
		self->writing_result = FALSE;
	}

	attr->data = (uint8_t*)str;
	attr->len  = len;

	DBG ("reading handle 0x%04x", attr->handle);
	update_disconnect_timeouts (self, device);

	return 0;
}


static gboolean
register_service (ARCServer *self)
{
	bt_uuid_t	uuid[ARC_ID_NUM], srv_uuid;
	gboolean	rv;
	unsigned	u;

	DBG ("%s", __FUNCTION__);

	bt_string_to_uuid (&srv_uuid, ARC_SERVICE_UUID);

 	bt_string_to_uuid (&uuid[ARC_REQUEST_ID], ARC_REQUEST_UUID);
	bt_string_to_uuid (&uuid[ARC_RESULT_ID], ARC_RESULT_UUID);
	bt_string_to_uuid (&uuid[ARC_EVENT_ID],  ARC_EVENT_UUID);
	bt_string_to_uuid (&uuid[ARC_TARGET_ID], ARC_TARGET_UUID);
	bt_string_to_uuid (&uuid[ARC_DEVNAME_ID], ARC_DEVNAME_UUID);

	rv =  gatt_service_add (
		self->adapter, GATT_PRIM_SVC_UUID, &srv_uuid,

		/*
		 * This gets the request from clients (ie., Json-blobs)
		 * */
		GATT_OPT_CHR_UUID, &uuid[ARC_REQUEST_ID],
		GATT_OPT_CHR_PROPS, ATT_CHAR_PROPER_WRITE | ATT_CHAR_PROPER_READ,
		GATT_OPT_CHR_VALUE_CB, ATTRIB_WRITE, on_attr_write, self,
		GATT_OPT_CHR_VALUE_CB, ATTRIB_READ, on_attr_read, self,
		GATT_OPT_CHR_VALUE_GET_HANDLE, &self->val_handles[ARC_REQUEST_ID],

		/*
		 * This get the results of request (Json blobs)
		 */
		GATT_OPT_CHR_UUID, &uuid[ARC_RESULT_ID],
		GATT_OPT_CHR_PROPS, ATT_CHAR_PROPER_READ,
		GATT_OPT_CHR_VALUE_CB, ATTRIB_READ, on_attr_read, self,
		GATT_OPT_CHR_VALUE_GET_HANDLE, &self->val_handles[ARC_RESULT_ID],

		/*
		 * This get events (ie., data for /all/ clients
		 */
		GATT_OPT_CHR_UUID, &uuid[ARC_EVENT_ID],
		GATT_OPT_CHR_PROPS, ATT_CHAR_PROPER_READ | ATT_CHAR_PROPER_WRITE,
		GATT_OPT_CHR_VALUE_CB, ATTRIB_WRITE, on_attr_write, self,
		GATT_OPT_CHR_VALUE_CB, ATTRIB_READ, on_attr_read, self,
		GATT_OPT_CHR_VALUE_GET_HANDLE, &self->val_handles[ARC_EVENT_ID],

		/*
		 * It seems that sometimes iOS-clients don't see the alias/name
		 * so we store it as a characteristic as well.
		 *
		 */
		GATT_OPT_CHR_UUID, &uuid[ARC_DEVNAME_ID],
		GATT_OPT_CHR_PROPS, ATT_CHAR_PROPER_READ,
		GATT_OPT_CHR_VALUE_CB, ATTRIB_READ, on_attr_read, self,
		GATT_OPT_CHR_VALUE_GET_HANDLE, &self->val_handles[ARC_DEVNAME_ID],


		GATT_OPT_CHR_UUID, &uuid[ARC_TARGET_ID],
		GATT_OPT_CHR_PROPS, ATT_CHAR_PROPER_READ,
		GATT_OPT_CHR_VALUE_CB, ATTRIB_READ, on_attr_read, self,
		GATT_OPT_CHR_VALUE_GET_HANDLE, &self->val_handles[ARC_TARGET_ID],

		GATT_OPT_INVALID);

	return rv;
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
chunked_attrib_db_update (ARCServer *self, GString *value, ARCID id)
{
	int		 ret;
	const unsigned	 chunksize = 20;
	GString		*gstr;
	char		*s;

	/* wrap in 0xfe <string> 0xff */
	gstr = g_string_new (value->str);
	g_string_prepend_c (gstr, 0xfe);
	g_string_append_c (gstr, 0xff);

	/* send in bufsize blobs */
	s   = gstr->str;
	ret = 0;

	for (;;) {
		size_t	size;
		int	ret;

		size = MIN(strlen(s), chunksize);

		ret = attrib_db_update (
			self->adapter,
			self->val_handles[id],
			NULL,
			(uint8_t*)s,
			size,
			NULL);

		if (ret != 0) {
			error ("failed to write attrib");
			break;
		}

		DBG ("written chunk (%u octets)", size);

		if (size < chunksize)
			break; /* we're done */

		s += size;
	}

	g_string_free (gstr, TRUE);

	return ret;
}




static DBusMessage*
emit_event_method (DBusConnection *conn, DBusMessage *msg, ARCServer *self)
{
	DBusMessage		*reply;
	const char		*event;
	gboolean		 rv;
	int			 ret;
	struct btd_device	*device;

	rv = dbus_message_get_args (msg, NULL,
				    DBUS_TYPE_STRING, &event,
				    DBUS_TYPE_INVALID);
	if (!rv)
		return btd_error_invalid_args (msg);

	/* server data */
	g_string_assign (self->values[ARC_EVENT_ID], event);

	ret = chunked_attrib_db_update (self, self->values[ARC_EVENT_ID],
					ARC_EVENT_ID);
	if (ret != 0)
		DBG ("error writing event to GATT: %s", strerror(-ret));

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
	const char		*target_path, *results;
	gboolean		 rv;
	int			 ret;
	const bdaddr_t*		 target_addr;
	char			*target;
	struct btd_device	*device;

	DBG ("%s", __FUNCTION__);

	rv     = dbus_message_get_args (msg, NULL,
					DBUS_TYPE_OBJECT_PATH, &target_path,
					DBUS_TYPE_STRING, &results,
					DBUS_TYPE_INVALID);
	if (!rv)
		return btd_error_invalid_args (msg);

	device = find_device_for_object_path (self, target_path);
	if (!device)
 		return btd_error_failed (msg, "could not find target");

	DBG ("SUBMITTING RESULT '%s'", self->values[ARC_RESULT_ID]->str);

	ret = attrib_db_update (
		self->adapter,
		self->val_handles[ARC_RESULT_ID],
		NULL,
		(uint8_t*)self->values[ARC_RESULT_ID]->str,
		self->values[ARC_RESULT_ID]->len,
		NULL);

	/* target = batostr (target_addr); */
	/* g_string_assign (self->values[ARC_TARGET_ID], target); */
	/* bt_free (target); */

	/* ret = chunked_attrib_db_update (self, self->values[ARC_TARGET_ID], */
	/* 				ARC_TARGET_ID); */

	if (ret != 0)
		return btd_error_failed (msg, "gatt update failed (result)");

	/* notify_devices (self, ARC_RESULT_ID); */

	if (!(reply = dbus_message_new_method_return (msg)))
		return btd_error_failed (msg, "error creating DBus reply");

	/* notify_devices (self, ARC_TARGET_ID); */

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
update_name_method (DBusConnection *conn, DBusMessage *msg, ARCServer *self)
{
	DBusMessage	*reply;
	const char	*name;
	gboolean	 rv;
	int		 ret;

	rv     = dbus_message_get_args (msg, NULL,
					DBUS_TYPE_STRING, &name,
					DBUS_TYPE_INVALID);
	if (!rv)
		return btd_error_invalid_args (msg);

	if (adapter_set_name (self->adapter, name) != 0)
		return btd_error_failed (msg, "updating adapter name failed");

	/* server */
	g_string_assign (self->values[ARC_DEVNAME_ID], name);
	ret = chunked_attrib_db_update (self, self->values[ARC_DEVNAME_ID],
					ARC_DEVNAME_ID);
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
	else
		dbus_message_append_args(reply, DBUS_TYPE_INVALID);

	return reply;
}


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

	self	  = arc_server_new (adapter);

	register_service (self);

	g_dbus_register_interface (btd_get_dbus_connection(),
				   adapter_get_path (adapter),
				   ARC_SERVER_IFACE,
				   ARC_SERVER_METHODS,
				   ARC_SERVER_SIGNALS,
				   NULL, /* properties */
				   self,
				   NULL);

	ARC_SERVERS = g_slist_prepend (ARC_SERVERS, self);

	return 0;
}

void
arc_remove_server (struct btd_profile *profile, struct btd_adapter *adapter)
{
	GSList		*cur;
	ARCServer	*self;

	self = find_arc_server (adapter);
	if (!self)
		return;

	g_dbus_unregister_interface(btd_get_dbus_connection(),
				    adapter_get_path (adapter),
				    ARC_SERVER_IFACE);

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
	int			hcidev;
	struct hci_dev_info	devinfo;

	devinfo.dev_id = btd_adapter_get_index (self->adapter);
	if (devinfo.dev_id == MGMT_INDEX_NONE) {
		error ("can't get adapter index");
		return -1;
	}

	if (ioctl (hcisock, HCIGETDEVINFO, (void*)&devinfo) != 0) {
		error ("can't get hci socket");
		return -1;
	}

	if (hci_test_bit (HCI_RAW, &devinfo.flags) &&
	    bacmp(&devinfo.bdaddr, BDADDR_ANY) == 0) {
		int fd;
		fd = hci_open_dev (devinfo.dev_id);
		hci_read_bd_addr(fd, &devinfo.bdaddr, 1000);
		hci_close_dev(fd);
	}

	hcidev = hci_open_dev (devinfo.dev_id);
	if (hcidev < 0) {
		error ("failed to open hci device");
		return -1;
	}

	return hcidev;
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
	gboolean				 rv;
	struct hci_dev_info			 devinfo;
	struct hci_request			 rq;
	int					 hcisock, hcidev, ret;

	rv     = FALSE;
	hcidev = hcisock = -1;

	DBG ("attempt to %sable advertising", enable ? "en" : "dis");

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
		if (ret < 0) {
			error ("setting adv params failed (%d)", ret);
			goto leave;
		}
	}

	if (!hci_set_adv_enable (hcidev, enable)) {
		error ("failed to %sable advertising",
		       enable ? "en" : "dis");
		goto leave;
	}

	/* it seems we need to set this each time after enabling
	 * advertising... */
	ret = hci_set_adv_data (hcidev, btd_adapter_get_name (self->adapter));
	if (ret < 0) {
		error ("setting arc data failed (%d)", ret);
		goto leave;
	}


	rv = TRUE;
leave:
	if (hcidev >= 0)
		hci_close_dev (hcidev);

	if (hcisock >= 0)
		close (hcisock);

	return rv;
}
