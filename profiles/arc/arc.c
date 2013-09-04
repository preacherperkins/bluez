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
	{ ARC_TARGET_ID,  "Target" }
};

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

	g_print ("%s%s\n" ANSI_DEFAULT,
		 col, str);

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
