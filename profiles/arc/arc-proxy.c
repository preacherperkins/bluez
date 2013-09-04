/*-*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
**
**  Author: Dirk-Jan C. Binnema <dirk@morseproject.com>
**
** Copyright (c) 2013 Morse Project. All rights reserved.
**
** @file
*/

#include "config.h"
#include <unistd.h>

#include "lib/uuid.h"
#include "plugin.h"
#include "gdbus/gdbus.h"
#include "dbus-common.h"
#include "attrib/att.h"
#include "adapter.h"
#include "device.h"
#include "attrib/att-database.h"
#include "log.h"
#include "attrib/gatt-service.h"
#include "attrib/gattrib.h"
#include "attrib-server.h"
#include "attrib/gatt.h"
#include "profile.h"
#include "error.h"
#include "textfile.h"
#include "attio.h"
#include "service.h"

#include "arc.h"

/* ARCProxys contain device-specific data */
typedef struct {

	struct btd_adapter	*adapter;
	struct btd_device	*device;
	struct att_range	*svc_range;

	char*	values[ARC_ID_NUM];

	uint16_t	handles[ARC_ID_NUM];
	uint16_t	val_handles[ARC_ID_NUM];
	uint16_t	ccc_handles[ARC_ID_NUM];

	int             not_ids[ARC_ID_NUM]; /* notifications */

	int	 attio_id;
	GAttrib *attrib;
} ARCProxy;

static GSList *ARC_BLOBS = NULL;


static ARCProxy*
find_arc_blob (struct btd_device *device)
{
	GSList *cur;

	for (cur = ARC_BLOBS; cur; cur = g_slist_next (cur)) {
		ARCProxy *blob;
		blob = (ARCProxy*)cur->data;

		if (blob->device == device)
			return blob;
	}

	return NULL;
}


typedef struct {
	ARCProxy	*aproxy;
	ARCID		 id;
	char		*prop;
	gboolean	 dbus_notify;
} ReadData;

static ReadData *
read_data_new (ARCProxy *aproxy, ARCID id, const char *prop, gboolean dbus_notify)
{
	ReadData *rdata;

	g_return_val_if_fail (aproxy, NULL);
	g_return_val_if_fail (id < ARC_ID_NUM, NULL);

	rdata		   = g_new0 (ReadData, 1);
	rdata->aproxy	   = aproxy;
	rdata->id	   = id;
	rdata->dbus_notify = dbus_notify;

	if (prop)
		rdata->prop  = g_strdup (prop);

	return rdata;
}

static void
read_data_destroy (ReadData *rdata)
{
	if (!rdata)
		return;

	g_free (rdata->prop);
	g_free (rdata);

}

static void
on_read_string (guint8 status, const guint8 *pdu,
		guint16 len, ReadData *rdata)
{
	if (status != 0) {
		error("failure reading %d (%s) from gatt: %s",
		      rdata->id, rdata->prop ? rdata->prop : "<none>",
		      att_ecode2str (status));
		g_free (rdata);
		return;
	}

	g_free (rdata->aproxy->values[rdata->id]);
	if (len > 0)
		rdata->aproxy->values[rdata->id] = 	/* skip the first byte */
			g_strndup ((const char*)&pdu[1], len - 1);
	else
		rdata->aproxy->values[rdata->id] = NULL;

	if (rdata->dbus_notify && rdata->aproxy->values[rdata->id] && rdata->prop)
		g_dbus_emit_property_changed (
			btd_get_dbus_connection (),
			device_get_path (rdata->aproxy->device),
			ARC_PROXY_IFACE,
			rdata->prop);
}


static void
on_read_btaddr (guint8 status, const guint8 *pdu, guint16 len,
		ReadData *rdata)
{
	bdaddr_t	target_addr;
	guint		rv;
	char		s1[20], s2[20];

	DBG ("%s: %d", __FUNCTION__,__LINE__);

	if (status != 0) {
		error("failure reading %d (%s) from gatt: %s",
		      rdata->id, rdata->prop ? rdata->prop : "<none>",
		      att_ecode2str (status));
		goto leave;
	}

	if (len != sizeof(bdaddr_t) + 1) {
		/* when the target is not set yet... */

		g_free (rdata->aproxy->values[rdata->id]);
		rdata->aproxy->values[rdata->id] = g_strdup ("");

		goto leave;
	}

	DBG ("%s: %d", __FUNCTION__,__LINE__);

	/* skip first bytes */
	ba2str ((bdaddr_t*)&pdu[1], s1);
	ba2str (device_get_address(rdata->aproxy->device), s2);

	g_free (rdata->aproxy->values[rdata->id]);
	rdata->aproxy->values[rdata->id] = g_strdup (s2);

leave:
	g_free (rdata);
}


/*
 * this is called when the TARGET characteristic changes on the server
 * side. The notification contains a bluetooth-id, and if that id
 * matches our bluetooth-id, there's a new RESULT available for us.
 *
 * sadly, this does not work, currently.
 */
static void
on_notify_target (const uint8_t *pdu, uint16_t len, ARCProxy *aproxy)
{
	char		*addrstr, *my_addrstr;
	const bdaddr_t	*addr;

	DBG ("%s", __FUNCTION__);

	if (aproxy->val_handles[ARC_RESULT_ID] != 0)
		return; /* not ready yet */

	on_read_btaddr (0, pdu, len,
			read_data_new (aproxy, ARC_TARGET_ID, "Target",
				TRUE));

	my_addrstr = aproxy->values[ARC_TARGET_ID];
	if (!my_addrstr)
		return;

	addr = device_get_address (aproxy->device);
	if (!addr)
		return;

	ba2str ((bdaddr_t*)&pdu[1], addrstr);

	/* is it for us? */
	if (g_ascii_strcasecmp (addrstr, my_addrstr) == 0) {
		/* yes! update the result */
		int rv;
		rv = gatt_read_char (aproxy->attrib,
				     aproxy->val_handles[ARC_RESULT_ID],
				     (GAttribResultFunc)on_read_string,
				     read_data_new (aproxy, ARC_RESULT_ID,
					     "Result", TRUE));
		if (rv == 0)
			error ("reading gatt failed");
	}
}



static void
on_notify_event (const uint8_t *pdu, uint16_t len, ARCProxy *aproxy)
{
	DBG ("%s", __FUNCTION__);

	g_free (aproxy->values[ARC_EVENT_ID]);
	aproxy->values[ARC_EVENT_ID] = g_strndup ((const char*)pdu + 1, len - 1);

	/* tell DBus we got an event result */
	g_dbus_emit_property_changed (btd_get_dbus_connection (),
				      device_get_path (aproxy->device),
				      ARC_PROXY_IFACE,
				      "Event");
}


typedef struct {
	GAttribNotifyFunc	 func;
	ARCProxy			*aproxy;
	ARCID			 id;
	uint16_t		 bits;
} NotifyData;


static void
on_ccc_written (guint8 status, const guint8 *pdu, guint16 len,
	       gpointer user_data)
{
	NotifyData *ndata;

	ndata = (NotifyData*)user_data;

	if (status != 0) {
		error("failure writing ccc to gatt: %s",
		      att_ecode2str (status));
		g_free (ndata);
		return;
	} else
		DBG ("wrote bits to GATT");

	if (ndata->bits & GATT_CLIENT_CHARAC_CFG_NOTIF_BIT)
		ndata->aproxy->not_ids[ndata->id] =
			g_attrib_register (ndata->aproxy->attrib,
					   ATT_OP_HANDLE_NOTIFY,
					   ndata->aproxy->val_handles[ndata->id],
					   ndata->func,
					   ndata->aproxy,
					   NULL);
	g_free (ndata);
}


static void
install_ccc (ARCProxy *aproxy, ARCID id, uint16_t bits,
	     GAttribNotifyFunc func)
{
	NotifyData	*ndata;
	uint8_t		atval[2];

	ndata	    = g_new0 (NotifyData, 1);

	ndata->func  = func;
	ndata->id    = id;
	ndata->aproxy = aproxy;
	ndata->bits  = bits;

	DBG ("setting CCC for handle 0x%04x (%u) to 0x%04x",
	     aproxy->ccc_handles[id],
	     id, bits);

	att_put_u16 (bits, atval);
	gatt_write_char (aproxy->attrib, aproxy->ccc_handles[id],
			 atval, sizeof(atval), on_ccc_written, ndata);
}


typedef struct {
	char		uuid[MAX_LEN_UUID_STR + 1];
	ARCProxy		*aproxy;
} CharX; /* characteristic */


/* static void */
/* process_desc_ccc (CharX *chrx, uint16_t uuid, uint16_t handle) */
/* { */
/* 	uint8_t		 atval[2]; */
/* 	uint16_t	 val; */
/* 	char		*msg; */

/* 	if (uuid != GATT_CLIENT_CHARAC_CFG_UUID) */
/* 		return; */

/* 	if (!chrx->uuid) */
/* 		return; */

/* 	if (g_ascii_strcasecmp (chrx->uuid, ARC_EVENT_UUID) == 0) { */

/* 		chrx->aproxy->ccc_handles[ARC_EVENT_ID] = handle; */
/* 		install_ccc (chrx->aproxy, ARC_EVENT_ID, */
/* 			     GATT_CLIENT_CHARAC_CFG_NOTIF_BIT, */
/* 			     (GAttribNotifyFunc)on_notify_event); */

/* 	}  else if (g_ascii_strcasecmp (chrx->uuid, ARC_TARGET_UUID) == 0) { */

/* 		chrx->aproxy->ccc_handles[ARC_TARGET_ID] = handle; */
/* 		install_ccc (chrx->aproxy, ARC_TARGET_ID, */
/* 			     GATT_CLIENT_CHARAC_CFG_NOTIF_BIT, */
/* 			     (GAttribNotifyFunc)on_notify_target); */
/* 	} */
/* } */


static void
on_write_gatt (guint8 status, const guint8 *pdu, guint16 len,
	       gpointer user_data)
{
	if (status != 0)
		error("failure writing %s to gatt: %s",
		      user_data ? (char*)user_data : "\b",
		      att_ecode2str (status));

	DBG ("written %u byte(s) to gatt", len);
}


static void
on_discover_desc (guint8 status, const guint8 *pdu, guint16 len,
		  CharX *chrx)
{
	struct att_data_list	*dlst;
	uint8_t			 frm;
	int			 i;

	dlst = NULL;

	if (status != 0) {
		error("char disco failed for %s: %s",
		      chrx->uuid, att_ecode2str(status));
		return;
	}

	dlst = dec_find_info_resp (pdu, len, &frm);
	if (!dlst)
		return;

	if (frm != ATT_FIND_INFO_RESP_FMT_16BIT) {
		if (dlst)
			att_data_list_free(dlst);
		return;
	}

	for (i = 0; i < dlst->num; ++i) {

		uint8_t		*val;
		uint16_t	 handle, uuid;

		val    = dlst->data[i];
		handle = att_get_u16 (val);
		uuid   = att_get_u16 (val + 2);

		/* process_desc_ccc (chrx, uuid, handle); */
	}

	if (dlst)
		att_data_list_free(dlst);
}


static void
desc_char_disco (ARCProxy *aproxy, struct gatt_char *chr,
		 uint16_t start, uint16_t end)
{
	CharX	*chrx;

	if (start > end || !aproxy->attrib)
		return;

	chrx	    = g_new0 (CharX, 1);
	chrx->aproxy = aproxy;
	memcpy(chrx->uuid, chr->uuid, sizeof(chr->uuid));

	gatt_discover_char_desc (
		aproxy->attrib,
		start, end,
		(GAttribResultFunc)on_discover_desc,
		chrx);

	g_free(chrx);
}


/* process a characteristic */
static void
process_chr (ARCProxy *aproxy, struct gatt_char *chr, uint16_t start,
	     uint16_t end)
{
	uint8_t atval[2];

	/* not sure if this could happen */
	if (!chr->uuid)
		return;

	DBG ("%s: %d", __FUNCTION__,__LINE__);

	if (g_ascii_strcasecmp (chr->uuid, ARC_RESULT_UUID) == 0) {

		aproxy->handles[ARC_RESULT_ID]	  = chr->handle;
		aproxy->val_handles[ARC_RESULT_ID] = chr->value_handle;

	} else if (g_ascii_strcasecmp (chr->uuid, ARC_REQUEST_UUID) == 0) {

		aproxy->handles[ARC_REQUEST_ID]	   = chr->handle;
		aproxy->val_handles[ARC_REQUEST_ID] = chr->value_handle;

	} else if (g_ascii_strcasecmp (chr->uuid, ARC_EVENT_UUID) == 0) {

		aproxy->handles[ARC_EVENT_ID]	 = chr->handle;
		aproxy->val_handles[ARC_EVENT_ID] = chr->value_handle;
		desc_char_disco (aproxy, chr, start, end);


	/* } else if (g_ascii_strcasecmp (chr->uuid, ARC_TARGET_UUID) == 0) { */

	/* 	aproxy->handles[ARC_TARGET_ID]	  = chr->handle; */
	/* 	aproxy->val_handles[ARC_TARGET_ID] = chr->value_handle; */
	/* 	desc_char_disco (aproxy, chr, start, end); */

	} else
		DBG ("unknown uuid/handle: %s/0x%04x", chr->uuid,
		     chr->handle);
}


static void
on_discover (GSList *chrs, guint8 status, ARCProxy *aproxy)
{
	GSList *cur;

	if (status != 0) {
		error ("%s", att_ecode2str(status));
		return;
	}

	DBG ("%s: %d", __FUNCTION__,__LINE__);

	for (cur = chrs; cur; cur = g_slist_next (cur)) {

		struct gatt_char	*chr;
		struct gatt_char	*chr_next;
		uint16_t		 start, end, tmp;
		CharX			*chrx;

		DBG ("%s: %d", __FUNCTION__,__LINE__);

		chr = (struct gatt_char*)cur->data;
		if (cur->next)
			chr_next = (struct gatt_char*)cur->next->data;
		else
			chr_next = NULL;

		start = chr->value_handle + 1;
		end   = chr_next ? chr_next->handle - 1 :
			aproxy->svc_range->end;

		DBG ("discovering: %s (0x%04x..0x%04x)",
		     chr->uuid, start, end);

		process_chr (aproxy, chr, start, end);
	}
}


static void
on_attio_connected (GAttrib *attrib, ARCProxy *aproxy)
{
	guint id;

	aproxy->attrib = g_attrib_ref (attrib);

	id = gatt_discover_char (aproxy->attrib,
			    aproxy->svc_range->start,
			    aproxy->svc_range->end,
			    NULL,
			    (gatt_cb_t)on_discover,
			    aproxy);

	DBG("%s 0x%04x 0x%04x (%u)", __FUNCTION__,
		aproxy->svc_range->start, aproxy->svc_range->end, id);
}

static void
on_attio_disconnected (ARCProxy *aproxy)
{
	unsigned u;

	if (aproxy->attio_id > 0) {
		g_attrib_unregister(aproxy->attrib, aproxy->attio_id);
		aproxy->attio_id = 0;
	}

	for (u = 0; u != ARC_ID_NUM; ++u) {
		if (aproxy->not_ids[u] > 0)
			g_attrib_unregister(aproxy->attrib, aproxy->not_ids[u]);
		aproxy->not_ids[u] = 0;
	}

	g_attrib_unref (aproxy->attrib);
	aproxy->attrib = NULL;
}


static ARCProxy*
arc_blob_new (struct btd_device *device)
{
	ARCProxy			*aproxy;
	struct gatt_primary	*primary;

	primary = btd_device_get_primary (device, ARC_SERVICE_UUID);
	if (!primary)
		return NULL;

	aproxy = g_new0 (ARCProxy, 1);

	aproxy->device           = btd_device_ref (device);
	aproxy->adapter          =
		btd_adapter_ref (device_get_adapter (device));

	aproxy->svc_range	= g_new0 (struct att_range, 1);
	aproxy->svc_range->start = primary->range.start;
	aproxy->svc_range->end	= primary->range.end;

	aproxy->attio_id = btd_device_add_attio_callback (
		device,
		(attio_connect_cb)on_attio_connected,
		(attio_disconnect_cb)on_attio_disconnected,
		aproxy);

	ARC_BLOBS = g_slist_prepend (ARC_BLOBS, aproxy);

	return aproxy;
}


static void
arc_blob_destroy (ARCProxy *aproxy)
{
	unsigned u;

	if (!aproxy)
		return;

	ARC_BLOBS = g_slist_remove (ARC_BLOBS, aproxy);

	if (aproxy->attio_id != 0)
		btd_device_remove_attio_callback (aproxy->device,
						  aproxy->attio_id);

	btd_device_unref (aproxy->device);
	btd_adapter_unref (aproxy->adapter);

	if (aproxy->attrib)
		g_attrib_unref (aproxy->attrib);

	for (u = 0; u != ARC_ID_NUM; ++u)
		g_free(aproxy->values[u]);

	g_free (aproxy);
}



static int
chunked_gatt_write (ARCProxy *aproxy, const char* str, ARCID id)
{
	const unsigned	 chunksize = 20;
	GString		*gstr;
	char		*s;

	/* wrap in 0xfe <string> 0xff */
	gstr = g_string_new (str);
	g_string_prepend_c (gstr, 0xfe);
	g_string_append_c (gstr, 0xff);

	/* send in bufsize blobs */
	s   = gstr->str;

	for (;;) {
		size_t	size;
		int	ret;

		size = MIN(strlen(s), chunksize);

		ret = gatt_write_char (
			aproxy->attrib,
			aproxy->val_handles[id],
			(uint8_t*)s,
			size,
			(GAttribResultFunc)on_write_gatt,
			"chunked");

		if (ret == 0) {
			error ("failed to write attrib");
			break;
		}

		DBG ("written chunk (%u octets)", size);

		if (size < chunksize)
			break; /* we're done */

		s += size;
	}

	g_string_free (gstr, TRUE);

	return 0;
}



static void
request_set (const GDBusPropertyTable *property, DBusMessageIter *iter,
	     GDBusPendingPropertySet id, ARCProxy *aproxy)
{
	const char *req;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_STRING) {
		g_dbus_pending_property_error(
			id, ERROR_INTERFACE ".InvalidArguments",
			"Invalid request parameter");
		return;
	}

	if (!aproxy->attrib) {
		g_dbus_pending_property_error(
			id, ERROR_INTERFACE ".InvalidArguments",
			"Not ready");
		return;
	}

	dbus_message_iter_get_basic(iter, &req);

	if (aproxy && req) {

		DBG ("writing request attribute 0x%04x (val=0x%04x) (%s)",
		     aproxy->handles[ARC_REQUEST_ID],
		     aproxy->val_handles[ARC_REQUEST_ID],
		     device_get_path (aproxy->device));

		/* gatt_write_char ( */
		/* 	aproxy->attrib, */
		/* 	aproxy->val_handles[ARC_REQUEST_ID], */
		/* 	(uint8_t*)req, */
		/* 	strlen(req), */
		/* 	(GAttribResultFunc)on_write_gatt, */
		/* 	"request-id"); */

		chunked_gatt_write (aproxy, req, ARC_REQUEST_ID);

		g_free (aproxy->values[ARC_REQUEST_ID]);
		aproxy->values[ARC_REQUEST_ID] = g_strdup (req);

		g_dbus_emit_property_changed (btd_get_dbus_connection (),
					      device_get_path (aproxy->device),
					      ARC_PROXY_IFACE, "Request");
	}
}



static gboolean
property_exists (const GDBusPropertyTable *property, ARCProxy *aproxy)
{
	ARCID id;

	id = arc_prop_to_id (property->name);
	if (id == ARC_ID_NUM)
		return FALSE;

	return aproxy->val_handles[id] != 0;
}





static gboolean
property_get_local (const GDBusPropertyTable *property,
		    DBusMessageIter *iter, ARCProxy *aproxy)
{
	ARCID		 id;
	const char	*val;
	int		 rv;

	id = arc_prop_to_id (property->name);
	if (id == ARC_ID_NUM)
		return FALSE;

	if (aproxy->val_handles[id] == 0 ||
	    !aproxy->values[id])
		return FALSE; /* not ready yet */

	dbus_message_iter_append_basic (iter, DBUS_TYPE_STRING,
					&aproxy->values[id]);

	return TRUE;
}



static gboolean
property_get (const GDBusPropertyTable *property,
	      DBusMessageIter *iter, ARCProxy *aproxy)
{
	ARCID		 id;
	const char	*val;
	int		 rv;

	if (!aproxy->attrib) {
		error ("not ready");
		return FALSE; /* not ready */
	}

	id = arc_prop_to_id (property->name);
	if (id == ARC_ID_NUM)
		return FALSE;

	if (aproxy->val_handles[id] == 0)
		return FALSE; /* not ready yet */

	val = aproxy->values[id];
	val = val ? val : "";

	if (!g_utf8_validate (val, -1, NULL)) {
		error ("%s: not valid utf8", __FUNCTION__);
	}

	DBG ("reading %d (0x%04f) from GATT", id,
	     aproxy->val_handles[id]);

	/* schedule an update; this is for debugging */
	rv = gatt_read_char (aproxy->attrib,
			     aproxy->val_handles[id],
			     (GAttribResultFunc)on_read_string,
			     read_data_new (aproxy, id, property->name, FALSE));
	if (rv == 0)
		error ("reading gatt failed");


	dbus_message_iter_append_basic (iter, DBUS_TYPE_STRING, &val);


	return TRUE;
}



static gboolean
target_get  (const GDBusPropertyTable *property,
	     DBusMessageIter *iter, ARCProxy *aproxy)
{
	int		 rv;
	const char	*target;

	if (!property_exists (property, aproxy) || !aproxy->attrib) {
		DBG ("not ready");
		return FALSE;
	}

	target = aproxy->values[ARC_TARGET_ID];
	target = target ? target : "";

	dbus_message_iter_append_basic (iter, DBUS_TYPE_STRING, &target);

	rv = gatt_read_char (aproxy->attrib,
			aproxy->val_handles[ARC_TARGET_ID],
			(GAttribResultFunc)on_read_btaddr,
			read_data_new (aproxy, ARC_TARGET_ID,
				"Target", FALSE));
	if (rv == 0)
		error ("reading gatt failed");

	return TRUE;
}



static const GDBusPropertyTable
PROXY_PROPS[] = {
	{ "Request", "s",
	  (GDBusPropertyGetter)property_get_local,
	  (GDBusPropertySetter)request_set,
	  (GDBusPropertyExists)property_exists
	},
	{ "Result", "s",
	  (GDBusPropertyGetter)property_get,
	  NULL,
	  (GDBusPropertyExists)property_exists

	},
	{ "Event", "s",
	  (GDBusPropertyGetter)property_get,
	  NULL,
	  (GDBusPropertyExists)property_exists
	},

	{}
};


int
arc_probe_proxy (struct btd_service *service)
{
	ARCProxy		*aproxy;
	struct btd_device*	 device;
	struct gatt_primary	*primary;

	DBG ("%s", __FUNCTION__);

	device = btd_service_get_device (service);
	aproxy  = arc_blob_new (device);
	if (!aproxy)
		return -1;

	if (!g_dbus_register_interface(
			btd_get_dbus_connection(),
			device_get_path (device),
			ARC_PROXY_IFACE,
			NULL,
			NULL,
			PROXY_PROPS,
			aproxy,
			NULL)) {
		error ("failed to register %s", ARC_PROXY_IFACE);
		arc_blob_destroy (aproxy);
		return -1;
	}

	DBG ("registered %s on %s", ARC_PROXY_IFACE,
	     device_get_path (device));

	return 0;
}


void
arc_remove_proxy (struct btd_service *service)
{
	struct btd_device*	 device;
	ARCProxy		*aproxy;

	device	= btd_service_get_device (service);
	aproxy   = find_arc_blob (device);

	if (aproxy)
		arc_blob_destroy (aproxy);
}
