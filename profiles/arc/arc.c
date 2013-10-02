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

static struct {
	ARCID		 id;
	const char	*prop;
} ARC_PROPS[] = {
	{ ARC_EVENT_ID,   "Event" },
	{ ARC_RESULT_ID,  "Result" },
	{ ARC_REQUEST_ID, "Request" },
	{ ARC_TARGET_ID,  "Target" },
	{ ARC_JID_ID,     "JID" }
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
