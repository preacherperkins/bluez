/*-*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
**
**  Author: Dirk-Jan C. Binnema <dirk@morseproject.com>
**
** Copyright (c) 2013 Morse Project. All rights reserved.
**
** @file
*/

#include "config.h"
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <dirent.h>

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

#include "lib/uuid.h"
#include "lib/mgmt.h"
#include "src/shared/mgmt.h"


#include "arc.h"

static struct {
	ARCID		 id;
	const char	*prop;
} ARC_PROPS[] = {
	{ ARC_EVENT_ID,   "Event" },
	{ ARC_RESULT_ID,  "Result" },
	{ ARC_REQUEST_ID, "Request" },
	{ ARC_TARGET_ID,  "Target" },
	{ ARC_JID_ID,     "JID" },
	{ ARC_DEVNAME_ID, "DeviceName" },
};



static void
free_arch_char (ARCChar *achar)
{
	if (!achar)
		return;

	DBG ("freeing %s", achar->name);

	g_free (achar->name);
	g_free (achar->uuidstr);

	if (achar->val)
		g_byte_array_unref (achar->val);
	if (achar->val_scratch)
		g_byte_array_unref (achar->val_scratch);

	g_free (achar);
}


void
arc_char_init_scratch (ARCChar *achar, gboolean copy)
{
	/* remove old garbage */
	if (achar->val_scratch->len)
		g_byte_array_remove_range (
			achar->val_scratch, 0, achar->val_scratch->len);

	/* copy from val */
	if (copy && achar->val->len)
		g_byte_array_append (achar->val_scratch,
				     achar->val->data, achar->val->len);
}




GHashTable*
arc_char_table_new (void)
{
	GHashTable *char_table;

	char_table = g_hash_table_new_full (
		g_str_hash, g_str_equal,
		NULL, (GDestroyNotify)free_arch_char);

	/* my characteristics */
	arc_char_table_add_char (
		char_table,
		ARC_REQUEST_UUID, "Request",
		ARC_CHAR_FLAG_READABLE | ARC_CHAR_FLAG_WRITABLE);
	arc_char_table_add_char (
		char_table,
		ARC_RESULT_UUID, "Result",
		ARC_CHAR_FLAG_READABLE);
	arc_char_table_add_char (
		char_table,
		ARC_EVENT_UUID, "Event",
		ARC_CHAR_FLAG_READABLE | ARC_CHAR_FLAG_WRITABLE);
	arc_char_table_add_char (
		char_table,
		ARC_DEVNAME_UUID, "DeviceName",
		ARC_CHAR_FLAG_READABLE);
	arc_char_table_add_char (
		char_table,
		ARC_JID_UUID, "JID",
		ARC_CHAR_FLAG_READABLE | ARC_CHAR_FLAG_WRITABLE);

	return char_table;
}


ARCChar*
arc_char_table_add_char (GHashTable *table,
			 const char *uuidstr, const char *name,
			 ARCCharFlags flags)
{
	ARCChar		*achar;

	g_return_val_if_fail (table, NULL);
	g_return_val_if_fail (uuidstr, NULL);
	g_return_val_if_fail (name, NULL);

	achar		   = g_new0 (ARCChar, 1);
	achar->name	   = g_strdup (name);
	achar->uuidstr	   = g_strdup (uuidstr);
	achar->val	   = g_byte_array_new ();
	achar->val_scratch = g_byte_array_new ();
	achar->flags	   = flags;

	bt_string_to_uuid (&achar->uuid, achar->uuidstr);

	achar->gatt_props = 0;
	if (achar->flags & ARC_CHAR_FLAG_READABLE)
		achar->gatt_props |= ATT_CHAR_PROPER_READ;
	if (achar->flags & ARC_CHAR_FLAG_WRITABLE)
		achar->gatt_props |= ATT_CHAR_PROPER_WRITE;

	g_hash_table_insert (table, achar->uuidstr, achar);

	return achar;
}


ARCChar*
arc_char_table_find_by_uuid (GHashTable *table, const char *uuid)
{
	g_return_val_if_fail (table, NULL);
	g_return_val_if_fail (uuid, NULL);

 	return (ARCChar*)g_hash_table_lookup (table, uuid);
}


ARCChar*
arc_char_table_find_by_attr (GHashTable *table,
				  struct attribute* attr)
{
	GHashTableIter	 iter;
	const char	*uuidstr;
	ARCChar		*achar;

	g_return_val_if_fail (table, NULL);
	g_return_val_if_fail (attr, NULL);

	g_hash_table_iter_init (&iter, table);
	while (g_hash_table_iter_next (&iter, (gpointer)&uuidstr,
				       (gpointer)&achar))
		if (attr->handle == achar->val_handle)
			return achar;

	return NULL;
}

ARCChar*
arc_char_table_find_by_name (GHashTable *table, const char *name)
{
	GHashTableIter	 iter;
	const char	*uuidstr;
	ARCChar		*achar;

	g_return_val_if_fail (table, NULL);
	g_return_val_if_fail (name, NULL);

	g_hash_table_iter_init (&iter, table);
	while (g_hash_table_iter_next (&iter, (gpointer)&uuidstr,
				       (gpointer)&achar))
		if (g_strcmp0 (name, achar->name) == 0)
			return achar;

	return NULL;
}


void
arc_char_table_clear_working_data (GHashTable *table)
{
	GHashTableIter	 iter;
	const char	*uuidstr;
	ARCChar		*achar;

	g_return_if_fail (table);

	g_hash_table_iter_init (&iter, table);
	while (g_hash_table_iter_next (&iter, (gpointer)&uuidstr,
				       (gpointer)&achar)) {
		arc_char_init_scratch (achar, FALSE/*don't copy*/);
		achar->writing = FALSE;
	}
}




void
arc_char_set_value_string (ARCChar *achar, const char *str)
{
	g_return_if_fail (achar);

	if (achar->val->len > 0)
		g_byte_array_remove_range (achar->val, 0,
					   achar->val->len);
	if (str)
		g_byte_array_append (achar->val, (const guint8*)str,
				     strlen (str));
}

char*
arc_char_get_value_string (ARCChar *achar)
{
	g_return_val_if_fail (achar, NULL);

	if (achar->val->len == 0)
		return NULL;

	return g_strndup ((const char*)achar->val->data,
			  achar->val->len);
}




const char*
arc_id_to_prop (ARCID id)
{
	unsigned u;

	for (u = 0; u != G_N_ELEMENTS(ARC_PROPS); ++u)
		if (ARC_PROPS[u].id == id)
			return ARC_PROPS[u].prop;

	return NULL;
}


ARCID
arc_prop_to_id (const char *prop)
{
	unsigned u;

	for (u = 0; u != G_N_ELEMENTS(ARC_PROPS); ++u)
		if (g_strcmp0 (ARC_PROPS[u].prop, prop) == 0)
			return ARC_PROPS[u].id;

	return ARC_ID_NUM;
}




void
arc_log (const char *col, const char *frm, ...)
{
	va_list	 args;
	gchar	*str;
	gboolean tty;

	if (!frm)
		return;

	tty = isatty(fileno(stdout)) ? TRUE : FALSE;

	va_start (args, frm);
	str = g_strdup_vprintf (frm, args);
	va_end (args);

	g_print ("%s%s" ANSI_DEFAULT "\n", col, str);
	g_free (str);
}


void
arc_dump_bytes (uint8_t *bytes, size_t len)
{
	unsigned u;

	for (u = 0; u !=len; ++u)
		g_print ("%02X ", bytes[u]);

	g_print ("(%d)\n", (int)len);
}



static int
hci_enable_adv (int hcidev, gboolean enable)
{
	struct hci_request			rq;
	le_set_advertise_enable_cp		advertise_cp;
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
 	memset(&rq, 0, sizeof(rq));
	rq.ogf	  = OGF_LE_CTL;
	rq.ocf	  = OCF_LE_SET_ADVERTISING_PARAMETERS;
	rq.cparam = &adv_params_cp;
	rq.clen	  = LE_SET_ADVERTISING_PARAMETERS_CP_SIZE;
	rq.rparam = &status;
	rq.rlen	  = 1;

	ret = hci_send_req (hcidev, &rq, 1000);
	if (ret != 0) {
		error ("setting adv params failed: %s", strerror(errno));
		return ret;
	}

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
	if (ret != 0) {
		error ("enable adv failed: %s", strerror(errno));
		return ret;
	}

	return 0;
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
	arc_dump_bytes ((uint8_t*)&advdata_cp, sizeof(advdata_cp));

	memset (&rq, 0, sizeof(rq));
	rq.ogf	  = OGF_LE_CTL;
	rq.ocf	  = OCF_LE_SET_ADVERTISING_DATA;
	rq.cparam = &advdata_cp;
	rq.clen	  = sizeof (advdata_cp);
	rq.rparam = &status;
	rq.rlen	  = 1;

	return hci_send_req (hcidev, &rq, 1000);
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
get_hci_device (struct btd_adapter *adapter, int hcisock)
{
	int			hcidev, fd;
	struct hci_dev_info	devinfo;

	devinfo.dev_id = btd_adapter_get_index (adapter);
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


/**
 * Enable/Disable advertising using the HCI-interface
 *
 * @param adapter a bluetooth adapter
 * @param magic the magic byte to set
 * @param enable if TRUE, enable advertising, otherwise disable it
 *
 * @return TRUE if enabling or disabling worked, FALSE otherwise
 */
gboolean
arc_enable_advertising (struct btd_adapter *adapter, uint8_t magic,
			gboolean enable)
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

	hcidev = get_hci_device (adapter, hcisock);
	if (hcidev < 0)
		goto leave;

	if (hci_enable_adv (hcidev, enable) != 0) {
		error ("failed to %sable advertising",
		       enable ? "en" : "dis");
		goto leave;
	}

	ret = hci_set_adv_data (hcidev, magic,
		btd_adapter_get_name (adapter));
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

	return rv;
}


static struct btd_profile
ARC_GATT_PROFILE = {

	.name		= "Morse-ARC GATT Profile",
	.remote_uuid    = GATT_UUID,

	.adapter_probe	= arc_probe_server,
	.adapter_remove	= arc_remove_server
};


static struct btd_profile
ARC_CMD_PROFILE = {

	.name	       = "Morse-ARC Command Profile",
	.remote_uuid   = ARC_SERVICE_UUID,

	.device_probe  = arc_probe_proxy,
	.device_remove = arc_remove_proxy
};

static int
arc_init (void)
{
	int ret;

	ret = btd_profile_register (&ARC_GATT_PROFILE);
	if (ret < 0)
		return ret;

	ret = btd_profile_register (&ARC_CMD_PROFILE);
	if (ret < 0)
		return ret;

	return 0;
}

static void
arc_uninit (void)
{
	btd_profile_unregister(&ARC_GATT_PROFILE);
	btd_profile_unregister(&ARC_CMD_PROFILE);
}

BLUETOOTH_PLUGIN_DEFINE(arc, VERSION,
			BLUETOOTH_PLUGIN_PRIORITY_DEFAULT,
			arc_init, arc_uninit)
